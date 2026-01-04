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

#ifndef CELL_ACTOR_TEST_HARNESS_H
#define CELL_ACTOR_TEST_HARNESS_H

#include "GhostActorSystem.h"
#include "TestCreature.h"
#include "TestPlayer.h"
#include "TestMap.h"
#include "TestObjectAccessor.h"
#include "ScriptMgr.h"
#include "ScriptDefines/AllMapScript.h"
#include "ScriptDefines/MiscScript.h"
#include "ScriptDefines/PlayerScript.h"
#include "ScriptDefines/WorldObjectScript.h"
#include "ScriptDefines/UnitScript.h"
#include <memory>
#include <vector>

using namespace GhostActor;

// ============================================================================
// Payload Factory Functions for Testing
// ============================================================================

inline std::shared_ptr<SpellHitPayload> MakeSpellHitPayload(uint32_t spellId, int32_t damage, int32_t healing = 0)
{
    auto payload = std::make_shared<SpellHitPayload>();
    payload->spellId = spellId;
    payload->damage = damage;
    payload->healing = healing;
    payload->effectMask = 1;
    return payload;
}

inline std::shared_ptr<MeleeDamagePayload> MakeMeleeDamagePayload(int32_t damage, bool isCritical = false)
{
    auto payload = std::make_shared<MeleeDamagePayload>();
    payload->damage = damage;
    payload->isCritical = isCritical;
    return payload;
}

inline std::shared_ptr<HealPayload> MakeHealPayload(uint32_t spellId, int32_t amount)
{
    auto payload = std::make_shared<HealPayload>();
    payload->spellId = spellId;
    payload->healAmount = amount;
    payload->effectiveHeal = amount;
    return payload;
}

inline std::shared_ptr<ThreatUpdatePayload> MakeThreatUpdatePayload(uint64_t attacker, uint64_t victim, float delta)
{
    auto payload = std::make_shared<ThreatUpdatePayload>();
    payload->attackerGuid = attacker;
    payload->victimGuid = victim;
    payload->threatDelta = delta;
    payload->isNewThreat = true;
    return payload;
}

inline std::shared_ptr<ThreatUpdatePayload> MakeThreatPercentPayload(uint64_t attacker, uint64_t victim, int32_t percent)
{
    auto payload = std::make_shared<ThreatUpdatePayload>();
    payload->attackerGuid = attacker;
    payload->victimGuid = victim;
    payload->threatDelta = 0.0f;
    payload->percentModify = percent;
    payload->isNewThreat = false;
    payload->isRemoval = false;
    return payload;
}

inline std::shared_ptr<AggroRequestPayload> MakeAggroRequestPayload(uint64_t creature, float x, float y, float z, float range)
{
    auto payload = std::make_shared<AggroRequestPayload>();
    payload->creatureGuid = creature;
    payload->creatureX = x;
    payload->creatureY = y;
    payload->creatureZ = z;
    payload->maxRange = range;
    payload->initialThreat = 1.0f;
    return payload;
}

inline std::shared_ptr<CombatInitiatedPayload> MakeCombatInitiatedPayload(uint64_t entity, uint64_t attacker, float threat)
{
    auto payload = std::make_shared<CombatInitiatedPayload>();
    payload->entityGuid = entity;
    payload->attackerGuid = attacker;
    payload->threatAmount = threat;
    return payload;
}

inline std::shared_ptr<AssistanceRequestPayload> MakeAssistanceRequestPayload(uint64_t caller, uint64_t target, float x, float y, float z, float radius)
{
    auto payload = std::make_shared<AssistanceRequestPayload>();
    payload->callerGuid = caller;
    payload->targetGuid = target;
    payload->callerX = x;
    payload->callerY = y;
    payload->callerZ = z;
    payload->radius = radius;
    return payload;
}

inline std::shared_ptr<TargetSwitchPayload> MakeTargetSwitchPayload(uint64_t creature, uint64_t oldTarget, uint64_t newTarget)
{
    auto payload = std::make_shared<TargetSwitchPayload>();
    payload->creatureGuid = creature;
    payload->oldTargetGuid = oldTarget;
    payload->newTargetGuid = newTarget;
    return payload;
}

