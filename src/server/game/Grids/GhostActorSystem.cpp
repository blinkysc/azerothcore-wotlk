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

#include "GhostActorSystem.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "ThreatMgr.h"
#include "Unit.h"
#include "WorkStealingPool.h"
#include "World.h"
#include <thread>

namespace GhostActor
{

// ============================================================================
// CellActor Implementation
// ============================================================================

void CellActor::Update(uint32_t diff)
{
    _lastUpdateTime += diff;
    ProcessMessages();
    UpdateEntities(diff);
}

void CellActor::ProcessMessages()
{
    ActorMessage msg;
    while (_inbox.Pop(msg))
    {
        HandleMessage(msg);
    }
}

void CellActor::HandleMessage(ActorMessage& msg)
{
    IncrementMessageCount();

    switch (msg.type)
    {
        case MessageType::SPELL_HIT:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<SpellHitPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: SPELL_HIT spell={} target={} damage={} healing={}",
                    _cellId, payload->spellId, msg.targetGuid, payload->damage, payload->healing);

                WorldObject* targetObj = FindEntityByGuid(msg.targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

                if (target && target->IsAlive() && target->IsInWorld())
                {
                    // Apply damage if present
                    if (payload->damage > 0)
                    {
                        Unit::DealDamage(nullptr, target, payload->damage, nullptr,
                            SPELL_DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                    }
                    // Apply healing if present
                    if (payload->healing > 0)
                    {
                        target->ModifyHealth(payload->healing);
                    }
                    // Broadcast health change to ghosts in neighboring cells
                    BroadcastHealthChange(target);
                }
            }
            break;
        }

        case MessageType::MELEE_DAMAGE:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<MeleeDamagePayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: MELEE_DAMAGE target={} damage={} crit={}",
                    _cellId, msg.targetGuid, payload->damage, payload->isCritical);

