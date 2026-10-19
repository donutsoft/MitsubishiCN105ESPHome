#include "cn105.h"

#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

#include <cstdio>
#include <cstring>

namespace esphome {

static const char *const HANDOFF_TAG = "heat_handoff";

void CN105Climate::set_heat_handoff_member_id(const std::string &member_id) {
    heat_handoff_member_id_ = member_id;
    heat_handoff_.configured = !member_id.empty();
}

void CN105Climate::set_heat_handoff_coordinator_id(const std::string &coordinator_id) {
    heat_handoff_coordinator_id_ = coordinator_id;
}

void CN105Climate::set_heat_handoff_topic_prefix(const std::string &topic_prefix) {
    heat_handoff_topic_prefix_ = topic_prefix;
    while (!heat_handoff_topic_prefix_.empty() && heat_handoff_topic_prefix_.back() == '/')
        heat_handoff_topic_prefix_.pop_back();
}

void CN105Climate::add_heat_handoff_peer(const std::string &member_id) {
    if (member_id.empty() || member_id == heat_handoff_member_id_) return;
    for (const auto &peer : heat_handoff_peers_)
        if (peer.member_id == member_id) return;
    heat_handoff_peers_.push_back({member_id});
}

void CN105Climate::mark_heat_handoff_temperature_dirty_() {
    last_remote_temp_sent_ = NAN;
    shouldSendExternalTemperature_ = true;
}

void CN105Climate::set_heat_handoff_enabled(bool enabled) {
    if (heat_handoff_.enabled == enabled) return;
    heat_handoff_.enabled = enabled;
    heat_handoff_last_state_payload_.clear();
    mark_heat_handoff_temperature_dirty_();
}

void CN105Climate::set_heat_handoff_switch(HVACOptionSwitch *control) {
    heat_handoff_switch_ = control;
    control->publish_state(false);
    control->setCallbackFunction([this](bool enabled) {
        this->set_heat_handoff_enabled(enabled);
        this->heat_handoff_switch_->publish_state(enabled);
    });
}

bool CN105Climate::heat_handoff_member_known_(const std::string &member_id) const {
    if (member_id == heat_handoff_member_id_) return true;
    for (const auto &peer : heat_handoff_peers_)
        if (peer.member_id == member_id) return true;
    return false;
}

void CN105Climate::setup_heat_handoff_mqtt() {
    if (!heat_handoff_.configured) return;
#ifdef USE_MQTT
    if (mqtt::global_mqtt_client == nullptr) {
        ESP_LOGE(HANDOFF_TAG, "MQTT client unavailable; coordinated handoff disabled");
        heat_handoff_.enabled = false;
        return;
    }
    for (const auto &peer : heat_handoff_peers_) {
        const std::string topic = heat_handoff_topic_prefix_ + "/" + peer.member_id + "/state";
        const std::string member_id = peer.member_id;
        mqtt::global_mqtt_client->subscribe(topic,
            [this, member_id](const std::string &, const std::string &payload) {
                this->handle_heat_handoff_peer_message_(member_id, payload);
            });
    }
    mqtt::global_mqtt_client->subscribe(heat_handoff_topic_prefix_ + "/assignment",
        [this](const std::string &, const std::string &payload) {
            this->handle_heat_handoff_assignment_(payload);
        }, 1);
    heat_handoff_mqtt_ready_ = true;
    ESP_LOGI(HANDOFF_TAG, "MQTT heat handoff member '%s', coordinator '%s', %u peers",
        heat_handoff_member_id_.c_str(), heat_handoff_coordinator_id_.c_str(),
        static_cast<unsigned>(heat_handoff_peers_.size()));
#else
    ESP_LOGE(HANDOFF_TAG, "Heat handoff configured without MQTT support");
    heat_handoff_.enabled = false;
#endif
}

void CN105Climate::handle_heat_handoff_peer_message_(const std::string &member_id,
                                                      const std::string &payload) {
    // v1|enabled|heat_mode|sensor_fresh|zone|temperature_error_C
    char version[4]{};
    int enabled = 0, heating = 0, fresh = 0, zone = 0;
    float error = NAN;
    if (std::sscanf(payload.c_str(), "%3[^|]|%d|%d|%d|%d|%f", version, &enabled,
                    &heating, &fresh, &zone, &error) != 6 ||
        std::strcmp(version, "v1") != 0 ||
        (enabled != 0 && enabled != 1) || (heating != 0 && heating != 1) ||
        (fresh != 0 && fresh != 1) || zone < cn105::HeatHandoff::UNAVAILABLE ||
        zone > cn105::HeatHandoff::SATISFIED || !std::isfinite(error)) {
        ESP_LOGW(HANDOFF_TAG, "Ignoring malformed state from '%s'", member_id.c_str());
        return;
    }
    for (auto &peer : heat_handoff_peers_) {
        if (peer.member_id != member_id) continue;
        peer.enabled = enabled != 0;
        peer.heating_mode = heating != 0;
        peer.sensor_fresh = fresh != 0;
        peer.zone = static_cast<cn105::HeatHandoff::Zone>(zone);
        if (!peer.enabled || !peer.heating_mode || !peer.sensor_fresh)
            peer.zone = cn105::HeatHandoff::UNAVAILABLE;
        peer.error = error;
        peer.updated_ms = CUSTOM_MILLIS;
        return;
    }
}

void CN105Climate::handle_heat_handoff_assignment_(const std::string &payload) {
    // v1|coordinator_id|generation|active|owner_id-or-dash
    char version[4]{}, coordinator[40]{}, owner[40]{};
    unsigned long generation = 0;
    int active = 0;
    if (std::sscanf(payload.c_str(), "%3[^|]|%39[^|]|%lu|%d|%39s", version,
                    coordinator, &generation, &active, owner) != 5 ||
        std::strcmp(version, "v1") != 0 || coordinator != heat_handoff_coordinator_id_ ||
        (active != 0 && active != 1) ||
        (std::strcmp(owner, "-") != 0 && !heat_handoff_member_known_(owner)) ||
        (active == 0 && std::strcmp(owner, "-") != 0)) {
        ESP_LOGW(HANDOFF_TAG, "Ignoring malformed or foreign assignment");
        return;
    }
    (void) generation;
    const bool changed = heat_handoff_assignment_active_ != (active != 0) ||
                         heat_handoff_owner_ != (std::strcmp(owner, "-") == 0 ? "" : owner);
    heat_handoff_assignment_active_ = active != 0;
    heat_handoff_owner_ = std::strcmp(owner, "-") == 0 ? "" : owner;
    heat_handoff_assignment_received_ms_ = CUSTOM_MILLIS;
    if (changed) mark_heat_handoff_temperature_dirty_();
}

bool CN105Climate::heat_handoff_peer_fresh_(const HeatHandoffPeerState &peer,
                                            uint32_t now) const {
    return peer.updated_ms != 0 &&
           static_cast<uint32_t>(now - peer.updated_ms) < heat_handoff_peer_timeout_ms_;
}

bool CN105Climate::heat_handoff_assignment_fresh_(uint32_t now) const {
    return heat_handoff_assignment_received_ms_ != 0 &&
           static_cast<uint32_t>(now - heat_handoff_assignment_received_ms_) <
               heat_handoff_peer_timeout_ms_;
}

void CN105Climate::publish_heat_handoff_state_(bool force) {
#ifdef USE_MQTT
    if (!heat_handoff_mqtt_ready_ || mqtt::global_mqtt_client == nullptr ||
        !mqtt::global_mqtt_client->is_connected()) return;
    const uint32_t now = CUSTOM_MILLIS;
    const bool heating = mode == climate::CLIMATE_MODE_HEAT;
    const bool fresh = short_cycle_sample_fresh();
    const float target = getTargetTemperature();
    const float error = std::isfinite(remoteTemperature_) && std::isfinite(target)
                            ? remoteTemperature_ - target : 99.0f;
    const auto zone = heat_handoff_.zone(heating, fresh, remoteTemperature_, target);
    char payload[80];
    std::snprintf(payload, sizeof(payload), "v1|%d|%d|%d|%u|%.3f",
        heat_handoff_.enabled ? 1 : 0, heating ? 1 : 0, fresh ? 1 : 0,
        static_cast<unsigned>(zone), error);
    if (!force && payload == heat_handoff_last_state_payload_ &&
        static_cast<uint32_t>(now - heat_handoff_last_state_publish_ms_) <
            heat_handoff_publish_interval_ms_) return;
    const std::string topic = heat_handoff_topic_prefix_ + "/" +
                              heat_handoff_member_id_ + "/state";
    if (mqtt::global_mqtt_client->publish(topic, std::string(payload), 0, false)) {
        heat_handoff_last_state_payload_ = payload;
        heat_handoff_last_state_publish_ms_ = now;
    }
#else
    (void) force;
#endif
}

bool CN105Climate::publish_heat_handoff_assignment_(bool active,
                                                     const std::string &owner,
                                                     bool force) {
#ifdef USE_MQTT
    if (!heat_handoff_mqtt_ready_ || mqtt::global_mqtt_client == nullptr ||
        !mqtt::global_mqtt_client->is_connected()) return false;
    const uint32_t now = CUSTOM_MILLIS;
    const bool changed = heat_handoff_assignment_active_ != active ||
                         heat_handoff_owner_ != owner;
    if (!force && !changed &&
        static_cast<uint32_t>(now - heat_handoff_last_assignment_publish_ms_) <
            heat_handoff_publish_interval_ms_) return true;
    const uint32_t generation = heat_handoff_generation_ + (changed ? 1U : 0U);
    char payload[112];
    std::snprintf(payload, sizeof(payload), "v1|%s|%lu|%d|%s",
        heat_handoff_coordinator_id_.c_str(), static_cast<unsigned long>(generation),
        active ? 1 : 0, owner.empty() ? "-" : owner.c_str());
    if (!mqtt::global_mqtt_client->publish(heat_handoff_topic_prefix_ + "/assignment",
                                           std::string(payload), 1, false))
        return false;
    heat_handoff_generation_ = generation;
    heat_handoff_last_assignment_publish_ms_ = now;
    heat_handoff_assignment_received_ms_ = now;
    if (changed) {
        heat_handoff_assignment_active_ = active;
        heat_handoff_owner_ = owner;
        heat_handoff_owner_since_ms_ = now;
        mark_heat_handoff_temperature_dirty_();
        ESP_LOGI(HANDOFF_TAG, "Assignment %lu: %s",
            static_cast<unsigned long>(generation), owner.empty() ? "no deadband owner" : owner.c_str());
    }
    return true;
#else
    (void) active;
    (void) owner;
    (void) force;
    return false;
#endif
}

void CN105Climate::update_heat_handoff() {
    if (!heat_handoff_.configured || !heat_handoff_mqtt_ready_) return;
    const uint32_t now = CUSTOM_MILLIS;
    publish_heat_handoff_state_();

    bool ready = heat_handoff_.enabled;
    const bool own_heating = mode == climate::CLIMATE_MODE_HEAT;
    const bool own_fresh = short_cycle_sample_fresh();
    if (own_heating && !own_fresh) ready = false;
    for (const auto &peer : heat_handoff_peers_) {
        if (!heat_handoff_peer_fresh_(peer, now) || !peer.enabled ||
            (peer.heating_mode && !peer.sensor_fresh)) {
            ready = false;
            break;
        }
    }
    if (ready != heat_handoff_states_ready_) {
        heat_handoff_states_ready_ = ready;
        mark_heat_handoff_temperature_dirty_();
    }

    if (heat_handoff_member_id_ != heat_handoff_coordinator_id_) return;
    if (!ready) {
        publish_heat_handoff_assignment_(false, "");
        return;
    }

    std::vector<cn105::HeatHandoff::Member> members;
    members.reserve(heat_handoff_peers_.size() + 1);
    const float target = getTargetTemperature();
    const float own_error = std::isfinite(remoteTemperature_) && std::isfinite(target)
                                ? remoteTemperature_ - target : 99.0f;
    members.push_back({heat_handoff_member_id_,
        heat_handoff_.zone(own_heating, own_fresh, remoteTemperature_, target), own_error});
    for (const auto &peer : heat_handoff_peers_)
        members.push_back({peer.member_id, peer.zone, peer.error});

    const std::string desired_owner = heat_handoff_.choose_owner(
        now, heat_handoff_owner_, heat_handoff_owner_since_ms_, members);
    publish_heat_handoff_assignment_(true, desired_owner);
}

}  // namespace esphome
