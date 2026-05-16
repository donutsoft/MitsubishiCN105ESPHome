#include "cn105.h"
#include "Globals.h"

#include <cmath>

#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#include <ArduinoJson.h>
#endif

// Multi-zone mode coordinator lets multiple indoor units that share one outdoor unit make
// the same heat/cool decision. Each zone publishes its requested mode, setpoint,
// and current temperature on the shared MQTT topic. Every zone keeps the newest
// non-stale values from the other zones, then combines all zones by demand.
//
// Demand is represented as a temperature delta:
//   delta = current temperature - the relevant target temperature
//
// A negative delta means the zone is colder than its heat target and wants heat.
// A positive delta means the zone is warmer than its cool target and wants
// cooling. A zero delta means the zone is satisfied, missing data, or in a mode
// that does not participate. Heat uses the low target when available; cool, dry,
// and fan-only use the high target when available; auto/heat_cool use the
// low/high range or a small range around the single target.
//
// The sum of all signed deltas is the shared demand. The local unit only turns
// on when its own delta is past the same threshold in the same direction as the
// shared demand: negative plus negative runs heat, positive plus positive runs
// cool/dry/fan, and any mismatch or satisfied local zone turns this unit off.
using namespace esphome;

namespace {

constexpr const char* LOG_MULTI_ZONE_MODE_COORDINATOR_TAG = "MULTI_ZONE_MODE_COORDINATOR";

inline bool hasTemperatureValue(float value) {
    return !std::isnan(value);
}

constexpr float MULTI_ZONE_MODE_COORDINATOR_MINIMUM_DELTA = 0.1f;
constexpr float MULTI_ZONE_MODE_COORDINATOR_SINGLE_TARGET_DEADBAND = 2.0f;

float getDualOrSingleSetpoint(float dual_setpoint, float single_setpoint) {
    return hasTemperatureValue(dual_setpoint) ? dual_setpoint : single_setpoint;
}

const char* multiZoneModeCoordinatorModeToString(climate::ClimateMode mode) {
    switch (mode) {
        case climate::CLIMATE_MODE_OFF:
            return "off";
        case climate::CLIMATE_MODE_HEAT:
            return "heat";
        case climate::CLIMATE_MODE_COOL:
            return "cool";
        case climate::CLIMATE_MODE_HEAT_COOL:
            return "heat_cool";
        case climate::CLIMATE_MODE_AUTO:
            return "auto";
        case climate::CLIMATE_MODE_DRY:
            return "dry";
        case climate::CLIMATE_MODE_FAN_ONLY:
            return "fan_only";
        default:
            return "unknown";
    }
}

climate::ClimateMode multiZoneModeCoordinatorModeFromString(const std::string& mode) {
    if (mode == "heat") return climate::CLIMATE_MODE_HEAT;
    if (mode == "cool") return climate::CLIMATE_MODE_COOL;
    if (mode == "heat_cool") return climate::CLIMATE_MODE_HEAT_COOL;
    if (mode == "auto") return climate::CLIMATE_MODE_AUTO;
    if (mode == "dry") return climate::CLIMATE_MODE_DRY;
    if (mode == "fan_only") return climate::CLIMATE_MODE_FAN_ONLY;
    return climate::CLIMATE_MODE_OFF;
}

bool isCoordinatedClimateMode(climate::ClimateMode mode) {
    // HEAT_COOL is an ESPHome/Home Assistant requested mode. Mitsubishi hardware
    // never receives it directly; controlMode() maps it to the appropriate CN105 command.
    return mode == climate::CLIMATE_MODE_HEAT ||
        mode == climate::CLIMATE_MODE_COOL ||
        mode == climate::CLIMATE_MODE_HEAT_COOL ||
        mode == climate::CLIMATE_MODE_AUTO ||
        mode == climate::CLIMATE_MODE_DRY ||
        mode == climate::CLIMATE_MODE_FAN_ONLY;
}

float calculateZoneDemandDelta(const MultiZoneModeCoordinatorData& data) {
    if (!isCoordinatedClimateMode(data.requested_mode) ||
        !hasTemperatureValue(data.current_temperature)) {
        return 0.0f;
    }

    const float low_target = getDualOrSingleSetpoint(data.target_temperature_low, data.target_temperature);
    const float high_target = getDualOrSingleSetpoint(data.target_temperature_high, data.target_temperature);

    if (data.requested_mode == climate::CLIMATE_MODE_HEAT) {
        return hasTemperatureValue(low_target) && data.current_temperature < low_target
            ? data.current_temperature - low_target
            : 0.0f;
    }

    if (data.requested_mode == climate::CLIMATE_MODE_COOL ||
        data.requested_mode == climate::CLIMATE_MODE_DRY ||
        data.requested_mode == climate::CLIMATE_MODE_FAN_ONLY) {
        return hasTemperatureValue(high_target) && data.current_temperature > high_target
            ? data.current_temperature - high_target
            : 0.0f;
    }

    // Remaining participating modes are AUTO and HEAT_COOL. They evaluate both
    // sides of the comfort range, synthesizing one from the single setpoint if needed.
    float low = data.target_temperature_low;
    float high = data.target_temperature_high;
    if ((!hasTemperatureValue(low) || !hasTemperatureValue(high)) && hasTemperatureValue(data.target_temperature)) {
        if (!hasTemperatureValue(low)) {
            low = data.target_temperature - MULTI_ZONE_MODE_COORDINATOR_SINGLE_TARGET_DEADBAND;
        }
        if (!hasTemperatureValue(high)) {
            high = data.target_temperature + MULTI_ZONE_MODE_COORDINATOR_SINGLE_TARGET_DEADBAND;
        }
    }

    if (hasTemperatureValue(low) && data.current_temperature < low) {
        return data.current_temperature - low;
    }
    if (hasTemperatureValue(high) && data.current_temperature > high) {
        return data.current_temperature - high;
    }
    return 0.0f;
}

climate::ClimateMode selectCoordinatedMode(
    const MultiZoneModeCoordinator& state,
    climate::ClimateMode current_mode,
    float net_demand_delta) {
    const auto local_it = state.data.find(state.climate_id);
    if (local_it == state.data.end()) {
        return current_mode;
    }

    const MultiZoneModeCoordinatorData& local = local_it->second;
    // If all zones are either off or requesting the same mode, use that mode.
    bool all_same_or_off = true;
    for (const auto& kvp : state.data) {
        const climate::ClimateMode mode = kvp.second.requested_mode;

        if (mode != climate::CLIMATE_MODE_OFF &&
            mode != local.requested_mode) {
            all_same_or_off = false;
            break;
        }
    }

    if (local.requested_mode == climate::CLIMATE_MODE_OFF ||
        !isCoordinatedClimateMode(local.requested_mode) ||
        all_same_or_off) {
        return local.requested_mode;
    }

    if (!hasTemperatureValue(local.current_temperature)) {
        return current_mode;
    }

    const float local_delta = calculateZoneDemandDelta(local);
    if (net_demand_delta < -MULTI_ZONE_MODE_COORDINATOR_MINIMUM_DELTA &&
        local_delta < -MULTI_ZONE_MODE_COORDINATOR_MINIMUM_DELTA) {
        return climate::CLIMATE_MODE_HEAT;
    }

    if (net_demand_delta > MULTI_ZONE_MODE_COORDINATOR_MINIMUM_DELTA &&
        local_delta > MULTI_ZONE_MODE_COORDINATOR_MINIMUM_DELTA) {
        if (local.requested_mode == climate::CLIMATE_MODE_DRY) {
            return climate::CLIMATE_MODE_DRY;
        }
        if (local.requested_mode == climate::CLIMATE_MODE_FAN_ONLY) {
            return climate::CLIMATE_MODE_FAN_ONLY;
        }
        return climate::CLIMATE_MODE_COOL;
    }

    return climate::CLIMATE_MODE_OFF;
}

#ifdef USE_MQTT

template<typename TDocument> void writeZoneJsonTemperature(TDocument& doc, const char* key, float value) {
    if (hasTemperatureValue(value)) {
        doc[key] = value;
    } else {
        doc[key] = nullptr;
    }
}

template<typename TVariant> float readZoneJsonTemperature(TVariant value) {
    return value.isNull() ? NAN : value.template as<float>();
}

#endif

}  // namespace

