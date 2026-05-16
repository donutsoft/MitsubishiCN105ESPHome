#pragma once

#include "esphome/components/climate/climate.h"

#include <cmath>
#include <map>
#include <string>

namespace esphome {

struct MultiZoneModeCoordinatorData {
    climate::ClimateMode requested_mode = climate::CLIMATE_MODE_OFF;
    float target_temperature = NAN;
    float target_temperature_low = NAN;
    float target_temperature_high = NAN;
    float current_temperature = NAN;
    uint32_t updated_ms = 0;
    bool is_local = false;
};

struct MultiZoneModeCoordinator {
    bool enabled = false;
    bool mqtt_subscribed = false;
    bool requested_mode_initialized = false;
    bool applying_override = false;
    bool effective_mode_initialized = false;
    bool force_apply = false;
    std::string topic;
    std::string climate_id;
    std::map<std::string, MultiZoneModeCoordinatorData> data;
    climate::ClimateMode requested_mode = climate::CLIMATE_MODE_OFF;
    climate::ClimateMode effective_mode = climate::CLIMATE_MODE_OFF;
    uint32_t publish_interval_ms = 30000;
    uint32_t stale_after_ms = 300000;
    uint32_t last_publish_ms = 0;
};

}  // namespace esphome