                WorldObject* targetObj = FindEntityByGuid(msg.targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

                if (target && target->IsAlive() && target->IsInWorld())
                {
                    Unit::DealDamage(nullptr, target, payload->damage, nullptr,
                        DIRECT_DAMAGE, SPELL_SCHOOL_MASK_NORMAL, nullptr, false);
                    BroadcastHealthChange(target);
                }
            }
            break;
        }

        case MessageType::HEAL:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<HealPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: HEAL spell={} target={} amount={}",
                    _cellId, payload->spellId, msg.targetGuid, payload->healAmount);

                WorldObject* targetObj = FindEntityByGuid(msg.targetGuid);
                Unit* target = targetObj ? targetObj->ToUnit() : nullptr;

                if (target && target->IsAlive() && target->IsInWorld())
                {
                    target->ModifyHealth(payload->effectiveHeal);
                    BroadcastHealthChange(target);
                }
            }
            break;
        }

        case MessageType::ENTITY_ENTERING:
        {
            // Entity is transferring to this cell
            LOG_DEBUG("server.ghost", "CellActor[{}]: Entity {} entering from cell {}",
                _cellId, msg.sourceGuid, msg.sourceCellId);
            break;
        }

        case MessageType::ENTITY_LEAVING:
        {
            // Entity is leaving this cell
            LOG_DEBUG("server.ghost", "CellActor[{}]: Entity {} leaving to cell {}",
                _cellId, msg.sourceGuid, msg.targetCellId);
            break;
        }

        case MessageType::POSITION_UPDATE:
        {
            // Update ghost position
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncPosition(msg.floatParam1, msg.floatParam2, msg.floatParam3, 0.0f);
            }
            break;
        }

        case MessageType::HEALTH_CHANGED:
        {
            // Update ghost's health display
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncHealth(
                    static_cast<uint32_t>(msg.intParam1),
                    static_cast<uint32_t>(msg.intParam2));
            }
            break;
        }

        case MessageType::COMBAT_STATE_CHANGED:
        {
            // Update ghost's combat state
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncCombatState(msg.intParam1 != 0);
            }
            break;
        }

        case MessageType::GHOST_CREATE:
        {
            // Create a ghost for an entity in neighboring cell
            if (_ghosts.find(msg.sourceGuid) == _ghosts.end())
            {
                auto ghost = std::make_unique<GhostEntity>(msg.sourceGuid, msg.sourceCellId);

                // If we have a full snapshot in complexPayload, use it
                if (msg.complexPayload)
                {
                    auto snapshot = std::static_pointer_cast<GhostSnapshot>(msg.complexPayload);
                    ghost->SyncFromSnapshot(*snapshot);
                }
                else
                {
                    // Use basic params
                    ghost->SyncPosition(msg.floatParam1, msg.floatParam2, msg.floatParam3, 0.0f);
                    ghost->SyncHealth(
                        static_cast<uint32_t>(msg.intParam1),
                        static_cast<uint32_t>(msg.intParam2));
                }

                _ghosts[msg.sourceGuid] = std::move(ghost);
            }
            break;
        }

        case MessageType::GHOST_UPDATE:
        {
            // Full state sync for a ghost
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end() && msg.complexPayload)
            {
                auto snapshot = std::static_pointer_cast<GhostSnapshot>(msg.complexPayload);
                it->second->SyncFromSnapshot(*snapshot);
            }
            break;
        }

        case MessageType::GHOST_DESTROY:
        {
            // Remove ghost - entity no longer visible
            _ghosts.erase(msg.sourceGuid);
            break;
        }

        case MessageType::AURA_APPLY:
        {
            // Aura visual sync - spellId in intParam1, effectMask in intParam2
            LOG_DEBUG("server.ghost", "CellActor[{}]: AURA_APPLY entity={} spell={}",
                _cellId, msg.sourceGuid, msg.intParam1);
            break;
        }

        case MessageType::AURA_REMOVE:
        {
            LOG_DEBUG("server.ghost", "CellActor[{}]: AURA_REMOVE entity={} spell={}",
                _cellId, msg.sourceGuid, msg.intParam1);
            break;
        }

        case MessageType::POWER_CHANGED:
        {
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                // Power type in intParam1, value in intParam2, max in intParam3
                it->second->SyncPower(static_cast<uint8_t>(msg.intParam1),
                    static_cast<uint32_t>(msg.intParam2),
                    static_cast<uint32_t>(msg.intParam3));
            }
            break;
        }

        case MessageType::AURA_STATE_SYNC:
        {
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncAuraState(static_cast<uint32_t>(msg.intParam1));
            }
            break;
        }

        case MessageType::PHASE_CHANGED:
        {
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncPhaseMask(static_cast<uint32_t>(msg.intParam1));
            }
            break;
        }

        case MessageType::MIGRATION_REQUEST:
        case MessageType::MIGRATION_ACK:
        case MessageType::MIGRATION_COMPLETE:
        case MessageType::MIGRATION_FORWARD:
            break;

        case MessageType::THREAT_UPDATE:
        {
            // Handle cross-cell threat modification
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<ThreatUpdatePayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: THREAT_UPDATE attacker={} victim={} delta={:.1f} new={} remove={}",
                    _cellId, payload->attackerGuid, payload->victimGuid,
                    payload->threatDelta, payload->isNewThreat, payload->isRemoval);

                // Find the attacker (creature) in this cell - threat list is on the creature
                WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
                if (!attackerObj)
                {
                    LOG_DEBUG("server.ghost", "CellActor[{}]: THREAT_UPDATE - attacker {} not found in this cell",
                        _cellId, payload->attackerGuid);
                    break;
                }

                Creature* creature = attackerObj->ToCreature();
                if (!creature)
                {
                    LOG_DEBUG("server.ghost", "CellActor[{}]: THREAT_UPDATE - attacker {} is not a creature",
                        _cellId, payload->attackerGuid);
                    break;
                }

                // We need to find the victim to apply threat
                // First check local entities
                WorldObject* victimObj = FindEntityByGuid(payload->victimGuid);
                Unit* victim = victimObj ? victimObj->ToUnit() : nullptr;

                // If victim not in this cell, try to get it via map
                if (!victim && _map)
                {
                    victim = ObjectAccessor::GetUnit(*creature, ObjectGuid(payload->victimGuid));
                }

                if (victim)
                {
                    if (payload->percentModify != 0)
                    {
                        creature->GetThreatMgr().ModifyThreatByPercent(victim, payload->percentModify);
                        LOG_DEBUG("server.ghost", "CellActor[{}]: Modified threat by {}% for victim {} on creature {}",
                            _cellId, payload->percentModify, payload->victimGuid, payload->attackerGuid);
                    }
                    else if (payload->isRemoval)
                    {
                        creature->GetThreatMgr().ModifyThreatByPercent(victim, -100);
                        LOG_DEBUG("server.ghost", "CellActor[{}]: Removed threat for victim {} from creature {}",
                            _cellId, payload->victimGuid, payload->attackerGuid);
                    }
                    else
                    {
                        creature->GetThreatMgr().AddThreat(victim, payload->threatDelta);
                        LOG_DEBUG("server.ghost", "CellActor[{}]: Added {:.1f} threat for victim {} to creature {}",
                            _cellId, payload->threatDelta, payload->victimGuid, payload->attackerGuid);
                    }
                }
            }
            break;
        }

        case MessageType::AGGRO_REQUEST:
        {
            // Handle zone-in-combat request from another cell
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<AggroRequestPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: AGGRO_REQUEST from creature={} at ({:.1f},{:.1f},{:.1f}) range={:.1f}",
                    _cellId, payload->creatureGuid,
                    payload->creatureX, payload->creatureY, payload->creatureZ,
                    payload->maxRange);

                float rangeSq = payload->maxRange * payload->maxRange;

                // Iterate players/units in this cell
                for (WorldObject* entity : _entities)
                {
                    if (!entity || !entity->IsInWorld())
                        continue;

                    Player* player = entity->ToPlayer();
                    if (!player || !player->IsAlive())
                        continue;

                    if (!(player->GetPhaseMask() & payload->creaturePhaseMask))
                        continue;

                    // Check distance
                    float dx = player->GetPositionX() - payload->creatureX;
                    float dy = player->GetPositionY() - payload->creatureY;
                    float dz = player->GetPositionZ() - payload->creatureZ;
                    float distSq = dx * dx + dy * dy + dz * dz;

                    if (distSq <= rangeSq)
                    {
                        // Send COMBAT_INITIATED back to creature's cell
                        auto responsePayload = std::make_shared<CombatInitiatedPayload>();
                        responsePayload->entityGuid = player->GetGUID().GetRawValue();
                        responsePayload->attackerGuid = payload->creatureGuid;
                        responsePayload->threatAmount = payload->initialThreat;

                        ActorMessage responseMsg;
                        responseMsg.type = MessageType::COMBAT_INITIATED;
                        responseMsg.sourceGuid = player->GetGUID().GetRawValue();
                        responseMsg.targetGuid = payload->creatureGuid;
                        responseMsg.sourceCellId = _cellId;
                        responseMsg.targetCellId = payload->creatureCellId;
                        responseMsg.complexPayload = responsePayload;

                        // Route via map's CellActorManager
                        if (_map && _map->GetCellActorManager())
                        {
                            _map->GetCellActorManager()->SendMessage(payload->creatureCellId, std::move(responseMsg));
                        }

                        LOG_DEBUG("server.ghost", "CellActor[{}]: Player {} within range, sending COMBAT_INITIATED to cell {}",
                            _cellId, player->GetGUID().GetRawValue(), payload->creatureCellId);
                    }
                }
            }
            break;
        }

        case MessageType::COMBAT_INITIATED:
        {
            // Response from AGGRO_REQUEST - entity entered combat
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<CombatInitiatedPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: COMBAT_INITIATED entity={} entered combat with attacker={} threat={:.1f}",
                    _cellId, payload->entityGuid, payload->attackerGuid, payload->threatAmount);

                // Find the creature (attacker) in this cell
                WorldObject* attackerObj = FindEntityByGuid(payload->attackerGuid);
                if (!attackerObj)
                {
                    LOG_DEBUG("server.ghost", "CellActor[{}]: COMBAT_INITIATED - creature {} not found in this cell",
                        _cellId, payload->attackerGuid);
                    break;
                }

                Creature* creature = attackerObj->ToCreature();
                if (!creature || !creature->IsAlive())
                    break;

                // Find the entity (player) that entered combat
                Unit* entity = ObjectAccessor::GetUnit(*creature, ObjectGuid(payload->entityGuid));
                if (!entity || !entity->IsAlive())
                    break;

                // Set creature in combat and add threat
                creature->SetInCombatWith(entity);
                creature->GetThreatMgr().AddThreat(entity, payload->threatAmount);

                LOG_DEBUG("server.ghost", "CellActor[{}]: Creature {} now in combat with entity {}, initial threat {:.1f}",
                    _cellId, payload->attackerGuid, payload->entityGuid, payload->threatAmount);
            }
            break;
        }

        case MessageType::ASSISTANCE_REQUEST:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<AssistanceRequestPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: ASSISTANCE_REQUEST from caller={} target={} at ({:.1f},{:.1f},{:.1f}) radius={:.1f}",
                    _cellId, payload->callerGuid, payload->targetGuid,
                    payload->callerX, payload->callerY, payload->callerZ,
                    payload->radius);

                float radiusSq = payload->radius * payload->radius;

                // Find the caller creature to use CanAssistTo check
                // First try local entities, then use ObjectAccessor with a reference object
                Creature* callerCreature = nullptr;
                WorldObject* callerObj = FindEntityByGuid(payload->callerGuid);
                if (callerObj)
                {
                    callerCreature = callerObj->ToCreature();
                }

                // Iterate creatures in this cell
                for (WorldObject* entity : _entities)
                {
                    if (!entity || !entity->IsInWorld())
                        continue;

                    Creature* assistant = entity->ToCreature();
                    if (!assistant || !assistant->IsAlive())
                        continue;

                    if (assistant->GetGUID().GetRawValue() == payload->callerGuid)
                        continue;

                    if (!(assistant->GetPhaseMask() & payload->callerPhaseMask))
                        continue;

                    // Check distance from caller position
                    float dx = assistant->GetPositionX() - payload->callerX;
                    float dy = assistant->GetPositionY() - payload->callerY;
                    float dz = assistant->GetPositionZ() - payload->callerZ;
                    float distSq = dx * dx + dy * dy + dz * dz;

                    if (distSq > radiusSq)
                        continue;

                    // Find the target to attack using this assistant as reference
                    Unit* target = ObjectAccessor::GetUnit(*assistant, ObjectGuid(payload->targetGuid));
                    if (!target || !target->IsAlive())
                        continue;

                    // Check if can assist (faction, etc) - requires caller creature
                    if (callerCreature && !assistant->CanAssistTo(callerCreature, target))
                        continue;

                    // Start combat - similar to original CallAssistance flow
                    assistant->SetNoCallAssistance(true);
                    if (assistant->IsAIEnabled && assistant->AI())
                    {
                        assistant->AI()->AttackStart(target);
                    }

                    LOG_DEBUG("server.ghost", "CellActor[{}]: Creature {} assisting caller {} against target {}",
                        _cellId, assistant->GetGUID().GetRawValue(), payload->callerGuid, payload->targetGuid);
                }
            }
            break;
        }

        case MessageType::TARGET_SWITCH:
        {
            // AI changed target - update ghost tracking
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<TargetSwitchPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: TARGET_SWITCH creature={} oldTarget={} newTarget={}",
                    _cellId, payload->creatureGuid, payload->oldTargetGuid, payload->newTargetGuid);

                // Update ghost's target info if we have a ghost for this creature
                auto it = _ghosts.find(payload->creatureGuid);
                if (it != _ghosts.end())
                {
                    it->second->SyncTargetGuid(payload->newTargetGuid);
                }
            }
            break;
        }

        case MessageType::EVADE_TRIGGERED:
        {
            LOG_DEBUG("server.ghost", "CellActor[{}]: EVADE_TRIGGERED creature={}",
                _cellId, msg.sourceGuid);

            // Update ghost's combat state to not-in-combat
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                it->second->SyncCombatState(false);
            }
            break;
        }

        case MessageType::PET_REMOVAL:
        {
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<PetRemovalPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: PET_REMOVAL pet={} owner={} mode={}",
                    _cellId, payload->petGuid, payload->ownerGuid, payload->saveMode);

                // Find the owner player (should be in this cell)
                Player* owner = nullptr;
                for (WorldObject* entity : _entities)
                {
                    if (entity && entity->GetGUID().GetRawValue() == payload->ownerGuid)
                    {
                        owner = entity->ToPlayer();
                        break;
                    }
                }

                if (!owner)
                {
                    // Owner not in this cell - try ObjectAccessor
                    owner = ObjectAccessor::FindPlayer(ObjectGuid(payload->ownerGuid));
                }

                if (owner)
                {
                    // Find the pet via owner
                    Pet* pet = owner->GetPet();
                    if (pet && pet->GetGUID().GetRawValue() == payload->petGuid)
                    {
                        // Now safe to remove - parallel updates finished
                        owner->RemovePet(pet, static_cast<PetSaveMode>(payload->saveMode), payload->returnReagent);
                        LOG_DEBUG("server.ghost", "CellActor[{}]: Pet {} removed from owner {}",
                            _cellId, payload->petGuid, payload->ownerGuid);
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

void CellActor::UpdateEntities(uint32_t diff)
{
    bool parallelUpdatesEnabled = sWorld->getBoolConfig(CONFIG_PARALLEL_UPDATES_ENABLED);

    for (WorldObject* entity : _entities)
    {
        if (!entity || !entity->IsInWorld())
            continue;

        if (GameObject* go = entity->ToGameObject())
        {
            go->Update(diff);
        }
        else if (Creature* creature = entity->ToCreature())
        {
            if (creature->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
                continue;

            if (parallelUpdatesEnabled)
                creature->UpdateParallel(diff);
            else
            {
                creature->UpdateRegeneration(diff);
                creature->UpdateTimersParallel(diff);
            }
        }
        else if (Player* player = entity->ToPlayer())
        {
            if (player->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
                continue;

            if (parallelUpdatesEnabled)
                player->UpdateParallel(diff);
        }
    }
}

void CellActor::AddEntity(WorldObject* obj)
{
    if (!obj)
        return;

    _entities.push_back(obj);
}

void CellActor::RemoveEntity(WorldObject* obj)
{
    if (!obj)
        return;

    auto it = std::find(_entities.begin(), _entities.end(), obj);
    if (it != _entities.end())
    {
        // Swap with last and pop for O(1) removal
        std::swap(*it, _entities.back());
        _entities.pop_back();
    }
}

WorldObject* CellActor::FindEntityByGuid(uint64_t guid) const
{
    for (WorldObject* entity : _entities)
    {
        if (entity && entity->GetGUID().GetRawValue() == guid)
            return entity;
    }
    return nullptr;
}

void CellActor::BroadcastHealthChange(Unit* unit)
{
    if (!unit || !_map)
        return;

    auto* cellMgr = _map->GetCellActorManager();
    if (!cellMgr)
        return;

    ActorMessage msg{};
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = unit->GetGUID().GetRawValue();
    msg.sourceCellId = _cellId;
    msg.intParam1 = static_cast<int64_t>(unit->GetHealth());
    msg.intParam2 = static_cast<int64_t>(unit->GetMaxHealth());

    // Send to all neighboring cells that might have ghosts of this entity
    std::vector<uint32_t> neighbors = cellMgr->GetNeighborCellIds(_cellId);
    for (uint32_t neighborCellId : neighbors)
    {
        cellMgr->SendMessage(neighborCellId, msg);
    }
}

// ============================================================================
// GhostEntity Implementation
// ============================================================================

GhostEntity::GhostEntity(uint64_t guid, uint32_t ownerCellId)
    : _guid(guid)
    , _ownerCellId(ownerCellId)
{
}

void GhostEntity::SyncFromSnapshot(const GhostSnapshot& snapshot)
{
    _posX = snapshot.posX;
    _posY = snapshot.posY;
    _posZ = snapshot.posZ;
    _orientation = snapshot.orientation;
    _health = snapshot.health;
    _maxHealth = snapshot.maxHealth;
    _displayId = snapshot.displayId;
    _moveFlags = snapshot.moveFlags;
    _inCombat = snapshot.inCombat;
    _isDead = snapshot.isDead;
}

void GhostEntity::SyncPosition(float x, float y, float z, float o)
{
    _posX = x;
    _posY = y;
    _posZ = z;
    _orientation = o;
}

void GhostEntity::SyncHealth(uint32_t health, uint32_t maxHealth)
{
    _health = health;
    _maxHealth = maxHealth;
    _isDead = (health == 0);
}

void GhostEntity::SyncCombatState(bool inCombat)
{
    _inCombat = inCombat;
}

void GhostEntity::SyncPower(uint8_t power, uint32_t value, uint32_t maxValue)
{
    if (power < 7)  // MAX_POWERS
    {
        _power[power] = value;
        _maxPower[power] = maxValue;
    }
}

uint32_t GhostEntity::GetPower(uint8_t power) const
{
    if (power < 7)  // MAX_POWERS
        return _power[power];
    return 0;
}

// ============================================================================
// CellActorManager Implementation
// ============================================================================

CellActorManager::CellActorManager(Map* map)
    : _map(map)
{
}

CellActor* CellActorManager::GetOrCreateCellActor(uint32_t gridX, uint32_t gridY)
{
    uint32_t cellId = MakeCellId(gridX, gridY);

    auto it = _cellActors.find(cellId);
    if (it != _cellActors.end())
        return it->second.get();

    // Create new cell actor
    auto actor = std::make_unique<CellActor>(cellId, _map);
    CellActor* ptr = actor.get();
    _cellActors[cellId] = std::move(actor);
    _activeCells.push_back(ptr);

    return ptr;
}

CellActor* CellActorManager::GetCellActor(uint32_t gridX, uint32_t gridY)
{
    uint32_t cellId = MakeCellId(gridX, gridY);
    auto it = _cellActors.find(cellId);
    return (it != _cellActors.end()) ? it->second.get() : nullptr;
}

CellActor* CellActorManager::GetCellActorForPosition(float x, float y)
{
    constexpr float kCellSize = 66.6666f;
    constexpr float kCenterCellOffset = 256.0f;

    uint32_t cellX = static_cast<uint32_t>((kCenterCellOffset - (x / kCellSize)));
    uint32_t cellY = static_cast<uint32_t>((kCenterCellOffset - (y / kCellSize)));

    return GetCellActor(cellX, cellY);
}

void CellActorManager::Update(uint32_t diff)
{
    if (!_workPool || _activeCells.empty())
    {
        for (CellActor* cell : _activeCells)
        {
            if (cell->HasWork())
                cell->Update(diff);
        }
        return;
    }

    for (CellActor* cell : _activeCells)
    {
        if (cell->HasWork())
        {
            _pendingCellUpdates.fetch_add(1, std::memory_order_release);

            _workPool->Submit(TaskType::CELL, [this, cell, diff]() {
                cell->Update(diff);
                _pendingCellUpdates.fetch_sub(1, std::memory_order_release);
            });
        }
    }

    while (_pendingCellUpdates.load(std::memory_order_acquire) > 0)
    {
        if (!_workPool->TryExecuteOne(TaskType::CELL))
            std::this_thread::yield();
    }
}

void CellActorManager::SendMessage(uint32_t targetCellId, ActorMessage msg)
{
    auto it = _cellActors.find(targetCellId);
    if (it != _cellActors.end())
    {
        it->second->SendMessage(std::move(msg));
    }
}

void CellActorManager::OnEntityAdded(WorldObject* obj)
{
    if (!obj)
        return;

    float x = obj->GetPositionX();
    float y = obj->GetPositionY();

    constexpr float kCellSize = 66.6666f;
    constexpr float kCenterCellOffset = 256.0f;

    uint32_t cellX = static_cast<uint32_t>((kCenterCellOffset - (x / kCellSize)));
    uint32_t cellY = static_cast<uint32_t>((kCenterCellOffset - (y / kCellSize)));

    CellActor* cell = GetOrCreateCellActor(cellX, cellY);
    cell->AddEntity(obj);

    if (obj->IsGameObject())
        obj->SetCellManaged(true);
    else if (obj->ToCreature() && sWorld->getBoolConfig(CONFIG_PARALLEL_UPDATES_ENABLED))
        obj->SetCellManaged(true);
    else if (obj->ToPlayer() && sWorld->getBoolConfig(CONFIG_PARALLEL_UPDATES_ENABLED))
        obj->SetCellManaged(true);
}

void CellActorManager::OnEntityRemoved(WorldObject* obj)
{
    if (!obj)
        return;

    CellActor* cell = GetCellActorForPosition(obj->GetPositionX(), obj->GetPositionY());
    if (cell)
        cell->RemoveEntity(obj);

    if (obj->IsGameObject())
        obj->SetCellManaged(false);
    else if (obj->ToCreature())
        obj->SetCellManaged(false);
    else if (obj->ToPlayer())
        obj->SetCellManaged(false);
}

void CellActorManager::OnEntityMoved(WorldObject* obj, float oldX, float oldY)
{
    if (!obj)
        return;

    constexpr float kCellSize = 66.6666f;
    constexpr float kCenterCellOffset = 256.0f;

    // Calculate old and new cell positions
    uint32_t oldCellX = static_cast<uint32_t>((kCenterCellOffset - (oldX / kCellSize)));
    uint32_t oldCellY = static_cast<uint32_t>((kCenterCellOffset - (oldY / kCellSize)));

    float newX = obj->GetPositionX();
    float newY = obj->GetPositionY();
    uint32_t newCellX = static_cast<uint32_t>((kCenterCellOffset - (newX / kCellSize)));
    uint32_t newCellY = static_cast<uint32_t>((kCenterCellOffset - (newY / kCellSize)));

    // If cell changed, transfer entity
    if (oldCellX != newCellX || oldCellY != newCellY)
    {
        CellActor* oldCell = GetCellActor(oldCellX, oldCellY);
        CellActor* newCell = GetOrCreateCellActor(newCellX, newCellY);

        if (oldCell)
        {
            oldCell->RemoveEntity(obj);

            // Send migration message
            ActorMessage msg;
            msg.type = MessageType::ENTITY_LEAVING;
            msg.sourceGuid = obj->GetGUID().GetRawValue();
            msg.sourceCellId = MakeCellId(oldCellX, oldCellY);
            msg.targetCellId = MakeCellId(newCellX, newCellY);
            oldCell->SendMessage(std::move(msg));
        }

        newCell->AddEntity(obj);

        // Notify new cell of arrival
        ActorMessage enterMsg;
        enterMsg.type = MessageType::ENTITY_ENTERING;
        enterMsg.sourceGuid = obj->GetGUID().GetRawValue();
        enterMsg.sourceCellId = MakeCellId(oldCellX, oldCellY);
        enterMsg.targetCellId = MakeCellId(newCellX, newCellY);
        enterMsg.floatParam1 = newX;
        enterMsg.floatParam2 = newY;
        enterMsg.floatParam3 = obj->GetPositionZ();
        newCell->SendMessage(std::move(enterMsg));
    }

    // Update ghosts after movement
    UpdateEntityGhosts(obj);
}

// ============================================================================
// Ghost Management
// ============================================================================

GhostSnapshot CellActorManager::CreateSnapshotFromEntity(WorldObject* obj)
{
    GhostSnapshot snapshot;
    if (!obj)
        return snapshot;

    snapshot.guid = obj->GetGUID().GetRawValue();
    snapshot.phaseMask = obj->GetPhaseMask();
    snapshot.posX = obj->GetPositionX();
    snapshot.posY = obj->GetPositionY();
    snapshot.posZ = obj->GetPositionZ();
    snapshot.orientation = obj->GetOrientation();

    // Get health if it's a unit
    if (obj->IsUnit())
    {
        Unit* unit = obj->ToUnit();
        snapshot.health = unit->GetHealth();
        snapshot.maxHealth = unit->GetMaxHealth();
        snapshot.inCombat = unit->IsInCombat();
        snapshot.isDead = !unit->IsAlive();
        snapshot.displayId = unit->GetDisplayId();
    }

    return snapshot;
}

void CellActorManager::UpdateEntityGhosts(WorldObject* obj)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    float x = obj->GetPositionX();
    float y = obj->GetPositionY();

    constexpr float kCellSize = 66.6666f;
    constexpr float kCenterCellOffset = 256.0f;
    uint32_t cellX = static_cast<uint32_t>((kCenterCellOffset - (x / kCellSize)));
    uint32_t cellY = static_cast<uint32_t>((kCenterCellOffset - (y / kCellSize)));
    uint32_t homeCellId = MakeCellId(cellX, cellY);

    // Determine which neighbors need ghosts
    NeighborFlags neededGhosts = GhostBoundary::GetNeighborsNeedingGhosts(x, y);

    // Get or create ghost info for this entity
    auto& info = _entityGhostInfo[guid];
    if (info.guid == 0)
    {
        info.guid = guid;
        info.homeCellId = homeCellId;
    }

    NeighborFlags currentGhosts = info.activeGhosts;

    // Array of all directions to check
    static const NeighborFlags allDirections[] = {
        NeighborFlags::NORTH, NeighborFlags::SOUTH,
        NeighborFlags::EAST, NeighborFlags::WEST,
        NeighborFlags::NORTH_EAST, NeighborFlags::NORTH_WEST,
        NeighborFlags::SOUTH_EAST, NeighborFlags::SOUTH_WEST
    };

    GhostSnapshot snapshot;
    bool snapshotCreated = false;

    for (NeighborFlags dir : allDirections)
    {
        bool needsGhost = HasFlag(neededGhosts, dir);
        bool hasGhost = HasFlag(currentGhosts, dir);

        if (needsGhost && !hasGhost)
        {
            // Create ghost in this neighbor
            if (!snapshotCreated)
            {
                snapshot = CreateSnapshotFromEntity(obj);
                snapshotCreated = true;
            }

            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(homeCellId, dir);
            CreateGhostInCell(neighborCellId, snapshot);
            info.activeGhosts = info.activeGhosts | dir;
        }
        else if (!needsGhost && hasGhost)
        {
            // Destroy ghost in this neighbor
            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(homeCellId, dir);
            DestroyGhostInCell(neighborCellId, guid);
            info.activeGhosts = static_cast<NeighborFlags>(
                static_cast<uint8_t>(info.activeGhosts) & ~static_cast<uint8_t>(dir));
        }
        else if (needsGhost && hasGhost)
        {
            // Update position in existing ghost
            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(homeCellId, dir);

            ActorMessage msg;
            msg.type = MessageType::POSITION_UPDATE;
            msg.sourceGuid = guid;
            msg.sourceCellId = homeCellId;
            msg.floatParam1 = x;
            msg.floatParam2 = y;
            msg.floatParam3 = obj->GetPositionZ();
            UpdateGhostInCell(neighborCellId, guid, msg);
        }
    }

    info.homeCellId = homeCellId;
    if (snapshotCreated)
        info.lastSnapshot = snapshot;
}

void CellActorManager::OnEntityHealthChanged(WorldObject* obj, uint32_t health, uint32_t maxHealth)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg;
    msg.type = MessageType::HEALTH_CHANGED;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(health);
    msg.intParam2 = static_cast<int32_t>(maxHealth);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityCombatStateChanged(WorldObject* obj, bool inCombat)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg;
    msg.type = MessageType::COMBAT_STATE_CHANGED;
    msg.sourceGuid = guid;
    msg.intParam1 = inCombat ? 1 : 0;

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityPowerChanged(WorldObject* obj, uint8_t power, uint32_t value, uint32_t maxValue)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::POWER_CHANGED;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(power);
    msg.intParam2 = static_cast<int32_t>(value);
    msg.intParam3 = static_cast<int32_t>(maxValue);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityAuraStateChanged(WorldObject* obj, uint32_t auraState)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::AURA_STATE_SYNC;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(auraState);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityAuraApplied(WorldObject* obj, uint32_t spellId, uint8_t effectMask)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::AURA_APPLY;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(spellId);
    msg.intParam2 = static_cast<int32_t>(effectMask);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityAuraRemoved(WorldObject* obj, uint32_t spellId)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end() || it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::AURA_REMOVE;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(spellId);

    BroadcastToGhosts(guid, msg);
}

void CellActorManager::OnEntityPhaseChanged(WorldObject* obj, uint32_t newPhaseMask)
{
    // Must be valid and in world (SetPhaseMask can be called during Create before object is ready)
    if (!obj || !obj->IsInWorld())
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end())
        return;

    // Update the cached phase mask in ghost info
    it->second.lastSnapshot.phaseMask = newPhaseMask;

    if (it->second.activeGhosts == NeighborFlags::NONE)
        return;

    ActorMessage msg{};
    msg.type = MessageType::PHASE_CHANGED;
    msg.sourceGuid = guid;
    msg.intParam1 = static_cast<int32_t>(newPhaseMask);

    BroadcastToGhosts(guid, msg);
}

bool CellActorManager::CanInteractCrossPhase(WorldObject* source, uint64_t targetGuid)
{
    if (!source)
        return false;

    // Check if target is tracked as a ghost - if so, use ghost's phase mask
    auto it = _entityGhostInfo.find(targetGuid);
    if (it != _entityGhostInfo.end())
    {
        return (source->GetPhaseMask() & it->second.lastSnapshot.phaseMask) != 0;
    }

    // Not a tracked entity, allow interaction (same-cell interactions use normal checks)
    return true;
}

void CellActorManager::BroadcastToGhosts(uint64_t guid, const ActorMessage& msg)
{
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end())
        return;

    const EntityGhostInfo& info = it->second;
    if (info.activeGhosts == NeighborFlags::NONE)
        return;

    static const NeighborFlags allDirections[] = {
        NeighborFlags::NORTH, NeighborFlags::SOUTH,
        NeighborFlags::EAST, NeighborFlags::WEST,
        NeighborFlags::NORTH_EAST, NeighborFlags::NORTH_WEST,
        NeighborFlags::SOUTH_EAST, NeighborFlags::SOUTH_WEST
    };

    for (NeighborFlags dir : allDirections)
    {
        if (HasFlag(info.activeGhosts, dir))
        {
            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(info.homeCellId, dir);
            UpdateGhostInCell(neighborCellId, guid, msg);
        }
    }
}