inline std::shared_ptr<PetRemovalPayload> MakePetRemovalPayload(uint64_t pet, uint64_t owner, uint8_t saveMode = 0)
{
    auto payload = std::make_shared<PetRemovalPayload>();
    payload->petGuid = pet;
    payload->ownerGuid = owner;
    payload->saveMode = saveMode;
    return payload;
}

inline std::shared_ptr<GhostSnapshot> MakeGhostSnapshot(uint64_t guid, float x = 0, float y = 0, float z = 0, uint32_t health = 1000)
{
    auto snapshot = std::make_shared<GhostSnapshot>();
    snapshot->guid = guid;
    snapshot->posX = x;
    snapshot->posY = y;
    snapshot->posZ = z;
    snapshot->health = health;
    snapshot->maxHealth = health;
    return snapshot;
}

inline std::shared_ptr<MigrationRequestPayload> MakeMigrationRequestPayload(uint64_t entityGuid, uint64_t migrationId = 1)
{
    auto payload = std::make_shared<MigrationRequestPayload>();
    payload->migrationId = migrationId;
    payload->snapshot.guid = entityGuid;
    return payload;
}

inline std::shared_ptr<MigrationAckPayload> MakeMigrationAckPayload(uint64_t migrationId, bool accepted = true)
{
    auto payload = std::make_shared<MigrationAckPayload>();
    payload->migrationId = migrationId;
    payload->accepted = accepted;
    return payload;
}

inline std::shared_ptr<SpellInterruptPayload> MakeSpellInterruptPayload(uint64_t caster, uint64_t target,
    uint32_t interruptSpellId, uint32_t interruptedSpellId, uint32_t schoolMask = 1, int32_t lockoutDuration = 4000)
{
    auto payload = std::make_shared<SpellInterruptPayload>();
    payload->casterGuid = caster;
    payload->targetGuid = target;
    payload->interruptSpellId = interruptSpellId;
    payload->interruptedSpellId = interruptedSpellId;
    payload->schoolMask = schoolMask;
    payload->lockoutDuration = lockoutDuration;
    return payload;
}

inline std::shared_ptr<SpellDispelPayload> MakeSpellDispelPayload(uint64_t caster, uint64_t target,
    uint32_t dispelSpellId, const std::vector<std::pair<uint32_t, uint8_t>>& dispelList)
{
    auto payload = std::make_shared<SpellDispelPayload>();
    payload->casterGuid = caster;
    payload->targetGuid = target;
    payload->dispelSpellId = dispelSpellId;
    payload->dispelList = dispelList;
    return payload;
}

inline std::shared_ptr<PowerDrainPayload> MakePowerDrainPayload(uint64_t caster, uint64_t target,
    uint32_t spellId, uint8_t powerType, int32_t amount, float gainMultiplier = 1.0f, bool isPowerBurn = false)
{
    auto payload = std::make_shared<PowerDrainPayload>();
    payload->casterGuid = caster;
    payload->targetGuid = target;
    payload->spellId = spellId;
    payload->powerType = powerType;
    payload->amount = amount;
    payload->gainMultiplier = gainMultiplier;
    payload->isPowerBurn = isPowerBurn;
    return payload;
}

inline std::shared_ptr<SpellstealPayload> MakeSpellstealPayload(uint64_t caster, uint64_t target,
    uint32_t spellstealSpellId, const std::vector<std::pair<uint32_t, uint64_t>>& stealList)
{
    auto payload = std::make_shared<SpellstealPayload>();
    payload->casterGuid = caster;
    payload->targetGuid = target;
    payload->spellstealSpellId = spellstealSpellId;
    payload->stealList = stealList;
    return payload;
}

inline std::shared_ptr<SpellstealApplyPayload> MakeSpellstealApplyPayload(uint64_t stealer, uint64_t target,
    uint32_t spellstealSpellId, const std::vector<StolenAuraData>& stolenAuras)
{
    auto payload = std::make_shared<SpellstealApplyPayload>();
    payload->stealerGuid = stealer;
    payload->targetGuid = target;
    payload->spellstealSpellId = spellstealSpellId;
    payload->stolenAuras = stolenAuras;
    return payload;
}

inline std::shared_ptr<ControlEffectPayload> MakeControlEffectPayload(uint64_t caster, uint64_t target,
    uint32_t spellId, int32_t duration, uint32_t controlType, uint8_t auraType = 0)
{
    auto payload = std::make_shared<ControlEffectPayload>();
    payload->casterGuid = caster;
    payload->targetGuid = target;
    payload->spellId = spellId;
    payload->duration = duration;
    payload->controlType = controlType;
    payload->auraType = auraType;
    return payload;
}

