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
            // Find target entity in our cell
            // Apply spell damage/effects
            // TODO: Implement when integrating with spell system
            break;
        }

        case MessageType::MELEE_DAMAGE:
        {
            // Apply melee damage to target
            // TODO: Implement when integrating with combat system
            break;
        }

        case MessageType::HEAL:
        {
            // Apply healing to target
            break;
        }

        case MessageType::ENTITY_ENTERING:
        {
            // Entity is transferring to this cell
            // complexPayload contains serialized entity state
            // TODO: Deserialize and take ownership
            break;
        }

        case MessageType::ENTITY_LEAVING:
        {
            // Entity is leaving this cell
            // Remove from our entity list
            // TODO: Implement entity removal
            break;
        }

        case MessageType::POSITION_UPDATE:
        {
            // Update ghost position
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                // Update ghost's cached position
                // floatParam1/2/3 = x, y, z
            }
            break;
        }

        case MessageType::HEALTH_CHANGED:
        {
            // Update ghost's health display
            auto it = _ghosts.find(msg.sourceGuid);
            if (it != _ghosts.end())
            {
                // intParam1 = new health, intParam2 = max health
            }
            break;
        }

        case MessageType::GHOST_CREATE:
        {
            // Create a ghost for an entity in neighboring cell
            // TODO: Implement ghost creation
            break;
        }

        case MessageType::GHOST_UPDATE:
        {
            // Full state sync for a ghost
            break;
        }

        case MessageType::GHOST_DESTROY:
        {
            // Remove ghost - entity no longer visible
            _ghosts.erase(msg.sourceGuid);
            break;
        }

        // Phase 4: Migration messages are handled by CellActorManager
        case MessageType::MIGRATION_REQUEST:
        case MessageType::MIGRATION_ACK:
        case MessageType::MIGRATION_COMPLETE:
        case MessageType::MIGRATION_FORWARD:
        {
            // These are processed at the manager level
            // Forward to manager if needed
            break;
        }

        default:
            break;
    }
}

void CellActor::UpdateEntities(uint32_t /*diff*/)
{
    // For now, entities are still updated via the existing Map::Update path
    // This will be changed when we fully integrate with the grid system
    //
    // Future: Each entity's Update() will be called here
    // for (WorldObject* entity : _entities)
    // {
    //     entity->Update(diff);
    // }
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

} // namespace GhostActor