void CellActorManager::CreateGhostInCell(uint32_t cellId, const GhostSnapshot& snapshot)
{
    ActorMessage msg;
    msg.type = MessageType::GHOST_CREATE;
    msg.sourceGuid = snapshot.guid;
    msg.floatParam1 = snapshot.posX;
    msg.floatParam2 = snapshot.posY;
    msg.floatParam3 = snapshot.posZ;
    msg.intParam1 = static_cast<int32_t>(snapshot.health);
    msg.intParam2 = static_cast<int32_t>(snapshot.maxHealth);

    // Store full snapshot in complexPayload
    auto snapshotCopy = std::make_shared<GhostSnapshot>(snapshot);
    msg.complexPayload = snapshotCopy;

    SendMessage(cellId, std::move(msg));
}

void CellActorManager::UpdateGhostInCell(uint32_t cellId, uint64_t guid, const ActorMessage& msg)
{
    ActorMessage updateMsg = msg;
    updateMsg.targetCellId = cellId;
    SendMessage(cellId, std::move(updateMsg));
}

void CellActorManager::DestroyGhostInCell(uint32_t cellId, uint64_t guid)
{
    ActorMessage msg;
    msg.type = MessageType::GHOST_DESTROY;
    msg.sourceGuid = guid;
    msg.targetCellId = cellId;
    SendMessage(cellId, std::move(msg));
}

