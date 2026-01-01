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
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "Object.h"
#include "Unit.h"

namespace GhostActor
{

// ============================================================================
// CellActor Implementation
// ============================================================================

void CellActor::Update(uint32_t diff)
{
    _lastUpdateTime += diff;

    // 1. Process all incoming messages first
    ProcessMessages();

    // 2. Update all entities owned by this cell
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
    switch (msg.type)
    {
        case MessageType::SPELL_HIT:
        {
            // Phase 6C: Apply spell damage/effects from cross-cell caster
            // When entity updates are parallelized, this processes spell hits
            // from casters in different cells.
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<SpellHitPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: SPELL_HIT spell={} target={} damage={} healing={}",
                    _cellId, payload->spellId, msg.targetGuid, payload->damage, payload->healing);

                // TODO: When parallel updates enabled:
                // 1. Find target Unit* in _entities by GUID
                // 2. Apply damage via Unit::DealDamageFromMessage(payload)
                // 3. Apply healing via Unit::HealFromMessage(payload)
                // 4. Broadcast HEALTH_CHANGED to ghosts
            }
            break;
        }

        case MessageType::MELEE_DAMAGE:
        {
            // Phase 6C: Apply melee damage from cross-cell attacker
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<MeleeDamagePayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: MELEE_DAMAGE target={} damage={} crit={}",
                    _cellId, msg.targetGuid, payload->damage, payload->isCritical);

                // TODO: When parallel updates enabled:
                // 1. Find target Unit* in _entities by GUID
                // 2. Apply damage via Unit::DealMeleeDamageFromMessage(payload)
                // 3. Broadcast HEALTH_CHANGED to ghosts
            }
            break;
        }

        case MessageType::HEAL:
        {
            // Phase 6C: Apply healing from cross-cell healer
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<HealPayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: HEAL spell={} target={} amount={}",
                    _cellId, payload->spellId, msg.targetGuid, payload->healAmount);

                // TODO: When parallel updates enabled:
                // 1. Find target Unit* in _entities by GUID
                // 2. Apply healing via Unit::HealFromMessage(payload)
                // 3. Broadcast HEALTH_CHANGED to ghosts
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
        case MessageType::AURA_REMOVE:
        case MessageType::POWER_CHANGED:
        case MessageType::AURA_STATE_SYNC:
        {
            // Phase 6B+: Aura and power sync messages
            // For now, just acknowledge receipt
            break;
        }

        // Phase 4: Migration messages are handled by CellActorManager
        case MessageType::MIGRATION_REQUEST:
        case MessageType::MIGRATION_ACK:
        case MessageType::MIGRATION_COMPLETE:
        case MessageType::MIGRATION_FORWARD:
        {
            // These are processed at the manager level
            break;
        }

        // Phase 6D: Threat/AI messages
        case MessageType::THREAT_UPDATE:
        {
            // Handle cross-cell threat modification
            if (msg.complexPayload)
            {
                auto payload = std::static_pointer_cast<ThreatUpdatePayload>(msg.complexPayload);
                LOG_DEBUG("server.ghost", "CellActor[{}]: THREAT_UPDATE attacker={} victim={} delta={:.1f} new={} remove={}",
                    _cellId, payload->attackerGuid, payload->victimGuid,
                    payload->threatDelta, payload->isNewThreat, payload->isRemoval);

                // TODO: When parallel updates enabled:
                // 1. Find victim Unit* in _entities by GUID
                // 2. If isNewThreat, add to victim's HostileRefMgr
                // 3. If isRemoval, remove from victim's HostileRefMgr
                // 4. Otherwise, update threat value
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

                // TODO: When parallel updates enabled:
                // 1. Iterate players in _entities
                // 2. Check distance to creature position
                // 3. For each eligible player within range:
                //    - Send COMBAT_INITIATED message back to creature's cell
                //    - Include player GUID and initial threat
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

                // TODO: When parallel updates enabled:
                // 1. Find attacker Creature* in _entities by GUID
                // 2. Add entity to creature's threat list (via ghost reference)
                // 3. Update creature combat state if needed
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

                // TODO: When parallel updates enabled:
                // 1. Update ghost state to reflect new target
                // 2. May be used for target-of-target displays
            }
            break;
        }

        default:
            break;
    }
}

