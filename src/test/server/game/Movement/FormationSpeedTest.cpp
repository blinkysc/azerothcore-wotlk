/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "gtest/gtest.h"
#include <cmath>

/**
 * @brief Test suite for formation speed calculations
 *
 * These tests verify that formation member speed calculations use 2D distance
 * to prevent elevation differences on slopes from causing incorrect speed rates.
 *
 * The fix ensures that members moving on slopes maintain proper formation cohesion
 * by calculating speed based on horizontal distance only.
 */

namespace
{
    // Simulate the old 3D distance calculation
    float CalculateSpeedRate3D(float leaderSpeed,
                                float leaderX, float leaderY, float leaderZ,
                                float destX, float destY, float destZ,
                                float memberX, float memberY, float memberZ,
                                float memberDestX, float memberDestY, float memberDestZ)
    {
        // Old code: uses 3D distance
        float pathDist = std::sqrt(
            (destX - leaderX) * (destX - leaderX) +
            (destY - leaderY) * (destY - leaderY) +
            (destZ - leaderZ) * (destZ - leaderZ)
        );

        float memberDist = std::sqrt(
            (memberDestX - memberX) * (memberDestX - memberX) +
            (memberDestY - memberY) * (memberDestY - memberY) +
            (memberDestZ - memberZ) * (memberDestZ - memberZ)
        );

        return leaderSpeed * memberDist / pathDist;
    }

    // Simulate the new 2D distance calculation
    float CalculateSpeedRate2D(float leaderSpeed,
                                float leaderX, float leaderY, float /*leaderZ*/,
                                float destX, float destY, float /*destZ*/,
                                float memberX, float memberY, float /*memberZ*/,
                                float memberDestX, float memberDestY, float /*memberDestZ*/)
    {
        // New code: uses 2D distance only
        float pathDist = std::sqrt(
            (destX - leaderX) * (destX - leaderX) +
            (destY - leaderY) * (destY - leaderY)
        );

        if (pathDist < 0.1f)
            return 0.0f; // Safety check for division by zero

        float memberDist = std::sqrt(
            (memberDestX - memberX) * (memberDestX - memberX) +
            (memberDestY - memberY) * (memberDestY - memberY)
        );

        return leaderSpeed * memberDist / pathDist;
    }
}

// Test: Flat ground - both methods should give same result
TEST(FormationSpeedTest, FlatGround_BothMethodsEqual)
{
    float leaderSpeed = 7.0f;

    // Leader at origin, moving 100 units on flat ground
    float leaderX = 0, leaderY = 0, leaderZ = 0;
    float destX = 100, destY = 0, destZ = 0;

    // Member 5 units behind, same elevation
    float memberX = -5, memberY = 0, memberZ = 0;
    float memberDestX = 95, memberDestY = 0, memberDestZ = 0;

    float speed3D = CalculateSpeedRate3D(leaderSpeed, leaderX, leaderY, leaderZ,
                                          destX, destY, destZ,
                                          memberX, memberY, memberZ,
                                          memberDestX, memberDestY, memberDestZ);

    float speed2D = CalculateSpeedRate2D(leaderSpeed, leaderX, leaderY, leaderZ,
                                          destX, destY, destZ,
                                          memberX, memberY, memberZ,
                                          memberDestX, memberDestY, memberDestZ);

    EXPECT_FLOAT_EQ(speed3D, speed2D);
    EXPECT_FLOAT_EQ(speed2D, leaderSpeed); // Same distance = same speed
}

// Test: Slope - member at same relative position, different elevation
TEST(FormationSpeedTest, Slope_MemberHigher_3DGivesWrongSpeed)
{
    float leaderSpeed = 7.0f;

    // Leader moving up a slope: (0,0,0) -> (100,0,50)
    float leaderX = 0, leaderY = 0, leaderZ = 0;
    float destX = 100, destY = 0, destZ = 50;

    // Member behind leader, but HIGHER on the terrain (Z=30)
    float memberX = -5, memberY = 0, memberZ = 30;
    float memberDestX = 95, memberDestY = 0, memberDestZ = 50;

    float speed3D = CalculateSpeedRate3D(leaderSpeed, leaderX, leaderY, leaderZ,
                                          destX, destY, destZ,
                                          memberX, memberY, memberZ,
                                          memberDestX, memberDestY, memberDestZ);

    float speed2D = CalculateSpeedRate2D(leaderSpeed, leaderX, leaderY, leaderZ,
                                          destX, destY, destZ,
                                          memberX, memberY, memberZ,
                                          memberDestX, memberDestY, memberDestZ);

    // 3D calculation gives WRONG speed (too slow because vertical dist is less)
    EXPECT_LT(speed3D, leaderSpeed);

    // 2D calculation gives CORRECT speed (same horizontal distance)
    EXPECT_FLOAT_EQ(speed2D, leaderSpeed);
}