void CellActorManager::DestroyAllGhostsForEntity(uint64_t guid)
{
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end())
        return;

    EntityGhostInfo& info = it->second;
    uint32_t homeCellId = info.homeCellId;

    // Destroy ghost in each active neighbor direction
    static const NeighborFlags directions[] = {
        NeighborFlags::NORTH, NeighborFlags::SOUTH,
        NeighborFlags::EAST, NeighborFlags::WEST,
        NeighborFlags::NORTH_EAST, NeighborFlags::NORTH_WEST,
        NeighborFlags::SOUTH_EAST, NeighborFlags::SOUTH_WEST
    };

    for (NeighborFlags dir : directions)
    {
        if (static_cast<uint8_t>(info.activeGhosts) & static_cast<uint8_t>(dir))
        {
            uint32_t neighborCellId = GhostBoundary::GetNeighborCellId(homeCellId, dir);
            DestroyGhostInCell(neighborCellId, guid);
        }
    }

    // Clear ghost info
    _entityGhostInfo.erase(it);
}

// ============================================================================
// Cell Migration
// ============================================================================

uint64_t CellActorManager::GenerateMigrationId()
{
    return _nextMigrationId.fetch_add(1, std::memory_order_relaxed);
}

MigrationSnapshot CellActorManager::CreateMigrationSnapshot(WorldObject* obj)
{
    MigrationSnapshot snapshot;
    if (!obj)
        return snapshot;

    snapshot.guid = obj->GetGUID().GetRawValue();
    snapshot.entry = obj->GetEntry();
    snapshot.typeId = obj->GetTypeId();
    snapshot.posX = obj->GetPositionX();
    snapshot.posY = obj->GetPositionY();
    snapshot.posZ = obj->GetPositionZ();
    snapshot.orientation = obj->GetOrientation();
    snapshot.mapId = obj->GetMapId();

    if (obj->IsUnit())
    {
        Unit* unit = obj->ToUnit();
        snapshot.health = unit->GetHealth();
        snapshot.maxHealth = unit->GetMaxHealth();
        snapshot.power = unit->GetPower(unit->getPowerType());
        snapshot.maxPower = unit->GetMaxPower(unit->getPowerType());
        snapshot.powerType = static_cast<uint8_t>(unit->getPowerType());
        snapshot.displayId = unit->GetDisplayId();
        snapshot.nativeDisplayId = unit->GetNativeDisplayId();
        snapshot.inCombat = unit->IsInCombat();
        snapshot.isDead = !unit->IsAlive();
        snapshot.targetGuid = unit->GetTarget().GetRawValue();

        if (Creature* creature = obj->ToCreature())
        {
            snapshot.reactState = static_cast<uint32_t>(creature->GetReactState());
        }
    }

    return snapshot;
}