inline std::shared_ptr<ControlEffectPayload> MakePolymorphPayload(uint64_t caster, uint64_t target,
    uint32_t spellId, int32_t duration, uint32_t displayId)
{
    auto payload = std::make_shared<ControlEffectPayload>();
    payload->casterGuid = caster;
    payload->targetGuid = target;
    payload->spellId = spellId;
    payload->duration = duration;
    payload->controlType = 0;  // Polymorph uses aura, not unit state
    payload->auraType = 0;     // SPELL_AURA_MOD_CONFUSE equivalent
    payload->transformDisplayId = displayId;
    return payload;
}

inline std::shared_ptr<ControlEffectPayload> MakeFearPayload(uint64_t caster, uint64_t target,
    uint32_t spellId, int32_t duration, float destX = 0.0f, float destY = 0.0f, float destZ = 0.0f)
{
    auto payload = std::make_shared<ControlEffectPayload>();
    payload->casterGuid = caster;
    payload->targetGuid = target;
    payload->spellId = spellId;
    payload->duration = duration;
    payload->controlType = 0;  // Fear uses fleeing state
    payload->auraType = 0;
    payload->fearDestX = destX;
    payload->fearDestY = destY;
    payload->fearDestZ = destZ;
    return payload;
}

inline std::shared_ptr<KnockbackPayload> MakeKnockbackPayload(uint64_t caster, uint64_t target,
    uint32_t spellId, float originX, float originY, float originZ,
    float speedXY, float speedZ, float destX = 0.0f, float destY = 0.0f, float destZ = 0.0f)
{
    auto payload = std::make_shared<KnockbackPayload>();
    payload->casterGuid = caster;
    payload->targetGuid = target;
    payload->spellId = spellId;
    payload->originX = originX;
    payload->originY = originY;
    payload->originZ = originZ;
    payload->speedXY = speedXY;
    payload->speedZ = speedZ;
    payload->destX = destX;
    payload->destY = destY;
    payload->destZ = destZ;
    return payload;
}

// Initialize script registries required for object construction and map destruction
inline void EnsureCellActorTestScriptsInitialized()
{
    static bool initialized = false;
    if (!initialized)
    {
        ScriptRegistry<AllMapScript>::InitEnabledHooksIfNeeded(ALLMAPHOOK_END);
        ScriptRegistry<MiscScript>::InitEnabledHooksIfNeeded(MISCHOOK_END);
        ScriptRegistry<WorldObjectScript>::InitEnabledHooksIfNeeded(WORLDOBJECTHOOK_END);
        ScriptRegistry<UnitScript>::InitEnabledHooksIfNeeded(UNITHOOK_END);
        ScriptRegistry<PlayerScript>::InitEnabledHooksIfNeeded(PLAYERHOOK_END);
        initialized = true;
    }
}

/**
 * CellActorTestHarness - Test fixture for CellActor cross-cell interaction tests
 *
 * Manages test entities (TestCreature, TestPlayer) and provides utilities
 * for injecting messages and simulating entity deletion scenarios.
 *
 * Usage:
 *   CellActorTestHarness harness;
 *   auto* creature = harness.AddCreature(1001, 12345);
 *   auto* player = harness.AddPlayer(1);
 *
 *   ActorMessage msg;
 *   msg.type = MessageType::SPELL_HIT;
 *   msg.sourceGuid = player->GetGUID().GetRawValue();
 *   msg.targetGuid = creature->GetGUID().GetRawValue();
 *   harness.InjectMessage(msg);
 *
 *   harness.DeleteEntity(creature->GetGUID());  // Mark deleted
 *   harness.ProcessAllMessages();               // Should handle gracefully
 */
class CellActorTestHarness
{
public:
    CellActorTestHarness(uint32_t cellId = 0)
        : _cellId(cellId)
    {
        // Initialize script registries for object construction
        EnsureCellActorTestScriptsInitialized();

        // Create TestMap with CellActorManager (initializes DBC stores too)
        _map = std::make_unique<TestMap>(0, 0);

        // Create CellActor with real map for full ghost system integration
        _cell = std::make_unique<CellActor>(_cellId, _map.get());
    }

