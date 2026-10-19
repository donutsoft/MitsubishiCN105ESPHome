#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome::cn105 {

// Temperatures and offsets are Celsius. Coordination only changes the value
// reported over CN105; it never changes the user's visible setpoint.
struct HeatHandoff {
    enum Zone : uint8_t { UNAVAILABLE = 0, URGENT = 1, DEADBAND = 2, SATISFIED = 3 };
    struct Member {
        std::string id;
        Zone zone{UNAVAILABLE};
        float error{NAN};
    };

    bool configured{false};
    bool enabled{false};
    float lower_offset{-0.5f};
    float upper_offset{0.5f};
    float call_bias{0.5f};
    float idle_bias{1.0f};
    float switch_margin{0.3f};
    uint32_t minimum_hold_ms{300000};

    Zone zone(bool heating_mode, bool fresh, float actual, float target) const {
        if (!enabled || !heating_mode || !fresh || !std::isfinite(actual) ||
            !std::isfinite(target))
            return UNAVAILABLE;
        const float error = actual - target;
        if (error < lower_offset) return URGENT;
        if (error <= upper_offset) return DEADBAND;
        return SATISFIED;
    }

    float temperature(bool heating_mode, bool fresh, float actual, float target,
                      bool assignment_active, bool is_owner) const {
        if (!enabled || !assignment_active ||
            zone(heating_mode, fresh, actual, target) != DEADBAND)
            return actual;
        return is_owner ? target - call_bias : target + idle_bias;
    }

    bool may_replace_owner(uint32_t now, uint32_t owner_since_ms,
                           float owner_error, float challenger_error) const {
        if (static_cast<uint32_t>(now - owner_since_ms) < minimum_hold_ms)
            return false;
        return challenger_error < owner_error - switch_margin;
    }

    std::string choose_owner(uint32_t now, const std::string &current_owner,
                             uint32_t owner_since_ms,
                             const std::vector<Member> &members) const {
        std::string candidate;
        float candidate_error = 0.0f;
        bool current_valid = false;
        float current_error = 0.0f;
        for (const auto &member : members) {
            if (member.zone == URGENT) return "";
            if (member.zone != DEADBAND || !std::isfinite(member.error)) continue;
            if (member.id == current_owner) {
                current_valid = true;
                current_error = member.error;
            }
            if (candidate.empty() || member.error < candidate_error - 0.001f ||
                (std::fabs(member.error - candidate_error) <= 0.001f &&
                 member.id < candidate)) {
                candidate = member.id;
                candidate_error = member.error;
            }
        }
        if (current_valid && !candidate.empty() && candidate != current_owner &&
            !may_replace_owner(now, owner_since_ms, current_error, candidate_error))
            return current_owner;
        return candidate;
    }
};

}  // namespace esphome::cn105