void CN105Climate::set_multi_zone_mode_coordinator_topic(const std::string& topic) {
    this->multi_zone_mode_coordinator_.topic = topic;
    this->multi_zone_mode_coordinator_.enabled = !topic.empty();
}

void CN105Climate::set_multi_zone_mode_coordinator_climate_id(const std::string& climate_id) {
    this->multi_zone_mode_coordinator_.climate_id = climate_id;
}

void CN105Climate::set_multi_zone_mode_coordinator_publish_interval(uint32_t interval_ms) {
    this->multi_zone_mode_coordinator_.publish_interval_ms = interval_ms;
}

void CN105Climate::set_multi_zone_mode_coordinator_stale_after(uint32_t stale_after_ms) {
    this->multi_zone_mode_coordinator_.stale_after_ms = stale_after_ms;
}

void CN105Climate::setupMultiZoneModeCoordinator() {
    if (!this->multi_zone_mode_coordinator_.enabled) {
        return;
    }

#ifdef USE_MQTT
    if (mqtt::global_mqtt_client == nullptr) {
        ESP_LOGW(LOG_MULTI_ZONE_MODE_COORDINATOR_TAG, "MQTT is not ready yet; multi-zone mode coordinator will retry subscription in loop.");
        return;
    }

    if (this->multi_zone_mode_coordinator_.mqtt_subscribed) {
        return;
    }

    mqtt::global_mqtt_client->subscribe(this->multi_zone_mode_coordinator_.topic, [this](const std::string& topic, const std::string& payload) {
        (void)topic;
        this->receiveMultiZoneState(payload);
    });
    this->multi_zone_mode_coordinator_.mqtt_subscribed = true;
    ESP_LOGI(LOG_MULTI_ZONE_MODE_COORDINATOR_TAG, "Subscribed to shared zone topic '%s' as climate '%s'.",
        this->multi_zone_mode_coordinator_.topic.c_str(), this->multi_zone_mode_coordinator_.climate_id.c_str());
#else
    ESP_LOGW(LOG_MULTI_ZONE_MODE_COORDINATOR_TAG, "MQTT is not enabled; multi-zone mode coordinator is disabled.");
    this->multi_zone_mode_coordinator_.enabled = false;
#endif
}

