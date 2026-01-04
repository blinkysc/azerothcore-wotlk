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

#ifndef TEST_PLAYER_H
#define TEST_PLAYER_H

#include "Player.h"
#include "ObjectGuid.h"
#include "TestWorldSession.h"

/**
 * TestPlayer - A Player subclass for unit testing
 *
 * Inherits from real Player class but overrides heavy methods
 * to no-op for testing without database or network.
 *
 * Usage:
 *   TestPlayer player;
 *   player.ForceInit(1);         // guid=1
 *   player.SetHealth(10000);
 */
class TestPlayer : public Player
{
public:
    TestPlayer() : Player(&_testSession) {}

    // Override heavy methods to no-op
    void UpdateObjectVisibility(bool /*forced*/ = true, bool /*fromUpdate*/ = false) override { }

    // Override IsInWorld to return our test state
    [[nodiscard]] bool IsInWorld() const override { return _testInWorld; }

    // Force initialization without database
    void ForceInit(ObjectGuid::LowType guidLow = 1)
    {
        Object::_Create(guidLow, uint32(0), HighGuid::Player);

        // Set values directly without triggering UpdateSpeed and other side effects
        SetUInt32Value(UNIT_FIELD_MAXHEALTH, 10000);
        SetUInt32Value(UNIT_FIELD_HEALTH, 10000);
        SetUInt32Value(UNIT_FIELD_LEVEL, 80);

        // Mark as alive for handler checks
        // Note: Don't set _testInWorld here - let AddToWorld() handle it
        m_deathState = DeathState::Alive;
    }

    // Override AddToWorld/RemoveFromWorld to control our test in-world state
    // Don't call base class - they require infrastructure we don't have
    void AddToWorld() override
    {
        _testInWorld = true;
    }

    void RemoveFromWorld() override
    {
        _testInWorld = false;
    }

    // Allow tests to control in-world state
    void SetTestInWorld(bool inWorld) { _testInWorld = inWorld; }

    // Safe health getter for testing
    uint32 GetTestHealth() const
    {
        return GetUInt32Value(UNIT_FIELD_HEALTH);
    }

    // Safe max health setter
    void SetTestMaxHealth(uint32 val)
    {
        SetUInt32Value(UNIT_FIELD_MAXHEALTH, val);
    }

    // Safe health setter that doesn't trigger side effects
    // Also updates max health if needed to allow setting higher values
    void SetTestHealth(uint32 val)
    {
        if (val > GetUInt32Value(UNIT_FIELD_MAXHEALTH))
        {
            SetUInt32Value(UNIT_FIELD_MAXHEALTH, val);
        }
        SetUInt32Value(UNIT_FIELD_HEALTH, val);
    }

    // Track validity for segfault tests
    bool _testMarkedDeleted = false;

    void MarkTestDeleted()
    {
        _testMarkedDeleted = true;
    }

    bool IsTestDeleted() const { return _testMarkedDeleted; }

private:
    TestWorldSession _testSession;
    bool _testInWorld = false;
};

#endif // TEST_PLAYER_H