void CellActorManager::CheckAndInitiateMigration(WorldObject* obj, float oldX, float oldY)
{
    if (!obj)
        return;

    if (Unit* unit = obj->ToUnit())
    {
        if (unit->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
            return;
    }

    uint64_t guid = obj->GetGUID().GetRawValue();

    if (IsEntityMigrating(guid))
        return;

    constexpr float kCellSize = 66.6666f;
    constexpr float kCenterCellOffset = 256.0f;

    // Calculate old and new cell IDs
    uint32_t oldCellX = static_cast<uint32_t>((kCenterCellOffset - (oldX / kCellSize)));
    uint32_t oldCellY = static_cast<uint32_t>((kCenterCellOffset - (oldY / kCellSize)));
    uint32_t oldCellId = MakeCellId(oldCellX, oldCellY);

    float newX = obj->GetPositionX();
    float newY = obj->GetPositionY();
    uint32_t newCellX = static_cast<uint32_t>((kCenterCellOffset - (newX / kCellSize)));
    uint32_t newCellY = static_cast<uint32_t>((kCenterCellOffset - (newY / kCellSize)));
    uint32_t newCellId = MakeCellId(newCellX, newCellY);

    // Check if cell boundary was crossed
    if (oldCellId != newCellId)
    {
        InitiateMigration(obj, oldCellId, newCellId);
    }
}

void CellActorManager::InitiateMigration(WorldObject* obj, uint32_t oldCellId, uint32_t newCellId)
{
    if (!obj)
        return;

    uint64_t guid = obj->GetGUID().GetRawValue();
    uint64_t migrationId = GenerateMigrationId();

    // Create migration tracking info
    EntityMigrationInfo& info = _entityMigrations[guid];
    info.guid = guid;
    info.oldCellId = oldCellId;
    info.newCellId = newCellId;
    info.state = MigrationState::TRANSFERRING;
    info.migrationStartTime = 0;  // Will be set from game time
    info.bufferedMessages.clear();
    info.snapshot = CreateMigrationSnapshot(obj);

    // Create migration request payload
    auto payload = std::make_shared<MigrationRequestPayload>();
    payload->snapshot = info.snapshot;
    payload->migrationId = migrationId;

    // Send migration request to new cell
    ActorMessage msg;
    msg.type = MessageType::MIGRATION_REQUEST;
    msg.sourceGuid = guid;
    msg.sourceCellId = oldCellId;
    msg.targetCellId = newCellId;
    msg.complexPayload = payload;

    SendMessage(newCellId, std::move(msg));

    LOG_DEBUG("server", "GhostActor: Initiated migration for entity {} from cell {} to cell {} (migrationId: {})",
        guid, oldCellId, newCellId, migrationId);
}

void CellActorManager::ProcessMigrationRequest(const ActorMessage& msg)
{
    if (msg.type != MessageType::MIGRATION_REQUEST)
        return;

    auto payload = std::static_pointer_cast<MigrationRequestPayload>(msg.complexPayload);
    if (!payload)
        return;

    uint64_t guid = msg.sourceGuid;
    uint32_t oldCellId = msg.sourceCellId;
    uint32_t newCellId = msg.targetCellId;

    // Create the cell actor for the new cell if needed
    uint32_t gridX, gridY;
    ExtractCellCoords(newCellId, gridX, gridY);
    CellActor* newCell = GetOrCreateCellActor(gridX, gridY);

    // Accept the migration - entity will be added here
    // In a full implementation, we'd reconstruct the entity from the snapshot
    // For now, we just acknowledge

    // Send ACK back to old cell
    auto ackPayload = std::make_shared<MigrationAckPayload>();
    ackPayload->migrationId = payload->migrationId;
    ackPayload->accepted = true;

    ActorMessage ackMsg;
    ackMsg.type = MessageType::MIGRATION_ACK;
    ackMsg.sourceGuid = guid;
    ackMsg.sourceCellId = newCellId;
    ackMsg.targetCellId = oldCellId;
    ackMsg.complexPayload = ackPayload;

    SendMessage(oldCellId, std::move(ackMsg));

    LOG_DEBUG("server", "GhostActor: Accepted migration for entity {} into cell {} (migrationId: {})",
        guid, newCellId, payload->migrationId);
}

void CellActorManager::ProcessMigrationAck(const ActorMessage& msg)
{
    if (msg.type != MessageType::MIGRATION_ACK)
        return;

    auto payload = std::static_pointer_cast<MigrationAckPayload>(msg.complexPayload);
    if (!payload)
        return;

    uint64_t guid = msg.sourceGuid;

    auto it = _entityMigrations.find(guid);
    if (it == _entityMigrations.end())
        return;

    EntityMigrationInfo& info = it->second;

    if (!payload->accepted)
    {
        // Migration rejected - abort
        AbortMigration(guid);
        return;
    }

    // Migration accepted - forward buffered messages
    info.state = MigrationState::COMPLETING;
    ForwardBufferedMessages(info);

    // Send completion message
    ActorMessage completeMsg;
    completeMsg.type = MessageType::MIGRATION_COMPLETE;
    completeMsg.sourceGuid = guid;
    completeMsg.sourceCellId = info.oldCellId;
    completeMsg.targetCellId = info.newCellId;

    SendMessage(info.newCellId, std::move(completeMsg));

    // Clean up ghosts - destroy in old neighbors, create in new neighbors
    CleanupGhostsForMigration(guid, info.oldCellId, info.newCellId);

    // Complete migration
    CompleteMigration(guid);

    LOG_DEBUG("server", "GhostActor: Completed migration for entity {} to cell {}",
        guid, info.newCellId);
}

void CellActorManager::ProcessMigrationComplete(const ActorMessage& msg)
{
    if (msg.type != MessageType::MIGRATION_COMPLETE)
        return;

    // New cell acknowledges that migration is complete
    // Entity is now fully owned by new cell
    LOG_DEBUG("server", "GhostActor: Migration complete acknowledged for entity {} in cell {}",
        msg.sourceGuid, msg.targetCellId);
}

void CellActorManager::ForwardBufferedMessages(EntityMigrationInfo& info)
{
    for (ActorMessage& msg : info.bufferedMessages)
    {
        // Wrap in MIGRATION_FORWARD type
        ActorMessage forwardMsg;
        forwardMsg.type = MessageType::MIGRATION_FORWARD;
        forwardMsg.sourceGuid = info.guid;
        forwardMsg.targetCellId = info.newCellId;

        // Store original message in payload
        auto originalMsg = std::make_shared<ActorMessage>(std::move(msg));
        forwardMsg.complexPayload = originalMsg;

        SendMessage(info.newCellId, std::move(forwardMsg));
    }

    info.bufferedMessages.clear();
}

void CellActorManager::CompleteMigration(uint64_t guid)
{
    _entityMigrations.erase(guid);
}

void CellActorManager::AbortMigration(uint64_t guid)
{
    auto it = _entityMigrations.find(guid);
    if (it == _entityMigrations.end())
        return;

    LOG_WARN("server", "GhostActor: Aborting migration for entity {}", guid);

    // Clear buffered messages - they'll be processed in old cell
    it->second.bufferedMessages.clear();
    it->second.state = MigrationState::IDLE;

    _entityMigrations.erase(it);
}

void CellActorManager::CleanupGhostsForMigration(uint64_t guid, uint32_t oldCellId, uint32_t newCellId)
{
    auto it = _entityGhostInfo.find(guid);
    if (it == _entityGhostInfo.end())
        return;

    EntityGhostInfo& ghostInfo = it->second;

    // Update home cell to new cell
    ghostInfo.homeCellId = newCellId;

    // Ghost management will be updated on next UpdateEntityGhosts call
    // For now, just mark that ghosts need recalculation
    ghostInfo.activeGhosts = NeighborFlags::NONE;
}

bool CellActorManager::IsEntityMigrating(uint64_t guid) const
{
    auto it = _entityMigrations.find(guid);
    if (it == _entityMigrations.end())
        return false;

    return it->second.state != MigrationState::IDLE;
}

void CellActorManager::BufferMessageForMigrating(uint64_t guid, const ActorMessage& msg)
{
    auto it = _entityMigrations.find(guid);
    if (it == _entityMigrations.end())
        return;

    EntityMigrationInfo& info = it->second;
    if (info.state == MigrationState::TRANSFERRING)
    {
        info.bufferedMessages.push_back(msg);
    }
}

void CellActorManager::UpdateMigrations(uint32_t /*diff*/)
{
    // Check for migration timeouts
    std::vector<uint64_t> timedOut;

    for (auto& [guid, info] : _entityMigrations)
    {
        if (info.state == MigrationState::TRANSFERRING)
        {
            // TODO: Check actual time elapsed
            // For now, migrations complete quickly via message passing
            // If we wanted timeout support, we'd track start time and check diff
        }
    }

    for (uint64_t guid : timedOut)
    {
        AbortMigration(guid);
    }
}

// ============================================================================
// Cross-Cell Combat Helpers
// ============================================================================

uint32_t CellActorManager::GetCellIdForPosition(float x, float y) const
{
    constexpr float kCellSize = 66.6666f;
    constexpr float kCenterCellOffset = 256.0f;

    uint32_t cellX = static_cast<uint32_t>((kCenterCellOffset - (x / kCellSize)));
    uint32_t cellY = static_cast<uint32_t>((kCenterCellOffset - (y / kCellSize)));

    return MakeCellId(cellX, cellY);
}

uint32_t CellActorManager::GetCellIdForEntity(WorldObject* obj) const
{
    if (!obj)
        return 0;

    return GetCellIdForPosition(obj->GetPositionX(), obj->GetPositionY());
}

bool CellActorManager::AreInSameCell(WorldObject* a, WorldObject* b) const
{
    if (!a || !b)
        return false;

    return GetCellIdForEntity(a) == GetCellIdForEntity(b);
}

bool CellActorManager::AreInSameCell(float x1, float y1, float x2, float y2) const
{
    return GetCellIdForPosition(x1, y1) == GetCellIdForPosition(x2, y2);
}

// ============================================================================
// Threat/AI Integration
// ============================================================================

std::vector<uint32_t> CellActorManager::GetCellsInRadius(float x, float y, float radius) const
{
    std::vector<uint32_t> cells;

    // Convert center position to cell coordinates
    constexpr float kCellSize = 66.6666f;
    constexpr float kCenterCellOffset = 256.0f;

    // Calculate how many cells the radius spans
    // +1 to be conservative and catch edge cases
    int32_t cellRadius = static_cast<int32_t>(std::ceil(radius / kCellSize)) + 1;

    // Get center cell coordinates
    int32_t centerCellX = static_cast<int32_t>((kCenterCellOffset - (x / kCellSize)));
    int32_t centerCellY = static_cast<int32_t>((kCenterCellOffset - (y / kCellSize)));

    // Iterate through potential cells in a square around center
    for (int32_t dy = -cellRadius; dy <= cellRadius; ++dy)
    {
        for (int32_t dx = -cellRadius; dx <= cellRadius; ++dx)
        {
            int32_t cellX = centerCellX + dx;
            int32_t cellY = centerCellY + dy;

            // Skip invalid cell coordinates
            if (cellX < 0 || cellY < 0 || cellX >= 512 || cellY >= 512)
                continue;

            // Calculate center of this cell in world coords
            float cellCenterX = (kCenterCellOffset - cellX - 0.5f) * kCellSize;
            float cellCenterY = (kCenterCellOffset - cellY - 0.5f) * kCellSize;

            // Check if any part of the cell is within radius
            // Use distance to closest point in cell (clamped)
            float closestX = std::max(cellCenterX - kCellSize / 2.0f,
                             std::min(x, cellCenterX + kCellSize / 2.0f));
            float closestY = std::max(cellCenterY - kCellSize / 2.0f,
                             std::min(y, cellCenterY + kCellSize / 2.0f));

            float distSq = (closestX - x) * (closestX - x) + (closestY - y) * (closestY - y);

            if (distSq <= radius * radius)
            {
                cells.push_back(MakeCellId(static_cast<uint32_t>(cellX),
                                          static_cast<uint32_t>(cellY)));
            }
        }
    }

    return cells;
}

void CellActorManager::DoZoneInCombatCellAware(WorldObject* creature, float maxRange)
{
    if (!creature)
        return;

    BroadcastAggroRequest(creature, maxRange, 0.0f);
}

void CellActorManager::BroadcastAggroRequest(WorldObject* creature, float maxRange, float initialThreat)
{
    if (!creature)
        return;

    float x = creature->GetPositionX();
    float y = creature->GetPositionY();
    float z = creature->GetPositionZ();

    uint32_t creatureCellId = GetCellIdForEntity(creature);
    uint64_t creatureGuid = creature->GetGUID().GetRawValue();

    // Get all cells within the aggro range
    std::vector<uint32_t> nearbyCells = GetCellsInRadius(x, y, maxRange);

    LOG_DEBUG("server.ghost", "CellActorManager::BroadcastAggroRequest: creature={} at ({:.1f},{:.1f}) range={:.1f} cells={}",
        creatureGuid, x, y, maxRange, nearbyCells.size());

    // Send AGGRO_REQUEST to each nearby cell
    for (uint32_t cellId : nearbyCells)
    {
        auto payload = std::make_shared<AggroRequestPayload>();
        payload->creatureGuid = creatureGuid;
        payload->creatureCellId = creatureCellId;
        payload->creatureX = x;
        payload->creatureY = y;
        payload->creatureZ = z;
        payload->maxRange = maxRange;
        payload->initialThreat = initialThreat;
        payload->creaturePhaseMask = creature->GetPhaseMask();

        ActorMessage msg;
        msg.type = MessageType::AGGRO_REQUEST;
        msg.sourceGuid = creatureGuid;
        msg.sourceCellId = creatureCellId;
        msg.targetCellId = cellId;
        msg.complexPayload = payload;

        SendMessage(cellId, std::move(msg));
    }
}

// ============================================================================
// Parallel AI Integration
// ============================================================================

void CellActorManager::BroadcastAssistanceRequest(WorldObject* caller, uint64_t targetGuid, float radius)
{
    if (!caller)
        return;

    float x = caller->GetPositionX();
    float y = caller->GetPositionY();
    float z = caller->GetPositionZ();

    uint32_t callerCellId = GetCellIdForEntity(caller);
    uint64_t callerGuid = caller->GetGUID().GetRawValue();

    // Get all cells within the assistance radius
    std::vector<uint32_t> nearbyCells = GetCellsInRadius(x, y, radius);

    LOG_DEBUG("server.ghost", "CellActorManager::BroadcastAssistanceRequest: caller={} target={} at ({:.1f},{:.1f}) radius={:.1f} cells={}",
        callerGuid, targetGuid, x, y, radius, nearbyCells.size());

    // Send ASSISTANCE_REQUEST to each nearby cell
    for (uint32_t cellId : nearbyCells)
    {
        auto payload = std::make_shared<AssistanceRequestPayload>();
        payload->callerGuid = callerGuid;
        payload->targetGuid = targetGuid;
        payload->callerCellId = callerCellId;
        payload->callerX = x;
        payload->callerY = y;
        payload->callerZ = z;
        payload->radius = radius;
        payload->callerPhaseMask = caller->GetPhaseMask();

        ActorMessage msg;
        msg.type = MessageType::ASSISTANCE_REQUEST;
        msg.sourceGuid = callerGuid;
        msg.targetGuid = targetGuid;
        msg.sourceCellId = callerCellId;
        msg.targetCellId = cellId;
        msg.complexPayload = payload;

        SendMessage(cellId, std::move(msg));
    }
}

// ============================================================================
// Pet Parallelization Safety
// ============================================================================

void CellActorManager::QueuePetRemoval(WorldObject* pet, uint8_t saveMode, bool returnReagent)
{
    if (!pet)
        return;

    // Get the pet as a Pet* to access owner (ToPet is on Unit, not WorldObject)
    Unit* unitPet = pet->ToUnit();
    if (!unitPet)
        return;

    Pet* petObj = unitPet->ToPet();
    if (!petObj)
        return;

    Player* owner = petObj->GetOwner();
    if (!owner)
        return;

    uint32_t ownerCellId = GetCellIdForEntity(owner);

    auto payload = std::make_shared<PetRemovalPayload>();
    payload->petGuid = pet->GetGUID().GetRawValue();
    payload->ownerGuid = owner->GetGUID().GetRawValue();
    payload->saveMode = saveMode;
    payload->returnReagent = returnReagent;

    ActorMessage msg;
    msg.type = MessageType::PET_REMOVAL;
    msg.sourceGuid = pet->GetGUID().GetRawValue();
    msg.targetGuid = owner->GetGUID().GetRawValue();
    msg.sourceCellId = GetCellIdForEntity(pet);
    msg.targetCellId = ownerCellId;
    msg.complexPayload = payload;

    SendMessage(ownerCellId, std::move(msg));

    LOG_DEBUG("server.ghost", "CellActorManager::QueuePetRemoval: pet={} owner={} mode={} queued to cell {}",
        pet->GetGUID().GetRawValue(), owner->GetGUID().GetRawValue(), saveMode, ownerCellId);
}

// ============================================================================
// Cell-Aware Threat Management
// ============================================================================

void CellActorManager::AddThreatCellAware(WorldObject* attacker, WorldObject* victim, float threat)
{
    if (!attacker || !victim)
        return;

    if (!CanInteractCrossPhase(attacker, victim->GetGUID().GetRawValue()))
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    if (attackerCellId == victimCellId)
    {
        // Same cell - direct threat modification is safe
        // The actual ThreatMgr::AddThreat call happens at the caller level
        LOG_DEBUG("server.ghost", "AddThreatCellAware: same cell, direct threat ok attacker={} victim={} threat={:.1f}",
            attacker->GetGUID().GetRawValue(), victim->GetGUID().GetRawValue(), threat);
    }
    else
    {
        // Different cells - send threat update message
        // The victim's cell will process this and update the bidirectional links
        SendThreatUpdate(
            attacker->GetGUID().GetRawValue(),
            victim->GetGUID().GetRawValue(),
            victimCellId,
            threat,
            true,   // isNewThreat - caller should check, but default to true
            false); // isRemoval
    }
}

void CellActorManager::RemoveThreatCellAware(WorldObject* attacker, WorldObject* victim)
{
    if (!attacker || !victim)
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    if (attackerCellId == victimCellId)
    {
        // Same cell - direct removal is safe
        LOG_DEBUG("server.ghost", "RemoveThreatCellAware: same cell, direct removal ok attacker={} victim={}",
            attacker->GetGUID().GetRawValue(), victim->GetGUID().GetRawValue());
    }
    else
    {
        // Different cells - send threat removal message
        SendThreatUpdate(
            attacker->GetGUID().GetRawValue(),
            victim->GetGUID().GetRawValue(),
            victimCellId,
            0.0f,
            false,  // isNewThreat
            true);  // isRemoval
    }
}

void CellActorManager::ModifyThreatByPercentCellAware(WorldObject* attacker, WorldObject* victim, int32_t percent)
{
    if (!attacker || !victim)
        return;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);
    uint32_t victimCellId = GetCellIdForEntity(victim);

    if (attackerCellId == victimCellId)
    {
        // Same cell - direct modification is safe
        LOG_DEBUG("server.ghost", "ModifyThreatByPercentCellAware: same cell, direct modify ok attacker={} victim={} pct={}",
            attacker->GetGUID().GetRawValue(), victim->GetGUID().GetRawValue(), percent);
    }
    else
    {
        // Different cells - send threat percentage modification message
        SendThreatUpdate(
            attacker->GetGUID().GetRawValue(),
            victim->GetGUID().GetRawValue(),
            attackerCellId,  // Message goes to creature's cell (threat list is on creature)
            0.0f,
            false,           // isNewThreat
            false,           // isRemoval
            percent);        // percentModify
    }
}

