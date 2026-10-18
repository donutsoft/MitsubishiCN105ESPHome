#include "cn105.h"

namespace esphome {
void CN105Climate::set_short_cycle_protection_switch(HVACOptionSwitch* control) {
    short_cycle_switch_ = control;
    control->publish_state(false); // Explicit enable each boot, no restored ON state.
    control->setCallbackFunction([this](bool enabled) {
        short_cycle_.enabled = enabled;
        short_cycle_switch_->publish_state(enabled);
        last_remote_temp_sent_ = NAN;
        shouldSendExternalTemperature_ = true;
    });
}
bool CN105Climate::short_cycle_sample_fresh() const {
    return short_cycle_sample_seen_ && static_cast<uint32_t>(CUSTOM_MILLIS - short_cycle_sample_ms_) < remote_temp_timeout_;
}
float CN105Climate::getDeadbandAdjustedTemperature(float actual) {
    if (!short_cycle_switch_) return actual;
    if (!short_cycle_sample_fresh()) return 0;
    const float value = short_cycle_.temperature(mode == climate::CLIMATE_MODE_HEAT, actual, getTargetTemperature());
    return std::round(value * 2) / 2;
}
void CN105Climate::update_short_cycle_protection() {
    if (!short_cycle_switch_) return;
    if (getDeadbandAdjustedTemperature(remoteTemperature_) != last_remote_temp_sent_)
        shouldSendExternalTemperature_ = true;
    if (!isHeatpumpConnected() || !currentSettings.connected ||
        wantedSettings.hasChanged || wantedSettings.hasBeenSent) return;
    // Observable IR mode changes also cancel restart ownership. Repeated IR OFF
    // while already OFF cannot be distinguished from no command by CN105.
    if (short_cycle_.automatic_off && mode != climate::CLIMATE_MODE_OFF)
        short_cycle_.user_mode_changed();
    const auto action = short_cycle_.update(mode == climate::CLIMATE_MODE_HEAT,
        short_cycle_sample_fresh(), remoteTemperature_, getTargetTemperature());
    if (action == cn105::ShortCycleProtection::NONE) return;
    ESP_LOGI("short_cycle", "%s: room %.1f°C, target %.1f°C",
        action == cn105::ShortCycleProtection::OFF ? "Overheat OFF requested" : "Room cooled, restoring HEAT",
        remoteTemperature_, getTargetTemperature());
    short_cycle_own_call_ = true;
    auto call = make_call();
    call.set_mode(action == cn105::ShortCycleProtection::OFF ? climate::CLIMATE_MODE_OFF : climate::CLIMATE_MODE_HEAT);
    call.perform();
    short_cycle_own_call_ = false;
}
}  // namespace esphome
