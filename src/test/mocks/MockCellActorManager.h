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

#ifndef MOCK_CELL_ACTOR_MANAGER_H
#define MOCK_CELL_ACTOR_MANAGER_H

#include "GhostActorSystem.h"
#include "gmock/gmock.h"
#include <map>

namespace GhostActor {

/**
 * MockCellActorManager - Test double for CellActorManager
 *
 * Tracks calls to message-sending methods for verification in tests.
 * Allows configuring which entities are in same/different cells.
 *
 * Usage:
 *   MockCellActorManager mgr;
 *   mgr.SetEntityCell(casterGuid, 100);
 *   mgr.SetEntityCell(targetGuid, 101);  // Different cell
 *
 *   // Code under test calls SendSpellHitMessage...
 *
 *   EXPECT_TRUE(mgr.WasSpellHitMessageSent());
 *   EXPECT_EQ(mgr.GetLastSpellHitDamage(), 500);
 */
class MockCellActorManager
{
public:
    MockCellActorManager() = default;

    // Configure which cell an entity is in
    void SetEntityCell(uint64_t guid, uint32_t cellId)
    {
        _entityCells[guid] = cellId;
    }

    // Check if two entities are in the same cell
    bool AreInSameCell(WorldObject const* a, WorldObject const* b) const
    {
        if (!a || !b) return true;  // Null = same cell (safe default)

        uint64_t guidA = a->GetGUID().GetRawValue();
        uint64_t guidB = b->GetGUID().GetRawValue();

        auto itA = _entityCells.find(guidA);
        auto itB = _entityCells.find(guidB);

        // If either not registered, assume same cell
        if (itA == _entityCells.end() || itB == _entityCells.end())
            return true;

        return itA->second == itB->second;
    }

    // Message sending methods - track calls for verification
    void SendSpellHitMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t damage, int32_t healing)
    {
        _spellHitCalled = true;
        _lastSpellHitCasterGuid = caster ? caster->GetGUID().GetRawValue() : 0;
        _lastSpellHitTargetGuid = target ? target->GetGUID().GetRawValue() : 0;
        _lastSpellHitSpellId = spellId;
        _lastSpellHitDamage = damage;
        _lastSpellHitHealing = healing;
        _spellHitCallCount++;
    }

    void SendHealMessage(Unit* healer, Unit* target, uint32_t spellId, int32_t amount)
    {
        _healMessageCalled = true;
        _lastHealCasterGuid = healer ? healer->GetGUID().GetRawValue() : 0;
        _lastHealTargetGuid = target ? target->GetGUID().GetRawValue() : 0;
        _lastHealSpellId = spellId;
        _lastHealAmount = amount;
        _healMessageCallCount++;
    }

    void SendMeleeDamageMessage(Unit* attacker, Unit* target, int32_t damage, bool isCrit)
    {
        _meleeDamageCalled = true;
        _lastMeleeDamage = damage;
        _lastMeleeIsCrit = isCrit;
    }

    // Verification methods
    bool WasSpellHitMessageSent() const { return _spellHitCalled; }
    bool WasHealMessageSent() const { return _healMessageCalled; }
    bool WasMeleeDamageMessageSent() const { return _meleeDamageCalled; }

    int GetSpellHitCallCount() const { return _spellHitCallCount; }
    int GetHealMessageCallCount() const { return _healMessageCallCount; }

    // Get last call parameters
    uint64_t GetLastSpellHitCasterGuid() const { return _lastSpellHitCasterGuid; }
    uint64_t GetLastSpellHitTargetGuid() const { return _lastSpellHitTargetGuid; }
    uint32_t GetLastSpellHitSpellId() const { return _lastSpellHitSpellId; }
    int32_t GetLastSpellHitDamage() const { return _lastSpellHitDamage; }
    int32_t GetLastSpellHitHealing() const { return _lastSpellHitHealing; }

    uint64_t GetLastHealCasterGuid() const { return _lastHealCasterGuid; }
    uint64_t GetLastHealTargetGuid() const { return _lastHealTargetGuid; }
    uint32_t GetLastHealSpellId() const { return _lastHealSpellId; }
    int32_t GetLastHealAmount() const { return _lastHealAmount; }

    // Reset state between tests
    void Reset()
    {
        _entityCells.clear();
        _spellHitCalled = false;
        _healMessageCalled = false;
        _meleeDamageCalled = false;
        _spellHitCallCount = 0;
        _healMessageCallCount = 0;
        _lastSpellHitCasterGuid = 0;
        _lastSpellHitTargetGuid = 0;
        _lastSpellHitSpellId = 0;
        _lastSpellHitDamage = 0;
        _lastSpellHitHealing = 0;
        _lastHealCasterGuid = 0;
        _lastHealTargetGuid = 0;
        _lastHealSpellId = 0;
        _lastHealAmount = 0;
        _lastMeleeDamage = 0;
        _lastMeleeIsCrit = false;
    }

private:
    std::map<uint64_t, uint32_t> _entityCells;

    bool _spellHitCalled = false;
    bool _healMessageCalled = false;
    bool _meleeDamageCalled = false;

    int _spellHitCallCount = 0;
    int _healMessageCallCount = 0;

    uint64_t _lastSpellHitCasterGuid = 0;
    uint64_t _lastSpellHitTargetGuid = 0;
    uint32_t _lastSpellHitSpellId = 0;
    int32_t _lastSpellHitDamage = 0;
    int32_t _lastSpellHitHealing = 0;

    uint64_t _lastHealCasterGuid = 0;
    uint64_t _lastHealTargetGuid = 0;
    uint32_t _lastHealSpellId = 0;
    int32_t _lastHealAmount = 0;

    int32_t _lastMeleeDamage = 0;
    bool _lastMeleeIsCrit = false;
};

} // namespace GhostActor

#endif // MOCK_CELL_ACTOR_MANAGER_H
