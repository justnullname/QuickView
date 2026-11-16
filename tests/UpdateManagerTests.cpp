#include <gtest/gtest.h>
#include "UpdateManager.h"

TEST(UpdateManagerTest, CompareVersionStringsHandlesPrefixesAndPadding) {
    EXPECT_EQ(UpdateManager::CompareVersionStrings("6.57.0.0", "v6.57.0"), 0);
    EXPECT_EQ(UpdateManager::CompareVersionStrings("V6.40.1", "6.40.1.0"), 0);
    EXPECT_GT(UpdateManager::CompareVersionStrings("6.57.0.0", "6.40.1.1"), 0);
    EXPECT_LT(UpdateManager::CompareVersionStrings("6.40.1", "6.57.0"), 0);
    EXPECT_GT(UpdateManager::CompareVersionStrings("6.57.0.1", "6.57.0"), 0);
}

TEST(UpdateManagerTest, ShouldOfferUpdateNeverDowngradesUnlessChannelSwitched) {
    // Running pre-release 6.57.0.0 against older stable 6.40.1 during normal background check
    EXPECT_FALSE(UpdateManager::ShouldOfferUpdate("6.57.0.0", "6.40.1", false));
    // Equal versions never prompt
    EXPECT_FALSE(UpdateManager::ShouldOfferUpdate("6.57.0.0", "6.57.0", false));
    EXPECT_FALSE(UpdateManager::ShouldOfferUpdate("6.57.0.0", "6.57.0", true));
    // Higher remote always prompts
    EXPECT_TRUE(UpdateManager::ShouldOfferUpdate("6.57.0.0", "6.58.0", false));
    // Explicit channel switch allows downgrade back to stable
    EXPECT_TRUE(UpdateManager::ShouldOfferUpdate("6.57.0.0", "6.40.1", true));
}

TEST(UpdateManagerTest, AutoSwitchToPreReleaseTriggersForFirstRunPreReleaseOnly) {
    // Fresh user or existing stable config opening manually downloaded pre-release 6.57.0 > stable 6.40.1
    EXPECT_TRUE(UpdateManager::ShouldAutoSwitchToPreRelease(0, "6.57.0.0", "", "6.40.1", false));
    EXPECT_TRUE(UpdateManager::ShouldAutoSwitchToPreRelease(0, "6.58.0.0", "6.40.1.1", "6.40.1", false));

    // User manually switched back to stable during this version -> respect user choice
    EXPECT_FALSE(UpdateManager::ShouldAutoSwitchToPreRelease(0, "6.57.0.0", "6.57.0.0", "6.40.1", false));
    EXPECT_FALSE(UpdateManager::ShouldAutoSwitchToPreRelease(0, "6.57.0.0", "6.57.0", "6.40.1", false));

    // Explicit channel switch action never bounces back to pre-release
    EXPECT_FALSE(UpdateManager::ShouldAutoSwitchToPreRelease(0, "6.57.0.0", "", "6.40.1", true));

    // Already on pre-release channel
    EXPECT_FALSE(UpdateManager::ShouldAutoSwitchToPreRelease(1, "6.57.0.0", "", "6.40.1", false));

    // Stable release matching or behind remote stable stays on stable
    EXPECT_FALSE(UpdateManager::ShouldAutoSwitchToPreRelease(0, "6.57.0.0", "", "6.57.0", false));
    EXPECT_FALSE(UpdateManager::ShouldAutoSwitchToPreRelease(0, "6.40.1.0", "", "6.57.0", false));
}