void CellActorManager::SendThreatUpdate(uint64_t attackerGuid, uint64_t victimGuid, uint32_t victimCellId,
                                         float threatDelta, bool isNewThreat, bool isRemoval, int32_t percentModify)
{
    auto payload = std::make_shared<ThreatUpdatePayload>();
    payload->attackerGuid = attackerGuid;
    payload->victimGuid = victimGuid;
    payload->threatDelta = threatDelta;
    payload->percentModify = percentModify;
    payload->isNewThreat = isNewThreat;
    payload->isRemoval = isRemoval;

    ActorMessage msg;
    msg.type = MessageType::THREAT_UPDATE;
    msg.sourceGuid = attackerGuid;
    msg.targetGuid = victimGuid;
    msg.targetCellId = victimCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendThreatUpdate: attacker={} victim={} cell={} delta={:.1f} new={} remove={} pct={}",
        attackerGuid, victimGuid, victimCellId, threatDelta, isNewThreat, isRemoval, percentModify);

    SendMessage(victimCellId, std::move(msg));
}

// ============================================================================
// Cross-Cell Damage/Healing Message Senders
// ============================================================================

void CellActorManager::SendSpellHitMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t damage, int32_t healing)
{
    if (!target)
        return;

    if (caster && !CanInteractCrossPhase(caster, target->GetGUID().GetRawValue()))
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<SpellHitPayload>();
    payload->spellId = spellId;
    payload->damage = damage;
    payload->healing = healing;
    payload->effectMask = 1;

    ActorMessage msg{};
    msg.type = MessageType::SPELL_HIT;
    msg.sourceGuid = caster ? caster->GetGUID().GetRawValue() : 0;
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.sourceCellId = caster ? GetCellIdForEntity(caster) : 0;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendSpellHitMessage: spell={} caster={} target={} damage={} healing={}",
        spellId, msg.sourceGuid, msg.targetGuid, damage, healing);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendMeleeDamageMessage(Unit* attacker, Unit* target, int32_t damage, bool isCrit)
{
    if (!target)
        return;

    if (attacker && !CanInteractCrossPhase(attacker, target->GetGUID().GetRawValue()))
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<MeleeDamagePayload>();
    payload->damage = damage;
    payload->isCritical = isCrit;

    ActorMessage msg{};
    msg.type = MessageType::MELEE_DAMAGE;
    msg.sourceGuid = attacker ? attacker->GetGUID().GetRawValue() : 0;
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.sourceCellId = attacker ? GetCellIdForEntity(attacker) : 0;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendMeleeDamageMessage: attacker={} target={} damage={} crit={}",
        msg.sourceGuid, msg.targetGuid, damage, isCrit);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendHealMessage(Unit* healer, Unit* target, uint32_t spellId, int32_t amount)
{
    if (!target)
        return;

    if (healer && !CanInteractCrossPhase(healer, target->GetGUID().GetRawValue()))
        return;

    uint32_t targetCellId = GetCellIdForEntity(target);

    auto payload = std::make_shared<HealPayload>();
    payload->spellId = spellId;
    payload->healAmount = amount;
    payload->effectiveHeal = amount;

    ActorMessage msg{};
    msg.type = MessageType::HEAL;
    msg.sourceGuid = healer ? healer->GetGUID().GetRawValue() : 0;
    msg.targetGuid = target->GetGUID().GetRawValue();
    msg.sourceCellId = healer ? GetCellIdForEntity(healer) : 0;
    msg.targetCellId = targetCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendHealMessage: spell={} healer={} target={} amount={}",
        spellId, msg.sourceGuid, msg.targetGuid, amount);

    SendMessage(targetCellId, std::move(msg));
}

