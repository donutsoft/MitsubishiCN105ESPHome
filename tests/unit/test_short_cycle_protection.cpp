#include <gtest/gtest.h>
#include "short_cycle_protection.h"
using esphome::cn105::ShortCycleProtection;

TEST(ShortCycleProtection, ClampMatchesHeatingBand) {
    ShortCycleProtection p;
    p.enabled = true;
    EXPECT_FLOAT_EQ(p.temperature(true, 21, 22), 21);
    EXPECT_FLOAT_EQ(p.temperature(true, 22.5, 22), 22.5);
    EXPECT_FLOAT_EQ(p.temperature(true, 23, 22), 23);
    EXPECT_FLOAT_EQ(p.temperature(true, 23.5, 22), 23);
    EXPECT_FLOAT_EQ(p.temperature(true, 24, 22), 23);
    EXPECT_FLOAT_EQ(p.temperature(true, 24.01, 22), 24.01);
}
TEST(ShortCycleProtection, DisabledAndOtherModesPassThrough) {
    ShortCycleProtection p;
    EXPECT_FLOAT_EQ(p.temperature(true, 23, 22), 23);
    EXPECT_EQ(p.update(true, true, 24, 22), ShortCycleProtection::NONE);
    p.enabled = true;
    EXPECT_FLOAT_EQ(p.temperature(false, 23, 22), 23);
    EXPECT_EQ(p.update(false, true, 24, 22), ShortCycleProtection::NONE);
}
TEST(ShortCycleProtection, StrictOffAndRestartThresholds) {
    ShortCycleProtection p;
    p.enabled = true;
    EXPECT_EQ(p.update(true, true, 24, 22), ShortCycleProtection::NONE);
    EXPECT_EQ(p.update(true, true, 24.01, 22), ShortCycleProtection::OFF);
    EXPECT_EQ(p.update(false, true, 23, 22), ShortCycleProtection::NONE);
    EXPECT_EQ(p.update(false, true, 22.99, 22), ShortCycleProtection::HEAT);
    EXPECT_FALSE(p.automatic_off);
}
TEST(ShortCycleProtection, ManualOffCancelsAutomaticRestart) {
    ShortCycleProtection p;
    p.enabled = true;
    p.update(true, true, 24.1, 22);
    p.user_mode_changed();
    EXPECT_EQ(p.update(false, true, 21, 22), ShortCycleProtection::NONE);
}
TEST(ShortCycleProtection, StaleOrInvalidSensorCannotSwitchPower) {
    ShortCycleProtection p;
    p.enabled = true;
    EXPECT_EQ(p.update(true, false, 24, 22), ShortCycleProtection::NONE);
    EXPECT_EQ(p.update(true, true, NAN, 22), ShortCycleProtection::NONE);
    EXPECT_EQ(p.update(true, true, INFINITY, 22), ShortCycleProtection::NONE);
    EXPECT_EQ(p.update(true, true, 24, NAN), ShortCycleProtection::NONE);
    p.update(true, true, 24, 22);
    EXPECT_EQ(p.update(false, false, 21, 22), ShortCycleProtection::NONE);
}
TEST(ShortCycleProtection, DisablingDoesNotRestartAHotRoom) {
    ShortCycleProtection p;
    p.enabled = true;
    p.update(true, true, 24.1, 22);
    p.enabled = false;
    EXPECT_EQ(p.update(false, true, 24, 22), ShortCycleProtection::NONE);
    EXPECT_EQ(p.update(false, true, 22.9, 22), ShortCycleProtection::HEAT);
}
TEST(ShortCycleProtection, TargetChangeUsesNewTarget) {
    ShortCycleProtection p;
    p.enabled = true;
    EXPECT_FLOAT_EQ(p.temperature(true, 23, 23), 23);
    EXPECT_EQ(p.update(true, true, 23.1, 21), ShortCycleProtection::OFF);
    EXPECT_EQ(p.update(false, true, 23, 24), ShortCycleProtection::HEAT);
}
TEST(ShortCycleProtection, RebootDoesNotClaimExistingOff) {
    ShortCycleProtection p;
    p.enabled = true;
    EXPECT_EQ(p.update(false, true, 20, 22), ShortCycleProtection::NONE);
}
