#pragma once
#include <cmath>

namespace esphome::cn105 {
// Temperatures and offsets are Celsius. No synthetic values are stored as samples.
struct ShortCycleProtection {
    bool enabled{false};
    bool automatic_off{false};
    enum Action { NONE, OFF, HEAT };

    void user_mode_changed() { automatic_off = false; }
    Action update(bool heating, bool fresh, float actual, float target) {
        if (!fresh || !std::isfinite(actual) || !std::isfinite(target)) return NONE;
        if (enabled && heating && !automatic_off && actual > target + 1.5f) {
            automatic_off = true;
            return OFF;
        }
        if (automatic_off && actual < target + 1.0f) {
            automatic_off = false;
            return HEAT;
        }
        return NONE;
    }
    float temperature(bool heating, float actual, float target) const {
        if (enabled && heating && std::isfinite(actual) && std::isfinite(target) &&
            actual > target + 0.5f && actual <= target + 1.5f)
            return target + 0.5f;
        return actual;
    }
};
}  // namespace esphome::cn105