void CN105Climate::multiZoneCoordinationLoop() {
    if (!this->multi_zone_mode_coordinator_.enabled) {
        return;
    }

    if (!this->multi_zone_mode_coordinator_.mqtt_subscribed) {
        this->setupMultiZoneModeCoordinator();
    }

    const uint32_t now = CUSTOM_MILLIS;
    for (auto it = this->multi_zone_mode_coordinator_.data.begin(); it != this->multi_zone_mode_coordinator_.data.end();) {
        const bool is_stale_remote = !it->second.is_local &&
            this->multi_zone_mode_coordinator_.stale_after_ms > 0 &&
            now - it->second.updated_ms > this->multi_zone_mode_coordinator_.stale_after_ms;
        if (is_stale_remote) {
            ESP_LOGD(LOG_MULTI_ZONE_MODE_COORDINATOR_TAG, "Dropping stale zone '%s'.", it->first.c_str());
            it = this->multi_zone_mode_coordinator_.data.erase(it);
        } else {
            ++it;
        }
    }

    this->publishMultiZoneState(false);
    this->reconcileMultiZoneMode();
}

void CN105Climate::setRequestedMultiZoneMode(climate::ClimateMode requested_mode) {
    if (!this->multi_zone_mode_coordinator_.enabled || this->multi_zone_mode_coordinator_.applying_override) {
        return;
    }

    this->multi_zone_mode_coordinator_.requested_mode = requested_mode;
    this->multi_zone_mode_coordinator_.requested_mode_initialized = true;
    this->multi_zone_mode_coordinator_.force_apply = true;
}

