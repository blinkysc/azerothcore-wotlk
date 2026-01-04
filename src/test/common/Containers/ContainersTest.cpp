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

#include "Containers.h"
#include "gtest/gtest.h"
#include <map>

struct TestCreatureModel
{
    uint32 CreatureDisplayID;
    float Probability;
};

TEST(ContainersTest, SelectRandomWeightedContainerElement_ZeroProbabilityNeverSelected)
{
    std::vector<TestCreatureModel> models = {
        {1126, 0.0f},   // Probability 0 - should NEVER be selected
        {25628, 1.0f}   // Probability 1 - should ALWAYS be selected
    };

    std::map<uint32, int> selections;

    for (int i = 0; i < 10000; ++i)
    {
        auto selectedItr = Acore::Containers::SelectRandomWeightedContainerElement(models,
            [](TestCreatureModel const& model) {
                return static_cast<double>(model.Probability);
            });
        selections[selectedItr->CreatureDisplayID]++;
    }

    EXPECT_EQ(selections[1126], 0) << "Model with Probability=0 should never be selected";
    EXPECT_EQ(selections[25628], 10000) << "Model with Probability=1 should always be selected when other is 0";
}

TEST(ContainersTest, SelectRandomWeightedContainerElement_AllZeroProbabilityFallbackToEqual)
{
    std::vector<TestCreatureModel> models = {
        {1126, 0.0f},
        {25628, 0.0f}
    };

    std::map<uint32, int> selections;

    for (int i = 0; i < 10000; ++i)
    {
        auto selectedItr = Acore::Containers::SelectRandomWeightedContainerElement(models,
            [](TestCreatureModel const& model) {
                return static_cast<double>(model.Probability);
            });
        selections[selectedItr->CreatureDisplayID]++;
    }

    // When all probabilities are 0, fallback sets all to 1.0 (equal chance)
    // With 10000 iterations, each should be roughly 5000 (allow +-15% tolerance)
    EXPECT_GT(selections[1126], 4000) << "With all zero probabilities, each model should have equal chance";
    EXPECT_LT(selections[1126], 6000);
    EXPECT_GT(selections[25628], 4000);
    EXPECT_LT(selections[25628], 6000);
}

TEST(ContainersTest, SelectRandomWeightedContainerElement_MixedProbabilities)
{
    std::vector<TestCreatureModel> models = {
        {100, 0.0f},   // 0% chance
        {200, 1.0f},   // ~33% chance
        {300, 2.0f}    // ~67% chance
    };

    std::map<uint32, int> selections;

    for (int i = 0; i < 30000; ++i)
    {
        auto selectedItr = Acore::Containers::SelectRandomWeightedContainerElement(models,
            [](TestCreatureModel const& model) {
                return static_cast<double>(model.Probability);
            });
        selections[selectedItr->CreatureDisplayID]++;
    }

    // Model with probability 0 should never be selected
    EXPECT_EQ(selections[100], 0) << "Model with Probability=0 should never be selected";

    // Model 200 should be ~33% (10000 out of 30000), allow +-15%
    EXPECT_GT(selections[200], 8500);
    EXPECT_LT(selections[200], 11500);

    // Model 300 should be ~67% (20000 out of 30000), allow +-15%
    EXPECT_GT(selections[300], 17000);
    EXPECT_LT(selections[300], 23000);
}