void CellActorManager::SendTargetSwitchMessage(Unit* creature, uint64_t oldTargetGuid, uint64_t newTargetGuid)
{
    if (!creature)
        return;

    auto payload = std::make_shared<TargetSwitchPayload>();
    payload->creatureGuid = creature->GetGUID().GetRawValue();
    payload->oldTargetGuid = oldTargetGuid;
    payload->newTargetGuid = newTargetGuid;

    ActorMessage msg{};
    msg.type = MessageType::TARGET_SWITCH;
    msg.sourceGuid = creature->GetGUID().GetRawValue();
    msg.complexPayload = payload;

    // Broadcast to all neighboring cells so they can update their ghosts
    uint32_t creatureCellId = GetCellIdForEntity(creature);
    std::vector<uint32_t> neighbors = GetNeighborCellIds(creatureCellId);

    LOG_DEBUG("server.ghost", "SendTargetSwitchMessage: creature={} cell={} old={} new={} neighbors={}",
        msg.sourceGuid, creatureCellId, oldTargetGuid, newTargetGuid, neighbors.size());

    for (uint32_t neighborCellId : neighbors)
    {
        SendMessage(neighborCellId, msg);
    }
}

void CellActorManager::BroadcastEvadeTriggered(Unit* creature)
{
    if (!creature)
        return;

    ActorMessage msg{};
    msg.type = MessageType::EVADE_TRIGGERED;
    msg.sourceGuid = creature->GetGUID().GetRawValue();

    uint32_t creatureCellId = GetCellIdForEntity(creature);
    std::vector<uint32_t> neighbors = GetNeighborCellIds(creatureCellId);

    LOG_DEBUG("server.ghost", "BroadcastEvadeTriggered: creature={} cell={}",
        msg.sourceGuid, creatureCellId);

    for (uint32_t neighborCellId : neighbors)
    {
        SendMessage(neighborCellId, msg);
    }
}

