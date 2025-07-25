/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <locale>

#include <ftl/enum.h>

#include "../Macros.h"
#include "SensorInputMapper.h"

// Log detailed debug messages about each sensor event notification to the dispatcher.
constexpr bool DEBUG_SENSOR_EVENT_DETAILS = false;

namespace android {

// Mask for the LSB 2nd, 3rd and fourth bits.
constexpr int REPORTING_MODE_MASK = 0xE;
constexpr int REPORTING_MODE_SHIFT = 1;
constexpr float GRAVITY_MS2_UNIT = 9.80665f;
constexpr float DEGREE_RADIAN_UNIT = 0.0174533f;

/* Convert the sensor data from Linux to Android
 * Linux accelerometer unit is per g,  Android unit is m/s^2
 * Linux gyroscope unit is degree/second, Android unit is radians/second
 */
static void convertFromLinuxToAndroid(std::vector<float>& values,
                                      InputDeviceSensorType sensorType) {
    for (size_t i = 0; i < values.size(); i++) {
        switch (sensorType) {
            case InputDeviceSensorType::ACCELEROMETER:
                values[i] *= GRAVITY_MS2_UNIT;
                break;
            case InputDeviceSensorType::GYROSCOPE:
                values[i] *= DEGREE_RADIAN_UNIT;
                break;
            default:
                break;
        }
    }
}

SensorInputMapper::SensorInputMapper(InputDeviceContext& deviceContext,
                                     const InputReaderConfiguration& readerConfig)
      : InputMapper(deviceContext, readerConfig) {}

SensorInputMapper::~SensorInputMapper() {}

uint32_t SensorInputMapper::getSources() const {
    return AINPUT_SOURCE_SENSOR;
}

void SensorInputMapper::parseSensorConfiguration(InputDeviceSensorType sensorType, int32_t absCode,
                                                 int32_t sensorDataIndex, const Axis& axis) {
    auto it = mSensors.find(sensorType);
    if (it == mSensors.end()) {
        Sensor sensor = createSensor(sensorType, axis);
        sensor.dataVec[sensorDataIndex] = absCode;
        mSensors.emplace(sensorType, sensor);
    } else {
        it->second.dataVec[sensorDataIndex] = absCode;
    }
}

void SensorInputMapper::populateDeviceInfo(InputDeviceInfo& info) {
    InputMapper::populateDeviceInfo(info);

    for (const auto& [sensorType, sensor] : mSensors) {
        info.addSensorInfo(sensor.sensorInfo);
        info.setHasSensor(true);
    }
}

void SensorInputMapper::dump(std::string& dump) {
    dump += INDENT2 "Sensor Input Mapper:\n";
    dump += StringPrintf(INDENT3 " isDeviceEnabled %d\n", getDeviceContext().isDeviceEnabled());
    dump += StringPrintf(INDENT3 " mHasHardwareTimestamp %d\n", mHasHardwareTimestamp);
    dump += INDENT3 "Sensors:\n";
    for (const auto& [sensorType, sensor] : mSensors) {
        dump += StringPrintf(INDENT4 "%s\n", ftl::enum_string(sensorType).c_str());
        dump += StringPrintf(INDENT5 "enabled: %d\n", sensor.enabled);
        dump += StringPrintf(INDENT5 "samplingPeriod: %lld\n", sensor.samplingPeriod.count());
        dump += StringPrintf(INDENT5 "maxBatchReportLatency: %lld\n",
                             sensor.maxBatchReportLatency.count());
        dump += StringPrintf(INDENT5 "maxRange: %f\n", sensor.sensorInfo.maxRange);
        dump += StringPrintf(INDENT5 "power: %f\n", sensor.sensorInfo.power);
        for (ssize_t i = 0; i < SENSOR_VEC_LEN; i++) {
            int32_t rawAxis = sensor.dataVec[i];
            dump += StringPrintf(INDENT5 "[%zd]: rawAxis: %d \n", i, rawAxis);
            const auto it = mAxes.find(rawAxis);
            if (it != mAxes.end()) {
                const Axis& axis = it->second;
                dump += StringPrintf(INDENT5 " min=%0.5f, max=%0.5f, flat=%0.5f, fuzz=%0.5f,"
                                             "resolution=%0.5f\n",
                                     axis.min, axis.max, axis.flat, axis.fuzz, axis.resolution);
                dump += StringPrintf(INDENT5 "  scale=%0.5f, offset=%0.5f\n", axis.scale,
                                     axis.offset);
                dump += StringPrintf(INDENT5 " rawMin=%d, rawMax=%d, "
                                             "rawFlat=%d, rawFuzz=%d, rawResolution=%d\n",
                                     axis.rawAxisInfo.minValue, axis.rawAxisInfo.maxValue,
                                     axis.rawAxisInfo.flat, axis.rawAxisInfo.fuzz,
                                     axis.rawAxisInfo.resolution);
            }
        }
    }
}

std::list<NotifyArgs> SensorInputMapper::reconfigure(nsecs_t when,
                                                     const InputReaderConfiguration& config,
                                                     ConfigurationChanges changes) {
    std::list<NotifyArgs> out = InputMapper::reconfigure(when, config, changes);

    if (!changes.any()) { // first time only
        mDeviceEnabled = true;
        // Check if device has MSC_TIMESTAMP event.
        mHasHardwareTimestamp = getDeviceContext().hasMscEvent(MSC_TIMESTAMP);
        // Collect all axes.
        for (int32_t abs = ABS_X; abs <= ABS_MAX; abs++) {
            // axis must be claimed by sensor class device
            if (!(getAbsAxisUsage(abs, getDeviceContext().getDeviceClasses())
                          .test(InputDeviceClass::SENSOR))) {
                continue;
            }
            if (std::optional<RawAbsoluteAxisInfo> rawAxisInfo = getAbsoluteAxisInfo(abs);
                rawAxisInfo) {
                AxisInfo axisInfo;
                // Axis doesn't need to be mapped, as sensor mapper doesn't generate any motion
                // input events
                axisInfo.mode = AxisInfo::MODE_NORMAL;
                axisInfo.axis = -1;
                // Check key layout map for sensor data mapping to axes
                auto ret = getDeviceContext().mapSensor(abs);
                if (ret.ok()) {
                    InputDeviceSensorType sensorType = (*ret).first;
                    int32_t sensorDataIndex = (*ret).second;
                    const Axis& axis = createAxis(axisInfo, rawAxisInfo.value());
                    parseSensorConfiguration(sensorType, abs, sensorDataIndex, axis);

                    mAxes.insert({abs, axis});
                }
            }
        }
    }
    return out;
}

SensorInputMapper::Axis SensorInputMapper::createAxis(const AxisInfo& axisInfo,
                                                      const RawAbsoluteAxisInfo& rawAxisInfo) {
    // Apply flat override.
    int32_t rawFlat = axisInfo.flatOverride < 0 ? rawAxisInfo.flat : axisInfo.flatOverride;

    float scale = std::numeric_limits<float>::signaling_NaN();
    float offset = 0;

    // resolution is 1 of sensor's unit.  For accelerometer, it is G, for gyroscope,
    // it is degree/s.
    scale = 1.0f / rawAxisInfo.resolution;
    offset = avg(rawAxisInfo.minValue, rawAxisInfo.maxValue) * -scale;

    const float max = rawAxisInfo.maxValue / rawAxisInfo.resolution;
    const float min = rawAxisInfo.minValue / rawAxisInfo.resolution;
    const float flat = rawFlat * scale;
    const float fuzz = rawAxisInfo.fuzz * scale;
    const float resolution = rawAxisInfo.resolution;

    // To eliminate noise while the Sensor is at rest, filter out small variations
    // in axis values up front.
    const float filter = fuzz ? fuzz : flat * 0.25f;
    return Axis(rawAxisInfo, axisInfo, scale, offset, min, max, flat, fuzz, resolution, filter);
}

std::list<NotifyArgs> SensorInputMapper::reset(nsecs_t when) {
    // Recenter all axes.
    for (std::pair<const int32_t, Axis>& pair : mAxes) {
        Axis& axis = pair.second;
        axis.resetValue();
    }
    mHardwareTimestamp = 0;
    mPrevMscTime = 0;
    return InputMapper::reset(when);
}

SensorInputMapper::Sensor SensorInputMapper::createSensor(InputDeviceSensorType sensorType,
                                                          const Axis& axis) {
    InputDeviceIdentifier identifier = getDeviceContext().getDeviceIdentifier();
    const auto& config = getDeviceContext().getConfiguration();

    std::string prefix = "sensor." + ftl::enum_string(sensorType);
    transform(prefix.begin(), prefix.end(), prefix.begin(), ::tolower);

    int32_t flags = 0;
    std::optional<int32_t> reportingMode = config.getInt(prefix + ".reportingMode");
    if (reportingMode.has_value()) {
        flags |= (*reportingMode & REPORTING_MODE_MASK) << REPORTING_MODE_SHIFT;
    }

    // Sensor Id will be assigned to device Id to distinguish same sensor from multiple input
    // devices, in such a way that the sensor Id will be same as input device Id.
    // The sensorType is to distinguish different sensors within one device.
    // One input device can only have 1 sensor for each sensor Type.
    InputDeviceSensorInfo sensorInfo(identifier.name, std::to_string(identifier.vendor),
                                     identifier.version, sensorType,
                                     InputDeviceSensorAccuracy::HIGH,
                                     /*maxRange=*/axis.max, /*resolution=*/axis.scale,
                                     /*power=*/config.getFloat(prefix + ".power").value_or(0.0f),
                                     /*minDelay=*/config.getInt(prefix + ".minDelay").value_or(0),
                                     /*fifoReservedEventCount=*/
                                     config.getInt(prefix + ".fifoReservedEventCount").value_or(0),
                                     /*fifoMaxEventCount=*/
                                     config.getInt(prefix + ".fifoMaxEventCount").value_or(0),
                                     ftl::enum_string(sensorType),
                                     /*maxDelay=*/config.getInt(prefix + ".maxDelay").value_or(0),
                                     /*flags=*/flags, getDeviceId());

    return Sensor(sensorInfo);
}

void SensorInputMapper::processHardWareTimestamp(nsecs_t evTime, int32_t mscTime) {
    // Since MSC_TIMESTAMP initial state is different from the system time, we
    // calculate the difference between two MSC_TIMESTAMP events, and use that
    // to calculate the system time that should be tagged on the event.
    // if the first time MSC_TIMESTAMP, store it
    // else calculate difference between previous and current MSC_TIMESTAMP
    if (mPrevMscTime == 0) {
        mHardwareTimestamp = evTime;
        ALOGD_IF(DEBUG_SENSOR_EVENT_DETAILS, "Initialize hardware timestamp = %" PRId64,
                 mHardwareTimestamp);
    } else {
        // Calculate the difference between current msc_timestamp and
        // previous msc_timestamp, including when msc_timestamp wraps around.
        uint32_t timeDiff = (mPrevMscTime > static_cast<uint32_t>(mscTime))
                ? (UINT32_MAX - mPrevMscTime + static_cast<uint32_t>(mscTime + 1))
                : (static_cast<uint32_t>(mscTime) - mPrevMscTime);

        mHardwareTimestamp += timeDiff * 1000LL;
    }
    mPrevMscTime = static_cast<uint32_t>(mscTime);
}

std::list<NotifyArgs> SensorInputMapper::process(const RawEvent& rawEvent) {
    std::list<NotifyArgs> out;
    switch (rawEvent.type) {
        case EV_ABS: {
            auto it = mAxes.find(rawEvent.code);
            if (it != mAxes.end()) {
                Axis& axis = it->second;
                axis.newValue = rawEvent.value * axis.scale + axis.offset;
            }
            break;
        }

        case EV_SYN:
            switch (rawEvent.code) {
                case SYN_REPORT:
                    for (std::pair<const int32_t, Axis>& pair : mAxes) {
                        Axis& axis = pair.second;
                        axis.currentValue = axis.newValue;
                    }
                    out += sync(rawEvent.when, /*force=*/false);
                    break;
            }
            break;

        case EV_MSC:
            switch (rawEvent.code) {
                case MSC_TIMESTAMP:
                    // hardware timestamp is nano seconds
                    processHardWareTimestamp(rawEvent.when, rawEvent.value);
                    break;
            }
    }
    return out;
}

bool SensorInputMapper::setSensorEnabled(InputDeviceSensorType sensorType, bool enabled) {
    auto it = mSensors.find(sensorType);
    if (it == mSensors.end()) {
        return false;
    }

    it->second.enabled = enabled;
    if (!enabled) {
        it->second.resetValue();
    }

    /* Currently we can't enable/disable sensors individually. Enabling any sensor will enable
     * the device
     */
    mDeviceEnabled = false;
    for (const auto& [_, sensor] : mSensors) {
        // If any sensor is on we will turn on the device.
        if (sensor.enabled) {
            mDeviceEnabled = true;
            break;
        }
    }
    return true;
}

void SensorInputMapper::flushSensor(InputDeviceSensorType sensorType) {
    auto it = mSensors.find(sensorType);
    if (it == mSensors.end()) {
        return;
    }
    auto& sensor = it->second;
    sensor.lastSampleTimeNs = 0;
    for (size_t i = 0; i < SENSOR_VEC_LEN; i++) {
        int32_t abs = sensor.dataVec[i];
        auto itAxis = mAxes.find(abs);
        if (itAxis != mAxes.end()) {
            Axis& axis = itAxis->second;
            axis.resetValue();
        }
    }
}

bool SensorInputMapper::enableSensor(InputDeviceSensorType sensorType,
                                     std::chrono::microseconds samplingPeriod,
                                     std::chrono::microseconds maxBatchReportLatency) {
    ALOGD_IF(DEBUG_SENSOR_EVENT_DETAILS,
             "Enable Sensor %s samplingPeriod %lld maxBatchReportLatency %lld",
             ftl::enum_string(sensorType).c_str(), samplingPeriod.count(),
             maxBatchReportLatency.count());

    if (!setSensorEnabled(sensorType, /*enabled=*/true)) {
        return false;
    }

    // Enable device
    if (mDeviceEnabled) {
        getDeviceContext().enableDevice();
    }

    // We know the sensor exists now, update the sampling period and batch report latency.
    auto it = mSensors.find(sensorType);
    it->second.samplingPeriod =
            std::chrono::duration_cast<std::chrono::nanoseconds>(samplingPeriod);
    it->second.maxBatchReportLatency =
            std::chrono::duration_cast<std::chrono::nanoseconds>(maxBatchReportLatency);
    return true;
}

void SensorInputMapper::disableSensor(InputDeviceSensorType sensorType) {
    ALOGD_IF(DEBUG_SENSOR_EVENT_DETAILS, "Disable Sensor %s", ftl::enum_string(sensorType).c_str());

    if (!setSensorEnabled(sensorType, /*enabled=*/false)) {
        return;
    }

    // Disable device
    if (!mDeviceEnabled) {
        mHardwareTimestamp = 0;
        mPrevMscTime = 0;
        getDeviceContext().disableDevice();
    }
}

std::list<NotifyArgs> SensorInputMapper::sync(nsecs_t when, bool force) {
    std::list<NotifyArgs> out;
    for (auto& [sensorType, sensor] : mSensors) {
        // Skip if sensor not enabled
        if (!sensor.enabled) {
            continue;
        }
        std::vector<float> values;
        for (ssize_t i = 0; i < SENSOR_VEC_LEN; i++) {
            int32_t abs = sensor.dataVec[i];
            auto it = mAxes.find(abs);
            if (it != mAxes.end()) {
                const Axis& axis = it->second;
                values.push_back(axis.currentValue);
            }
        }

        nsecs_t timestamp = mHasHardwareTimestamp ? mHardwareTimestamp : when;
        ALOGD_IF(DEBUG_SENSOR_EVENT_DETAILS, "Sensor %s timestamp %" PRIu64 " values [%f %f %f]",
                 ftl::enum_string(sensorType).c_str(), timestamp, values[0], values[1], values[2]);
        if (sensor.lastSampleTimeNs.has_value() &&
            timestamp - sensor.lastSampleTimeNs.value() < sensor.samplingPeriod.count()) {
            ALOGD_IF(DEBUG_SENSOR_EVENT_DETAILS, "Sensor %s Skip a sample.",
                     ftl::enum_string(sensorType).c_str());
        } else {
            // Convert to Android unit
            convertFromLinuxToAndroid(values, sensorType);
            // Notify dispatcher for sensor event
            out.push_back(NotifySensorArgs(getContext()->getNextId(), when, getDeviceId(),
                                           AINPUT_SOURCE_SENSOR, sensorType,
                                           sensor.sensorInfo.accuracy,
                                           /*accuracyChanged=*/sensor.accuracy !=
                                                   sensor.sensorInfo.accuracy,
                                           /*hwTimestamp=*/timestamp, values));
            sensor.lastSampleTimeNs = timestamp;
            sensor.accuracy = sensor.sensorInfo.accuracy;
        }
    }
    return out;
}

} // namespace android