void CN105Climate::syncLocalMultiZoneState(bool force_apply) {
    if (!this->multi_zone_mode_coordinator_.enabled) {
        return;
    }

    if (force_apply) {
        this->multi_zone_mode_coordinator_.force_apply = true;
    }

    this->publishMultiZoneState(true);
    this->reconcileMultiZoneMode();
}

void CN105Climate::publishMultiZoneState(bool force) {
    if (!this->multi_zone_mode_coordinator_.enabled) {
        return;
    }

    const uint32_t now = CUSTOM_MILLIS;
    if (!force && this->multi_zone_mode_coordinator_.publish_interval_ms > 0 &&
        now - this->multi_zone_mode_coordinator_.last_publish_ms < this->multi_zone_mode_coordinator_.publish_interval_ms) {
        return;
    }

    if (!this->multi_zone_mode_coordinator_.requested_mode_initialized) {
        if (this->currentSettings.power == nullptr || this->currentSettings.mode == nullptr) {
            ESP_LOGD(LOG_MULTI_ZONE_MODE_COORDINATOR_TAG, "Local heat pump mode is not known yet; delaying coordinator publish.");
            return;
        }
        this->multi_zone_mode_coordinator_.requested_mode = this->mode;
        this->multi_zone_mode_coordinator_.requested_mode_initialized = true;
    }

    MultiZoneModeCoordinatorData local;
    local.requested_mode = this->multi_zone_mode_coordinator_.requested_mode;
    local.target_temperature = this->getTargetTemperature();
    local.target_temperature_low = this->getTargetTemperatureLow();
    local.target_temperature_high = this->getTargetTemperatureHigh();
    local.current_temperature = this->getCurrentTemperature();
    local.updated_ms = now;
    local.is_local = true;
    this->multi_zone_mode_coordinator_.data[this->multi_zone_mode_coordinator_.climate_id] = local;
    this->multi_zone_mode_coordinator_.last_publish_ms = now;

#ifdef USE_MQTT
    if (mqtt::global_mqtt_client == nullptr || !this->multi_zone_mode_coordinator_.mqtt_subscribed) {
        return;
    }

    JsonDocument doc;
    doc["id"] = this->multi_zone_mode_coordinator_.climate_id;
    doc["mode"] = multiZoneModeCoordinatorModeToString(local.requested_mode);
    writeZoneJsonTemperature(doc, "target_temperature", local.target_temperature);
    writeZoneJsonTemperature(doc, "target_temperature_low", local.target_temperature_low);
    writeZoneJsonTemperature(doc, "target_temperature_high", local.target_temperature_high);
    writeZoneJsonTemperature(doc, "current_temperature", local.current_temperature);

    std::string payload;
    serializeJson(doc, payload);
    mqtt::global_mqtt_client->publish(this->multi_zone_mode_coordinator_.topic, payload, 0, false);
#endif
}