    ~CellActorTestHarness() = default;

    // Add a test creature and register with accessor
    TestCreature* AddCreature(ObjectGuid::LowType guid, uint32 entry = 0)
    {
        auto creature = std::make_unique<TestCreature>();
        creature->ForceInit(guid, entry);

        TestCreature* ptr = creature.get();
        _creatures.push_back(std::move(creature));

        // Set map on entity (enables FindMap() to work for ghost notifications)
        _map->AddTestEntity(ptr);

        // Register with accessor and cell
        _accessor.Register(ptr);
        _cell->AddEntity(ptr);

        return ptr;
    }

    // Add a test player and register with accessor
    TestPlayer* AddPlayer(ObjectGuid::LowType guid)
    {
        auto player = std::make_unique<TestPlayer>();
        player->ForceInit(guid);

        TestPlayer* ptr = player.get();
        _players.push_back(std::move(player));

        // Set map on entity (enables FindMap() to work for ghost notifications)
        _map->AddTestEntity(ptr);

        // Register with accessor and cell
        _accessor.Register(ptr);
        _cell->AddEntity(ptr);

        return ptr;
    }

    // Mark entity as deleted (simulates despawn without memory deallocation)
    void DeleteEntity(ObjectGuid guid)
    {
        _accessor.MarkDeleted(guid);

        // Find and mark the actual entity
        for (auto& creature : _creatures)
        {
            if (creature->GetGUID() == guid)
            {
                creature->MarkTestDeleted();
                _cell->RemoveEntity(creature.get());
                return;
            }
        }

        for (auto& player : _players)
        {
            if (player->GetGUID() == guid)
            {
                player->MarkTestDeleted();
                _cell->RemoveEntity(player.get());
                return;
            }
        }
    }

    // Inject a message into the cell's inbox
    void InjectMessage(ActorMessage msg)
    {
        _cell->SendMessage(std::move(msg));
    }

    // Process all pending messages via Update
    void ProcessAllMessages()
    {
        _cell->Update(0);  // diff=0 just processes messages
    }

    // Get the cell actor for direct access
    CellActor* GetCell() { return _cell.get(); }

    // Get the test object accessor
    TestObjectAccessor& GetAccessor() { return _accessor; }

    // Get entity counts
    size_t CreatureCount() const { return _creatures.size(); }
    size_t PlayerCount() const { return _players.size(); }
    size_t TotalEntityCount() const { return _creatures.size() + _players.size(); }

    // Find entity by GUID
    WorldObject* FindEntity(ObjectGuid guid)
    {
        return _accessor.FindWorldObject(guid);
    }

    // Create a ghost in the cell
    void AddGhost(uint64_t guid, uint32_t ownerCellId)
    {
        GhostSnapshot snapshot;
        snapshot.guid = guid;
        snapshot.posX = 0.0f;
        snapshot.posY = 0.0f;
        snapshot.posZ = 0.0f;
        snapshot.health = 1000;
        snapshot.maxHealth = 1000;

        ActorMessage msg{};
        msg.type = MessageType::GHOST_CREATE;
        msg.sourceGuid = guid;
        msg.sourceCellId = ownerCellId;
        msg.complexPayload = std::make_shared<GhostSnapshot>(snapshot);

        _cell->SendMessage(std::move(msg));
        _cell->Update(0);  // Process immediately
    }

    // Send a ghost destroy message
    void DestroyGhost(uint64_t guid)
    {
        ActorMessage msg{};
        msg.type = MessageType::GHOST_DESTROY;
        msg.sourceGuid = guid;

        _cell->SendMessage(std::move(msg));
        _cell->Update(0);  // Process immediately
    }

    // Get the test map
    TestMap* GetMap() { return _map.get(); }

    // Get performance stats for counter verification
    GhostActor::PerformanceStats* GetPerfStats()
    {
        if (_map && _map->GetCellActorManager())
            return &_map->GetCellActorManager()->GetPerfStats();
        return nullptr;
    }

private:
    uint32_t _cellId;
    std::unique_ptr<TestMap> _map;
    std::unique_ptr<CellActor> _cell;
    std::vector<std::unique_ptr<TestCreature>> _creatures;
    std::vector<std::unique_ptr<TestPlayer>> _players;
    TestObjectAccessor _accessor;
};

#endif // CELL_ACTOR_TEST_HARNESS_H