void CellActor::UpdateEntities(uint32_t diff)
{
    // Phase 7A: Update GameObjects in this cell
    for (WorldObject* entity : _entities)
    {
        if (!entity || !entity->IsInWorld())
            continue;

        // Only update GameObjects for now (Phase 7A)
        // Creatures have more complex cross-cell dependencies
        if (GameObject* go = entity->ToGameObject())
        {
            go->Update(diff);
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
    // Convert world coords to cell coords (Phase 5: 66-yard cells)
    constexpr float kCellSize = 66.6666f;
    constexpr float kCenterCellOffset = 256.0f;  // 512 cells / 2

    uint32_t cellX = static_cast<uint32_t>((kCenterCellOffset - (x / kCellSize)));
    uint32_t cellY = static_cast<uint32_t>((kCenterCellOffset - (y / kCellSize)));

    return GetCellActor(cellX, cellY);
}

void CellActorManager::Update(uint32_t diff)
{
    // Update all active cell actors
    // In the future, these could be submitted to the work-stealing pool
    for (CellActor* cell : _activeCells)
    {
        if (cell->HasWork())
        {
            cell->Update(diff);
        }
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

    // Convert to cell coords and get/create cell actor (Phase 5: 66-yard cells)
    constexpr float kCellSize = 66.6666f;
    constexpr float kCenterCellOffset = 256.0f;

    uint32_t cellX = static_cast<uint32_t>((kCenterCellOffset - (x / kCellSize)));
    uint32_t cellY = static_cast<uint32_t>((kCenterCellOffset - (y / kCellSize)));

    CellActor* cell = GetOrCreateCellActor(cellX, cellY);
    cell->AddEntity(obj);

    // Phase 7A: Track GameObjects as cell-managed
    if (obj->IsGameObject())
    {
        _cellManagedObjects.insert(obj->GetGUID().GetRawValue());
    }
}

void CellActorManager::OnEntityRemoved(WorldObject* obj)
{
    if (!obj)
        return;

    // Find the cell this entity was in and remove it
    CellActor* cell = GetCellActorForPosition(obj->GetPositionX(), obj->GetPositionY());
    if (cell)
    {
        cell->RemoveEntity(obj);
    }

    // Phase 7A: Remove from cell-managed tracking
    if (obj->IsGameObject())
    {
        _cellManagedObjects.erase(obj->GetGUID().GetRawValue());
    }
}

void CellActorManager::OnEntityMoved(WorldObject* obj, float oldX, float oldY)
{
    if (!obj)
        return;

    // Phase 5: 66-yard cells
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
// Phase 3: Ghost Management Implementation
// ============================================================================

GhostSnapshot CellActorManager::CreateSnapshotFromEntity(WorldObject* obj)
{
    GhostSnapshot snapshot;
    if (!obj)
        return snapshot;

    snapshot.guid = obj->GetGUID().GetRawValue();
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

    // Get current cell (Phase 5: 66-yard cells)
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

// ============================================================================
// Phase 4: Cell Migration Implementation
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

    uint64_t guid = obj->GetGUID().GetRawValue();

    // Skip if already migrating
    if (IsEntityMigrating(guid))
        return;

    // Phase 5: 66-yard cells
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
// Phase 6C: Cross-Cell Combat Helpers
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
// Phase 6D: Threat/AI Integration
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
// Phase 6D-3: Cell-Aware Threat Management
// ============================================================================

void CellActorManager::AddThreatCellAware(WorldObject* attacker, WorldObject* victim, float threat)
{
    if (!attacker || !victim)
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

void CellActorManager::SendThreatUpdate(uint64_t attackerGuid, uint64_t victimGuid, uint32_t victimCellId,
                                         float threatDelta, bool isNewThreat, bool isRemoval)
{
    auto payload = std::make_shared<ThreatUpdatePayload>();
    payload->attackerGuid = attackerGuid;
    payload->victimGuid = victimGuid;
    payload->threatDelta = threatDelta;
    payload->isNewThreat = isNewThreat;
    payload->isRemoval = isRemoval;

    ActorMessage msg;
    msg.type = MessageType::THREAT_UPDATE;
    msg.sourceGuid = attackerGuid;
    msg.targetGuid = victimGuid;
    msg.targetCellId = victimCellId;
    msg.complexPayload = payload;

    LOG_DEBUG("server.ghost", "SendThreatUpdate: attacker={} victim={} cell={} delta={:.1f} new={} remove={}",
        attackerGuid, victimGuid, victimCellId, threatDelta, isNewThreat, isRemoval);

    SendMessage(victimCellId, std::move(msg));
}

// ============================================================================
// Phase 6D-4: Cell-Aware Victim Selection
// ============================================================================

CellActorManager::VictimInfo CellActorManager::GetVictimCellAware(WorldObject* attacker)
{
    VictimInfo result;
    if (!attacker)
        return result;

    uint32_t attackerCellId = GetCellIdForEntity(attacker);

    // Get the cell actor for the attacker's cell
    auto it = _cellActors.find(attackerCellId);
    if (it == _cellActors.end())
        return result;

    // For now, this is a placeholder that returns empty
    // In a full implementation with parallel updates:
    // 1. Get attacker's ThreatMgr threat list
    // 2. Iterate in threat order
    // 3. For each target:
    //    a. If in same cell, return as local
    //    b. If in different cell, check ghost for validity
    // 4. Return first valid target

    // The actual victim selection still happens via ThreatMgr::SelectVictim()
    // This method is for future use when entity updates are fully parallelized
    // and we need to validate cross-cell targets

    LOG_DEBUG("server.ghost", "GetVictimCellAware: attacker={} cell={} (placeholder)",
        attacker->GetGUID().GetRawValue(), attackerCellId);

    return result;
}

// ============================================================================
// Phase 7A: Cell-Managed Object Tracking
// ============================================================================

bool CellActorManager::IsCellManaged(WorldObject* obj) const
{
    if (!obj)
        return false;

    return IsCellManagedByGuid(obj->GetGUID().GetRawValue());
}

bool CellActorManager::IsCellManagedByGuid(uint64_t guid) const
{
    return _cellManagedObjects.find(guid) != _cellManagedObjects.end();
}

} // namespace GhostActor