void CN105Climate::receiveMultiZoneState(const std::string& payload) {
    if (!this->multi_zone_mode_coordinator_.enabled) {
        return;
    }

#ifdef USE_MQTT
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        ESP_LOGW(LOG_MULTI_ZONE_MODE_COORDINATOR_TAG, "Ignoring invalid zone payload: %s", error.c_str());
        return;
    }

    const char* id = doc["id"] | "";
    if (id[0] == '\0' || this->multi_zone_mode_coordinator_.climate_id == id) {
        return;
    }

    const char* mode = doc["mode"] | "off";
    MultiZoneModeCoordinatorData remote;
    remote.requested_mode = multiZoneModeCoordinatorModeFromString(mode);
    remote.target_temperature = readZoneJsonTemperature(doc["target_temperature"]);
    remote.target_temperature_low = readZoneJsonTemperature(doc["target_temperature_low"]);
    remote.target_temperature_high = readZoneJsonTemperature(doc["target_temperature_high"]);
    remote.current_temperature = readZoneJsonTemperature(doc["current_temperature"]);
    remote.updated_ms = CUSTOM_MILLIS;
    remote.is_local = false;

    if (isCoordinatedClimateMode(remote.requested_mode)) {
        this->multi_zone_mode_coordinator_.data[id] = remote;
    } else {
        this->multi_zone_mode_coordinator_.data.erase(id);
    }

    this->reconcileMultiZoneMode();
#else
    (void)payload;
#endif
}

void CN105Climate::reconcileMultiZoneMode() {
    if (!this->multi_zone_mode_coordinator_.enabled) {
        return;
    }

    float net_demand_delta = 0.0f;
    for (const auto& kvp : this->multi_zone_mode_coordinator_.data) {
        net_demand_delta += calculateZoneDemandDelta(kvp.second);
    }

    if (std::fabs(net_demand_delta) <= MULTI_ZONE_MODE_COORDINATOR_MINIMUM_DELTA) {
        net_demand_delta = 0.0f;
    }

    const climate::ClimateMode effective_mode = selectCoordinatedMode(
        this->multi_zone_mode_coordinator_, this->mode, net_demand_delta);
    this->applyCoordinatedMode(effective_mode);
}

bool CN105Climate::shouldShowMultiZoneRequestedModeInUi() const {
    return this->multi_zone_mode_coordinator_.enabled &&
        this->multi_zone_mode_coordinator_.requested_mode_initialized &&
        isCoordinatedClimateMode(this->multi_zone_mode_coordinator_.requested_mode);
}

void CN105Climate::applyCoordinatedMode(climate::ClimateMode effective_mode) {
    if (this->multi_zone_mode_coordinator_.data.find(this->multi_zone_mode_coordinator_.climate_id) == this->multi_zone_mode_coordinator_.data.end()) {
        return;
    }

    if (this->multi_zone_mode_coordinator_.effective_mode_initialized &&
        this->multi_zone_mode_coordinator_.effective_mode == effective_mode &&
        !this->multi_zone_mode_coordinator_.force_apply) {
        return;
    }

    this->multi_zone_mode_coordinator_.effective_mode = effective_mode;
    this->multi_zone_mode_coordinator_.effective_mode_initialized = true;
    this->multi_zone_mode_coordinator_.force_apply = false;
    this->multi_zone_mode_coordinator_.applying_override = true;
    const climate::ClimateMode requested_mode = this->multi_zone_mode_coordinator_.requested_mode;
    const climate::ClimateMode ui_mode = this->shouldShowMultiZoneRequestedModeInUi() ? requested_mode : effective_mode;

    ESP_LOGI(LOG_MULTI_ZONE_MODE_COORDINATOR_TAG, "Applying effective mode %s for requested mode %s.",
        multiZoneModeCoordinatorModeToString(effective_mode),
        multiZoneModeCoordinatorModeToString(requested_mode));

    this->mode = effective_mode;
    this->controlMode();
    if (effective_mode != climate::CLIMATE_MODE_OFF) {
        this->controlTemperature();
    }
    this->mode = ui_mode;

    this->wantedSettings.hasChanged = true;
    this->wantedSettings.hasBeenSent = false;
    this->wantedSettings.lastChange = CUSTOM_MILLIS;
    this->publish_state();

    this->multi_zone_mode_coordinator_.applying_override = false;
}