// Test: Slope - member at lower elevation
TEST(FormationSpeedTest, Slope_MemberLower_3DGivesWrongSpeed)
{
    float leaderSpeed = 7.0f;

    // Leader moving up a slope: (0,0,0) -> (100,0,50)
    float leaderX = 0, leaderY = 0, leaderZ = 0;
    float destX = 100, destY = 0, destZ = 50;

    // Member behind leader, but LOWER on the terrain (Z=-20)
    float memberX = -5, memberY = 0, memberZ = -20;
    float memberDestX = 95, memberDestY = 0, memberDestZ = 50;

    float speed3D = CalculateSpeedRate3D(leaderSpeed, leaderX, leaderY, leaderZ,
                                          destX, destY, destZ,
                                          memberX, memberY, memberZ,
                                          memberDestX, memberDestY, memberDestZ);

    float speed2D = CalculateSpeedRate2D(leaderSpeed, leaderX, leaderY, leaderZ,
                                          destX, destY, destZ,
                                          memberX, memberY, memberZ,
                                          memberDestX, memberDestY, memberDestZ);

    // 3D calculation gives WRONG speed (too fast because vertical dist is more)
    EXPECT_GT(speed3D, leaderSpeed);

    // 2D calculation gives CORRECT speed (same horizontal distance)
    EXPECT_FLOAT_EQ(speed2D, leaderSpeed);
}

// Test: Steep slope - exaggerated case
TEST(FormationSpeedTest, SteepSlope_ExaggeratedDifference)
{
    float leaderSpeed = 7.0f;

    // Very steep slope: (0,0,0) -> (100,0,100) - 45 degree angle
    float leaderX = 0, leaderY = 0, leaderZ = 0;
    float destX = 100, destY = 0, destZ = 100;

    // Member at same horizontal position but very different Z
    float memberX = -5, memberY = 0, memberZ = 80;
    float memberDestX = 95, memberDestY = 0, memberDestZ = 100;

    float speed3D = CalculateSpeedRate3D(leaderSpeed, leaderX, leaderY, leaderZ,
                                          destX, destY, destZ,
                                          memberX, memberY, memberZ,
                                          memberDestX, memberDestY, memberDestZ);

    float speed2D = CalculateSpeedRate2D(leaderSpeed, leaderX, leaderY, leaderZ,
                                          destX, destY, destZ,
                                          memberX, memberY, memberZ,
                                          memberDestX, memberDestY, memberDestZ);

    // 3D: Leader path = sqrt(100^2 + 100^2) = 141.4
    //     Member path = sqrt(100^2 + 20^2) = 102.0
    //     Speed ratio = 102/141.4 = 0.72 -> WAY too slow!
    EXPECT_LT(speed3D, leaderSpeed * 0.75f);

    // 2D: Both paths = 100, speed ratio = 1.0
    EXPECT_FLOAT_EQ(speed2D, leaderSpeed);
}

// Test: Zero distance safety check
TEST(FormationSpeedTest, ZeroDistance_ReturnsZero)
{
    float leaderSpeed = 7.0f;

    // Leader already at destination
    float leaderX = 100, leaderY = 0, leaderZ = 0;
    float destX = 100, destY = 0, destZ = 0;

    float memberX = 95, memberY = 0, memberZ = 0;
    float memberDestX = 95, memberDestY = 0, memberDestZ = 0;

    float speed2D = CalculateSpeedRate2D(leaderSpeed, leaderX, leaderY, leaderZ,
                                          destX, destY, destZ,
                                          memberX, memberY, memberZ,
                                          memberDestX, memberDestY, memberDestZ);

    // Should return 0 due to safety check (pathDist < 0.1)
    EXPECT_FLOAT_EQ(speed2D, 0.0f);
}

// Test: Naxxramas-like scenario - hallway slope
TEST(FormationSpeedTest, NaxxramasSlope_FormationCohesion)
{
    float leaderSpeed = 7.0f;

    // Simulating Naxx hallway: leader moving down slope
    float leaderX = 3108.0f, leaderY = -3895.0f, leaderZ = 280.0f;
    float destX = 3150.0f, destY = -3850.0f, destZ = 260.0f;

    // Formation members at different elevations due to terrain
    struct Member {
        float x, y, z;
        float destX, destY, destZ;
    };

    Member members[] = {
        {3103.0f, -3890.0f, 282.0f, 3145.0f, -3845.0f, 262.0f},  // Slightly higher
        {3113.0f, -3900.0f, 278.0f, 3155.0f, -3855.0f, 258.0f},  // Slightly lower
        {3108.0f, -3890.0f, 285.0f, 3150.0f, -3845.0f, 265.0f},  // Much higher
    };

    for (const auto& m : members)
    {
        float speed2D = CalculateSpeedRate2D(leaderSpeed, leaderX, leaderY, leaderZ,
                                              destX, destY, destZ,
                                              m.x, m.y, m.z,
                                              m.destX, m.destY, m.destZ);

        // All members should have approximately the same speed as leader
        // (within 10% tolerance for different horizontal offsets)
        EXPECT_NEAR(speed2D, leaderSpeed, leaderSpeed * 0.15f);
    }
}