// ============================================================================
// Cell-Aware Victim Selection
// ============================================================================

CellActorManager::VictimInfo CellActorManager::GetVictimCellAware(WorldObject* attacker)
{
    VictimInfo result;
    if (!attacker)
        return result;

    Creature* creature = attacker->ToCreature();
    if (!creature)
        return result;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);

    // First try local victim (same cell) via direct pointer
    Unit* victim = creature->GetVictim();
    if (victim)
    {
        result.guid = victim->GetGUID().GetRawValue();
        result.isLocal = AreInSameCell(attacker, victim);
        result.posX = victim->GetPositionX();
        result.posY = victim->GetPositionY();
        result.posZ = victim->GetPositionZ();

        LOG_DEBUG("server.ghost", "GetVictimCellAware: attacker={} cell={} victim={} local={}",
            attacker->GetGUID().GetRawValue(), attackerCellId, result.guid, result.isLocal);
        return result;
    }

    // No direct victim - check ThreatMgr for current victim
    ThreatMgr& threatMgr = creature->GetThreatMgr();
    if (Unit* threatVictim = threatMgr.GetCurrentVictim())
    {
        result.guid = threatVictim->GetGUID().GetRawValue();
        result.isLocal = AreInSameCell(attacker, threatVictim);
        result.posX = threatVictim->GetPositionX();
        result.posY = threatVictim->GetPositionY();
        result.posZ = threatVictim->GetPositionZ();

        LOG_DEBUG("server.ghost", "GetVictimCellAware: attacker={} cell={} threatVictim={} local={}",
            attacker->GetGUID().GetRawValue(), attackerCellId, result.guid, result.isLocal);
    }

    return result;
}

// ============================================================================
// Cell-Managed Object Tracking
// ============================================================================

bool CellActorManager::IsCellManaged(WorldObject* obj) const
{
    if (!obj)
        return false;

    // O(1) flag check instead of hash lookup
    return obj->IsCellManaged();
}

bool CellActorManager::IsCellManagedByGuid([[maybe_unused]] uint64_t guid) const
{
    // Cannot check by GUID alone - need the object pointer for the flag
    // This method is deprecated in favor of IsCellManaged(WorldObject*)
    return false;
}

} // namespace GhostActor
