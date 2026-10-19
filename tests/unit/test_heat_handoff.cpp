#include <gtest/gtest.h>
#include "heat_handoff.h"

using esphome::cn105::HeatHandoff;

TEST(HeatHandoff, ClassifiesOnlyFreshHeatModeRooms) {
    HeatHandoff h;
    h.enabled = true;
    EXPECT_EQ(h.zone(true, true, 21.4f, 22.0f), HeatHandoff::URGENT);
    EXPECT_EQ(h.zone(true, true, 21.5f, 22.0f), HeatHandoff::DEADBAND);
    EXPECT_EQ(h.zone(true, true, 22.5f, 22.0f), HeatHandoff::DEADBAND);
    EXPECT_EQ(h.zone(true, true, 22.51f, 22.0f), HeatHandoff::SATISFIED);
    EXPECT_EQ(h.zone(false, true, 22.0f, 22.0f), HeatHandoff::UNAVAILABLE);
    EXPECT_EQ(h.zone(true, false, 22.0f, 22.0f), HeatHandoff::UNAVAILABLE);
}

TEST(HeatHandoff, AssignmentBiasesOnlyDeadbandRooms) {
    HeatHandoff h;
    h.enabled = true;
    EXPECT_FLOAT_EQ(h.temperature(true, true, 22.2f, 22.0f, true, true), 21.5f);
    EXPECT_FLOAT_EQ(h.temperature(true, true, 22.2f, 22.0f, true, false), 23.0f);
    EXPECT_FLOAT_EQ(h.temperature(true, true, 21.4f, 22.0f, true, false), 21.4f);
    EXPECT_FLOAT_EQ(h.temperature(true, true, 22.6f, 22.0f, true, true), 22.6f);
    EXPECT_FLOAT_EQ(h.temperature(true, true, 22.2f, 22.0f, false, false), 22.2f);
    EXPECT_FLOAT_EQ(h.temperature(false, true, 22.2f, 22.0f, true, true), 22.2f);
}

TEST(HeatHandoff, OwnerReplacementRequiresHoldAndMeaningfulAdvantage) {
    HeatHandoff h;
    h.enabled = true;
    h.minimum_hold_ms = 300000;
    h.switch_margin = 0.3f;
    EXPECT_FALSE(h.may_replace_owner(299999, 0, 0.2f, -0.2f));
    EXPECT_FALSE(h.may_replace_owner(300000, 0, 0.2f, -0.1f));
    EXPECT_TRUE(h.may_replace_owner(300000, 0, 0.2f, -0.11f));
}

TEST(HeatHandoff, TimeComparisonSurvivesMillisWrap) {
    HeatHandoff h;
    h.enabled = true;
    h.minimum_hold_ms = 500;
    EXPECT_FALSE(h.may_replace_owner(0x00000050U, 0xFFFFFF00U, 0.5f, 0.0f));
    EXPECT_TRUE(h.may_replace_owner(0x00000100U, 0xFFFFFF00U, 0.5f, 0.0f));
}

TEST(HeatHandoff, ChoosesColdestRelativeDeadbandRoom) {
    HeatHandoff h;
    h.enabled = true;
    const std::vector<HeatHandoff::Member> members{
        {"attic", HeatHandoff::DEADBAND, 0.2f},
        {"guest", HeatHandoff::DEADBAND, -0.3f},
        {"office", HeatHandoff::SATISFIED, 0.8f},
    };
    EXPECT_EQ(h.choose_owner(0, "", 0, members), "guest");
}

TEST(HeatHandoff, UrgentRoomSuppressesDiscretionaryOwner) {
    HeatHandoff h;
    h.enabled = true;
    const std::vector<HeatHandoff::Member> members{
        {"attic", HeatHandoff::URGENT, -0.6f},
        {"guest", HeatHandoff::DEADBAND, -0.3f},
    };
    EXPECT_TRUE(h.choose_owner(600000, "guest", 0, members).empty());
}

TEST(HeatHandoff, KeepsValidOwnerDuringHoldButReplacesInvalidOwner) {
    HeatHandoff h;
    h.enabled = true;
    h.minimum_hold_ms = 300000;
    h.switch_margin = 0.3f;
    std::vector<HeatHandoff::Member> members{
        {"attic", HeatHandoff::DEADBAND, 0.2f},
        {"guest", HeatHandoff::DEADBAND, -0.3f},
    };
    EXPECT_EQ(h.choose_owner(100000, "attic", 0, members), "attic");
    EXPECT_EQ(h.choose_owner(300000, "attic", 0, members), "guest");
    members[0].zone = HeatHandoff::SATISFIED;
    EXPECT_EQ(h.choose_owner(100000, "attic", 0, members), "guest");
}
