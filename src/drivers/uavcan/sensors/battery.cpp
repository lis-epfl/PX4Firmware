/****************************************************************************
 *
 *   Copyright (c) 2019-2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

#include "battery.hpp"

#include <lib/ecl/geo/geo.h>
#include <px4_defines.h>

const char *const UavcanBatteryBridge::NAME = "battery";

UavcanBatteryBridge::UavcanBatteryBridge(uavcan::INode &node) :
	UavcanCDevSensorBridgeBase("uavcan_battery", "/dev/uavcan/battery", "/dev/battery", ORB_ID(battery_status_multi_pack)),
	ModuleParams(nullptr),
	_sub_battery(node),
	_warning(battery_status_multi_pack_s::BATTERY_WARNING_NONE),
	_last_timestamp(0)
{
}

int
UavcanBatteryBridge::init()
{
	int res = device::CDev::init();

	if (res < 0) {
		return res;
	}

	res = _sub_battery.start(BatteryInfoCbBinder(this, &UavcanBatteryBridge::battery_sub_cb));

	if (res < 0) {
		PX4_ERR("failed to start uavcan sub: %d", res);
		return res;
	}

	// Initialize batteries ID by setting all instance ID to 255
	for (int i = 0; i < battery_status_multi_pack_s::MAX_BATTERY_PACK_COUNT; i++)
	{
		batteries.id[i] = 255;
	}

	return 0;
}

void
UavcanBatteryBridge::battery_sub_cb(const uavcan::ReceivedDataStructure<uavcan::equipment::power::BatteryInfo> &msg)
{
	// Find or assign the index of this node ID.
	uint8_t nodeID = msg.getSrcNodeID().get();
	uint8_t array_index = 255;

	for (int i = 0; i < battery_status_multi_pack_s::MAX_BATTERY_PACK_COUNT; i++)
	{
		// There are no more node further down this array. Assign i index to this node.
		if (batteries.id[i] == 255)
		{
			batteries.id[i] = nodeID;
		}

		// Find the corresponding battery id in the array
		if (batteries.id[i] == nodeID)
		{
			// Break off for loop when found
			array_index = i;
			break;
		}
	}

	// There are no more space in the message for this node ID
	if (array_index == 255)
	{
		return;
	}

	// Transfer CAN message to uORB topic.
	hrt_abstime now = hrt_absolute_time();
	batteries.timestamp = now;

	// decode model instance id
	// TODO: Should we add a check to see if we need to decode every round?

	batteries.cell_count[array_index] = (uint8_t)(((uint32_t)msg.model_instance_id << MDL_ID_CELL_COUNT_FROM_MSB_BIT) >> MDL_ID_TAKE_4_BITS);
	batteries.serial_number[array_index] = (uint16_t)(((uint32_t)msg.model_instance_id << MDL_ID_SERIAL_FROM_MSB_BIT) >> MDL_ID_TAKE_12_BITS);
	batteries.critical_voltage_v[array_index] = (float)(((uint32_t)msg.model_instance_id << MDL_ID_LOW_VOLT_FROM_MSB_BIT) >> MDL_ID_TAKE_8_BITS) / 10.0f;
	batteries.emergency_voltage_v[array_index] = (float)(((uint32_t)msg.model_instance_id << MDL_ID_CRIT_VOLT_FROM_MSB_BIT) >> MDL_ID_TAKE_8_BITS) / 10.0f;

	batteries.voltage_v[array_index] = msg.voltage;
	batteries.current_a[array_index] = msg.current;
	// battery.average_current_a = msg.;

	sumDischarged(now, batteries.current_a[array_index]);
	batteries.discharged_mah[array_index] = _discharged_mah;

	batteries.remaining[array_index] = msg.state_of_charge_pct / 100.0f; // between 0 and 1

	batteries.temperature[array_index] = msg.temperature + CONSTANTS_ABSOLUTE_NULL_CELSIUS; // Kelvin to Celcius

	batteries.source[array_index] = msg.status_flags & uavcan::equipment::power::BatteryInfo::STATUS_FLAG_IN_USE;
	batteries.capacity[array_index] = msg.full_charge_capacity_wh;

	batteries_last_update[array_index] = now;

	// Update monitor connection status
	// TODO: This will only work if at least 1 monitor is connected.
	for (int i = 0; i < battery_status_multi_pack_s::MAX_BATTERY_PACK_COUNT; i++)
	{
		batteries.connected[i] = (batteries_last_update[i] != 0)
					  && (now - batteries_last_update[i] < BATTERY_UPDATE_TIMEOUT_US);
	}

	publish(1, &batteries);
}

void
UavcanBatteryBridge::sumDischarged(hrt_abstime timestamp, float current_a)
{
	// Not a valid measurement
	if (current_a < 0.f) {
		// Because the measurement was invalid we need to stop integration
		// and re-initialize with the next valid measurement
		_last_timestamp = 0;
		return;
	}

	// Ignore first update because we don't know dt.
	if (_last_timestamp != 0) {
		const float dt = (timestamp - _last_timestamp) / 1e6;
		// mAh since last loop: (current[A] * 1000 = [mA]) * (dt[s] / 3600 = [h])
		_discharged_mah_loop = (current_a * 1e3f) * (dt / 3600.f);
		_discharged_mah += _discharged_mah_loop;
	}

	_last_timestamp = timestamp;
}
