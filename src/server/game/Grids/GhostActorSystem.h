/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file GhostActorSystem.h
 * @brief Lock-free parallel entity update system using cell-based actors
 *
 * The GhostActorSystem enables multi-threaded entity updates without mutex locks
 * by partitioning the world into cells, each with exclusive ownership of its entities.
 *
 * Key concepts:
 * - Cell Actors: Each 66-yard cell processes its entities independently
 * - Single-Writer: Only the owning cell can modify an entity's state
 * - Ghost Entities: Read-only projections of entities visible in neighboring cells
 * - Message Passing: Cross-cell interactions are asynchronous messages
 * - Work Stealing: Load balancing via Chase-Lev deque algorithm
 *
 * Memory overhead: ~576 bytes per player (ghost projections to 8 neighbors)
 */

#ifndef GHOST_ACTOR_SYSTEM_H
#define GHOST_ACTOR_SYSTEM_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// Forward declarations
class WorldObject;
class Unit;
class Cell;
class Map;
class WorkStealingPool;

namespace GhostActor
{

// Forward declarations within namespace
class GhostEntity;
struct GhostSnapshot;

/// Message types for cross-cell communication
enum class MessageType : uint8_t
{
    // Combat
    SPELL_HIT,
    MELEE_DAMAGE,
    HEAL,
    AURA_APPLY,
    AURA_REMOVE,

    // Movement
    ENTITY_ENTERING,
    ENTITY_LEAVING,
    POSITION_UPDATE,

    // State sync
    HEALTH_CHANGED,
    POWER_CHANGED,
    AURA_STATE_SYNC,
    COMBAT_STATE_CHANGED,
    PHASE_CHANGED,

    // Ghost lifecycle
    GHOST_CREATE,
    GHOST_UPDATE,
    GHOST_DESTROY,

    // Cell migration
    MIGRATION_REQUEST,
    MIGRATION_ACK,
    MIGRATION_COMPLETE,
    MIGRATION_FORWARD,

    // Threat/AI
    THREAT_UPDATE,
    AGGRO_REQUEST,
    COMBAT_INITIATED,
    TARGET_SWITCH,
    EVADE_TRIGGERED,
    ASSISTANCE_REQUEST,

    // Pet safety
    PET_REMOVAL,
};

/// Message passed between cell actors
struct ActorMessage
{
    MessageType type;
    uint64_t sourceGuid;
    uint64_t targetGuid;
    uint32_t sourceCellId;
    uint32_t targetCellId;

    int32_t intParam1;
    int32_t intParam2;
    int32_t intParam3;
    float floatParam1;
    float floatParam2;
    float floatParam3;

    std::shared_ptr<void> complexPayload;
};

/// Lock-free multi-producer single-consumer queue
template<typename T>
class MPSCQueue
{
    struct Node
    {
        std::atomic<Node*> next{nullptr};
        T data;
    };

public:
    MPSCQueue()
    {
        Node* dummy = new Node();
        _head.store(dummy, std::memory_order_relaxed);
        _tail = dummy;
    }

    ~MPSCQueue()
    {
        T dummy;
        while (Pop(dummy)) {}
        delete _tail;
    }

    void Push(T item)
    {
        Node* node = new Node();
        node->data = std::move(item);
        Node* prev = _head.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    bool Pop(T& result)
    {
        Node* tail = _tail;
        Node* next = tail->next.load(std::memory_order_acquire);
        if (next == nullptr)
            return false;
        result = std::move(next->data);
        _tail = next;
        delete tail;
        return true;
    }

    [[nodiscard]] bool Empty() const
    {
        return _tail->next.load(std::memory_order_acquire) == nullptr;
    }

    [[nodiscard]] size_t ApproximateSize() const
    {
        size_t count = 0;
        Node* current = _tail;
        while (current->next.load(std::memory_order_acquire) != nullptr)
        {
            current = current->next.load(std::memory_order_acquire);
            ++count;
            if (count > 10000) break;
        }
        return count;
    }

private:
    std::atomic<Node*> _head;
    Node* _tail;
};

/// Actor representing a single grid cell with exclusive entity ownership
class CellActor
{
public:
    CellActor(uint32_t cellId, Map* map)
        : _cellId(cellId), _map(map), _lastUpdateTime(0) {}

    void Update(uint32_t diff);
    void SendMessage(ActorMessage msg) { _inbox.Push(std::move(msg)); }

    void AddEntity(WorldObject* obj);
    void RemoveEntity(WorldObject* obj);
    WorldObject* FindEntityByGuid(uint64_t guid) const;

    const std::vector<WorldObject*>& GetEntities() const { return _entities; }
    Map* GetMap() const { return _map; }
    uint32_t GetCellId() const { return _cellId; }
    bool HasWork() const { return !_entities.empty() || !_inbox.Empty(); }

    [[nodiscard]] size_t GetEntityCount() const { return _entities.size(); }
    [[nodiscard]] size_t GetGhostCount() const { return _ghosts.size(); }
    [[nodiscard]] size_t GetPendingMessageCount() const { return _inbox.ApproximateSize(); }
    [[nodiscard]] GhostEntity* GetGhost(uint64_t guid) const
    {
        auto it = _ghosts.find(guid);
        return it != _ghosts.end() ? it->second.get() : nullptr;
    }

    [[nodiscard]] uint32_t GetMessagesProcessedLastTick() const
    {
        return _messagesProcessedLastTick.load(std::memory_order_relaxed);
    }
    void IncrementMessageCount() { _messagesProcessedLastTick.fetch_add(1, std::memory_order_relaxed); }
    void ResetMessageCount() { _messagesProcessedLastTick.store(0, std::memory_order_relaxed); }

private:
    void ProcessMessages();
    void UpdateEntities(uint32_t diff);
    void HandleMessage(ActorMessage& msg);
    void BroadcastHealthChange(Unit* unit);

    uint32_t _cellId;
    Map* _map;
    uint32_t _lastUpdateTime;

    MPSCQueue<ActorMessage> _inbox;
    std::vector<WorldObject*> _entities;
    std::unordered_map<uint64_t, std::unique_ptr<GhostEntity>> _ghosts;
    std::atomic<uint32_t> _messagesProcessedLastTick{0};
};

// ---------------------------------------------------------------------------
// Ghost Entity System
// ---------------------------------------------------------------------------

constexpr float GHOST_VISIBILITY_DISTANCE = 250.0f;
constexpr float CELL_SIZE = 66.6666f;
constexpr float CENTER_CELL_OFFSET = 256.0f;

enum class NeighborFlags : uint8_t
{
    NONE        = 0x00,
    NORTH       = 0x01,
    SOUTH       = 0x02,
    EAST        = 0x04,
    WEST        = 0x08,
    NORTH_EAST  = 0x10,
    NORTH_WEST  = 0x20,
    SOUTH_EAST  = 0x40,
    SOUTH_WEST  = 0x80,
    ALL         = 0xFF
};

inline NeighborFlags operator|(NeighborFlags a, NeighborFlags b)
{
    return static_cast<NeighborFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline NeighborFlags operator&(NeighborFlags a, NeighborFlags b)
{
    return static_cast<NeighborFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool HasFlag(NeighborFlags flags, NeighborFlags check)
{
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(check)) != 0;
}

/// Snapshot of entity state for ghost synchronization
struct GhostSnapshot
{
    uint64_t guid{0};
    uint32_t phaseMask{1};
    float posX{0}, posY{0}, posZ{0}, orientation{0};
    uint32_t health{0}, maxHealth{0};
    uint32_t displayId{0};
    uint32_t moveFlags{0};
    bool inCombat{false};
    bool isDead{false};
};

/// Read-only projection of an entity in a neighboring cell
class GhostEntity
{
public:
    GhostEntity(uint64_t guid, uint32_t ownerCellId);

    uint64_t GetGUID() const { return _guid; }
    float GetPositionX() const { return _posX; }
    float GetPositionY() const { return _posY; }
    float GetPositionZ() const { return _posZ; }
    float GetOrientation() const { return _orientation; }
    uint32_t GetHealth() const { return _health; }
    uint32_t GetMaxHealth() const { return _maxHealth; }
    uint32_t GetOwnerCellId() const { return _ownerCellId; }
    bool IsInCombat() const { return _inCombat; }
    bool IsDead() const { return _isDead; }
    uint64_t GetTargetGuid() const { return _targetGuid; }
    uint32_t GetPower(uint8_t power) const;
    uint32_t GetAuraState() const { return _auraState; }
    uint32_t GetPhaseMask() const { return _phaseMask; }
    bool InSamePhase(uint32_t otherMask) const { return _phaseMask & otherMask; }

    void SyncFromSnapshot(const GhostSnapshot& snapshot);
    void SyncPosition(float x, float y, float z, float o);
    void SyncHealth(uint32_t health, uint32_t maxHealth);
    void SyncCombatState(bool inCombat);
    void SyncTargetGuid(uint64_t targetGuid) { _targetGuid = targetGuid; }
    void SyncPower(uint8_t power, uint32_t value, uint32_t maxValue);
    void SyncAuraState(uint32_t auraState) { _auraState = auraState; }
    void SyncPhaseMask(uint32_t phaseMask) { _phaseMask = phaseMask; }

private:
    uint64_t _guid;
    uint32_t _ownerCellId;
    float _posX{0}, _posY{0}, _posZ{0}, _orientation{0};
    uint32_t _health{0}, _maxHealth{0};
    uint32_t _displayId{0};
    uint32_t _moveFlags{0};
    uint64_t _targetGuid{0};
    uint32_t _auraState{0};
    uint32_t _phaseMask{1};
    std::array<uint32_t, 7> _power{};
    std::array<uint32_t, 7> _maxPower{};
    bool _inCombat{false};
    bool _isDead{false};
};

struct EntityGhostInfo
{
    uint64_t guid{0};
    uint32_t homeCellId{0};
    NeighborFlags activeGhosts{NeighborFlags::NONE};
    GhostSnapshot lastSnapshot;
};

inline uint32_t CalculateCellId(float worldX, float worldY)
{
    float cellFloatX = CENTER_CELL_OFFSET - (worldX / CELL_SIZE);
    float cellFloatY = CENTER_CELL_OFFSET - (worldY / CELL_SIZE);
    uint32_t cellX = static_cast<uint32_t>(std::floor(cellFloatX));
    uint32_t cellY = static_cast<uint32_t>(std::floor(cellFloatY));
    return (cellY << 16) | cellX;
}

inline void ExtractCellCoords(uint32_t cellId, uint32_t& cellX, uint32_t& cellY)
{
    cellX = cellId & 0xFFFF;
    cellY = cellId >> 16;
}

namespace GhostBoundary
{
    inline void GetPositionInCell(float worldX, float worldY, float& cellLocalX, float& cellLocalY)
    {
        float cellFloatX = CENTER_CELL_OFFSET - (worldX / CELL_SIZE);
        float cellFloatY = CENTER_CELL_OFFSET - (worldY / CELL_SIZE);
        cellLocalX = (cellFloatX - std::floor(cellFloatX)) * CELL_SIZE;
        cellLocalY = (cellFloatY - std::floor(cellFloatY)) * CELL_SIZE;
    }

    inline NeighborFlags GetNeighborsNeedingGhosts([[maybe_unused]] float worldX, [[maybe_unused]] float worldY)
    {
        return NeighborFlags::ALL;
    }

    inline uint32_t GetNeighborCellId(uint32_t cellId, NeighborFlags direction)
    {
        uint32_t cellX = cellId & 0xFFFF;
        uint32_t cellY = cellId >> 16;

        switch (direction)
        {
            case NeighborFlags::NORTH:      cellY++; break;
            case NeighborFlags::SOUTH:      cellY--; break;
            case NeighborFlags::EAST:       cellX++; break;
            case NeighborFlags::WEST:       cellX--; break;
            case NeighborFlags::NORTH_EAST: cellX++; cellY++; break;
            case NeighborFlags::NORTH_WEST: cellX--; cellY++; break;
            case NeighborFlags::SOUTH_EAST: cellX++; cellY--; break;
            case NeighborFlags::SOUTH_WEST: cellX--; cellY--; break;
            default: break;
        }

        return (cellY << 16) | cellX;
    }
}

// ---------------------------------------------------------------------------
// Cell Migration System
// ---------------------------------------------------------------------------

enum class MigrationState : uint8_t
{
    IDLE,
    PENDING,
    TRANSFERRING,
    COMPLETING,
};

struct MigrationSnapshot
{
    uint64_t guid{0};
    uint32_t entry{0};
    uint8_t typeId{0};
    float posX{0}, posY{0}, posZ{0}, orientation{0};
    uint32_t mapId{0};
    uint32_t health{0}, maxHealth{0};
    uint32_t power{0}, maxPower{0};
    uint8_t powerType{0};
    uint32_t displayId{0};
    uint32_t nativeDisplayId{0};
    uint32_t moveFlags{0};
    float speed{0};
    bool inCombat{false};
    bool isDead{false};
    uint64_t targetGuid{0};
    uint32_t aiState{0};
    uint32_t reactState{0};
};

struct EntityMigrationInfo
{
    uint64_t guid{0};
    uint32_t oldCellId{0};
    uint32_t newCellId{0};
    MigrationState state{MigrationState::IDLE};
    uint64_t migrationStartTime{0};
    std::vector<ActorMessage> bufferedMessages;
    MigrationSnapshot snapshot;
};

struct MigrationRequestPayload
{
    MigrationSnapshot snapshot;
    uint64_t migrationId;
};

struct MigrationAckPayload
{
    uint64_t migrationId;
    bool accepted;
};

constexpr uint32_t MIGRATION_TIMEOUT_MS = 5000;

// ---------------------------------------------------------------------------
// Message Payloads
// ---------------------------------------------------------------------------

struct SpellHitPayload
{
    uint32_t spellId{0};
    uint8_t effectMask{0};
    uint8_t missInfo{0};
    int32_t damage{0};
    int32_t healing{0};
    uint32_t schoolMask{0};
    uint32_t absorb{0};
    uint32_t resist{0};
    uint32_t blocked{0};
    bool isCritical{false};
    uint32_t procAttacker{0};
    uint32_t procVictim{0};
    uint32_t procEx{0};
};

struct MeleeDamagePayload
{
    int32_t damage{0};
    uint32_t schoolMask{0};
    uint32_t blocked{0};
    uint32_t absorbed{0};
    uint32_t resisted{0};
    uint8_t attackType{0};
    uint8_t hitInfo{0};
    bool isCritical{false};
    uint32_t procAttacker{0};
    uint32_t procVictim{0};
    uint32_t procEx{0};
};

struct HealPayload
{
    uint32_t spellId{0};
    int32_t healAmount{0};
    int32_t effectiveHeal{0};
    int32_t absorbed{0};
    bool isCritical{false};
    uint32_t procAttacker{0};
    uint32_t procVictim{0};
};

struct ThreatUpdatePayload
{
    uint64_t attackerGuid{0};
    uint64_t victimGuid{0};
    float threatDelta{0.0f};
    bool isNewThreat{false};
    bool isRemoval{false};
};

struct AggroRequestPayload
{
    uint64_t creatureGuid{0};
    uint32_t creatureCellId{0};
    uint32_t creaturePhaseMask{1};
    float creatureX{0.0f};
    float creatureY{0.0f};
    float creatureZ{0.0f};
    float maxRange{0.0f};
    float initialThreat{0.0f};
};

struct CombatInitiatedPayload
{
    uint64_t entityGuid{0};
    uint64_t attackerGuid{0};
    float threatAmount{0.0f};
};

struct AssistanceRequestPayload
{
    uint64_t callerGuid{0};
    uint64_t targetGuid{0};
    uint32_t callerCellId{0};
    uint32_t callerPhaseMask{1};
    float callerX{0.0f};
    float callerY{0.0f};
    float callerZ{0.0f};
    float radius{0.0f};
};

struct TargetSwitchPayload
{
    uint64_t creatureGuid{0};
    uint64_t oldTargetGuid{0};
    uint64_t newTargetGuid{0};
};

struct PetRemovalPayload
{
    uint64_t petGuid{0};
    uint64_t ownerGuid{0};
    uint8_t saveMode{0};
    bool returnReagent{false};
};

// ---------------------------------------------------------------------------
// Performance Monitoring
// ---------------------------------------------------------------------------

struct PerformanceStats
{
    std::atomic<uint64_t> lastUpdateUs{0};
    uint64_t avgUpdateUs{0};
    uint64_t maxUpdateUs{0};

    static constexpr size_t MAX_MESSAGE_TYPES = 32;
    std::array<std::atomic<uint32_t>, MAX_MESSAGE_TYPES> messageCountsByType{};
    std::atomic<uint32_t> totalMessagesThisTick{0};
    std::atomic<uint32_t> tasksStolen{0};

    uint32_t ticksTracked{0};
    static constexpr uint32_t ROLLING_WINDOW = 100;
    uint64_t rollingUpdateSum{0};

    void RecordUpdateTime(uint64_t us)
    {
        lastUpdateUs.store(us, std::memory_order_relaxed);
        if (us > maxUpdateUs)
            maxUpdateUs = us;
        rollingUpdateSum += us;
        ticksTracked++;
        if (ticksTracked >= ROLLING_WINDOW)
        {
            avgUpdateUs = rollingUpdateSum / ticksTracked;
            rollingUpdateSum = 0;
            ticksTracked = 0;
            maxUpdateUs = us;
        }
    }

    void RecordMessage(MessageType type)
    {
        size_t idx = static_cast<size_t>(type);
        if (idx < MAX_MESSAGE_TYPES)
            messageCountsByType[idx].fetch_add(1, std::memory_order_relaxed);
        totalMessagesThisTick.fetch_add(1, std::memory_order_relaxed);
    }

    void ResetTickCounters()
    {
        totalMessagesThisTick.store(0, std::memory_order_relaxed);
        tasksStolen.store(0, std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// Cell Actor Manager
// ---------------------------------------------------------------------------

class CellActorManager
{
public:
    explicit CellActorManager(Map* map);
    ~CellActorManager() = default;

    // Get or create CellActor for a grid position
    CellActor* GetOrCreateCellActor(uint32_t gridX, uint32_t gridY);
    CellActor* GetCellActor(uint32_t gridX, uint32_t gridY);

    // Get CellActor for world coordinates
    CellActor* GetCellActorForPosition(float x, float y);

    // Update all active cell actors
    void Update(uint32_t diff);

    // Route message between cells
    void SendMessage(uint32_t targetCellId, ActorMessage msg);

    // Entity tracking
    void OnEntityAdded(WorldObject* obj);
    void OnEntityRemoved(WorldObject* obj);
    void OnEntityMoved(WorldObject* obj, float oldX, float oldY);

    // Ghost management
    void UpdateEntityGhosts(WorldObject* obj);
    void OnEntityHealthChanged(WorldObject* obj, uint32_t health, uint32_t maxHealth);
    void OnEntityCombatStateChanged(WorldObject* obj, bool inCombat);
    void OnEntityPowerChanged(WorldObject* obj, uint8_t power, uint32_t value, uint32_t maxValue);
    void OnEntityAuraStateChanged(WorldObject* obj, uint32_t auraState);
    void OnEntityAuraApplied(WorldObject* obj, uint32_t spellId, uint8_t effectMask);
    void OnEntityAuraRemoved(WorldObject* obj, uint32_t spellId);
    void OnEntityPhaseChanged(WorldObject* obj, uint32_t newPhaseMask);
    void BroadcastToGhosts(uint64_t guid, const ActorMessage& msg);
    void DestroyAllGhostsForEntity(uint64_t guid);
    bool CanInteractCrossPhase(WorldObject* source, uint64_t targetGuid);

    // Cell migration
    void CheckAndInitiateMigration(WorldObject* obj, float oldX, float oldY);
    void ProcessMigrationRequest(const ActorMessage& msg);
    void ProcessMigrationAck(const ActorMessage& msg);
    void ProcessMigrationComplete(const ActorMessage& msg);
    void UpdateMigrations(uint32_t diff);
    bool IsEntityMigrating(uint64_t guid) const;
    void BufferMessageForMigrating(uint64_t guid, const ActorMessage& msg);

    // Cell queries
    uint32_t GetCellIdForPosition(float x, float y) const;
    uint32_t GetCellIdForEntity(WorldObject* obj) const;
    bool AreInSameCell(WorldObject* a, WorldObject* b) const;
    bool AreInSameCell(float x1, float y1, float x2, float y2) const;
    std::vector<uint32_t> GetCellsInRadius(float x, float y, float radius) const;

    // Cross-cell combat
    void DoZoneInCombatCellAware(WorldObject* creature, float maxRange);
    void BroadcastAggroRequest(WorldObject* creature, float maxRange, float initialThreat);
    void BroadcastAssistanceRequest(WorldObject* caller, uint64_t targetGuid, float radius);
    void QueuePetRemoval(WorldObject* pet, uint8_t saveMode, bool returnReagent);

    // Cross-cell threat
    void AddThreatCellAware(WorldObject* attacker, WorldObject* victim, float threat);
    void RemoveThreatCellAware(WorldObject* attacker, WorldObject* victim);
    void SendThreatUpdate(uint64_t attackerGuid, uint64_t victimGuid, uint32_t victimCellId,
                          float threatDelta, bool isNewThreat, bool isRemoval);

    // Cross-cell damage/healing
    void SendSpellHitMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t damage, int32_t healing);
    void SendMeleeDamageMessage(Unit* attacker, Unit* target, int32_t damage, bool isCrit);
    void SendHealMessage(Unit* healer, Unit* target, uint32_t spellId, int32_t amount);
    void SendTargetSwitchMessage(Unit* creature, uint64_t oldTargetGuid, uint64_t newTargetGuid);
    void BroadcastEvadeTriggered(Unit* creature);

    struct VictimInfo
    {
        uint64_t guid{0};
        bool isLocal{false};
        float posX{0}, posY{0}, posZ{0};
    };
    VictimInfo GetVictimCellAware(WorldObject* attacker);

    bool IsCellManaged(WorldObject* obj) const;
    bool IsCellManagedByGuid(uint64_t guid) const;

    // Parallel execution
    void SetWorkPool(WorkStealingPool* pool) { _workPool = pool; }
    bool HasWorkPool() const { return _workPool != nullptr; }

    // Stats
    [[nodiscard]] size_t GetActiveCellCount() const { return _activeCells.size(); }
    [[nodiscard]] size_t GetGhostCount() const { return _entityGhostInfo.size(); }
    [[nodiscard]] size_t GetMigratingCount() const { return _entityMigrations.size(); }

    // Performance stats access
    [[nodiscard]] PerformanceStats& GetPerfStats() { return _perfStats; }
    [[nodiscard]] PerformanceStats const& GetPerfStats() const { return _perfStats; }

    // Get hotspot cells sorted by message count (top N)
    [[nodiscard]] std::vector<std::pair<uint32_t, uint32_t>> GetHotspotCells(size_t count) const
    {
        std::vector<std::pair<uint32_t, uint32_t>> cells;
        cells.reserve(_cellActors.size());
        for (const auto& [id, cell] : _cellActors)
            cells.emplace_back(id, cell->GetMessagesProcessedLastTick());

        std::sort(cells.begin(), cells.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

        if (cells.size() > count)
            cells.resize(count);
        return cells;
    }

    // Message tracing (lock-free with fixed slots)
    static constexpr size_t MAX_TRACED_GUIDS = 8;

    void StartTrace(uint64_t guid)
    {
        // Find empty slot and set
        for (size_t i = 0; i < MAX_TRACED_GUIDS; ++i)
        {
            uint64_t expected = 0;
            if (_tracedGuids[i].compare_exchange_strong(expected, guid, std::memory_order_acq_rel))
                return;  // Successfully added
            if (_tracedGuids[i].load(std::memory_order_relaxed) == guid)
                return;  // Already tracing
        }
        // All slots full - silently fail
    }

    void StopTrace(uint64_t guid)
    {
        for (size_t i = 0; i < MAX_TRACED_GUIDS; ++i)
        {
            uint64_t expected = guid;
            _tracedGuids[i].compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        }
    }

    void StopAllTraces()
    {
        for (size_t i = 0; i < MAX_TRACED_GUIDS; ++i)
            _tracedGuids[i].store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] bool IsTracing(uint64_t guid) const
    {
        for (size_t i = 0; i < MAX_TRACED_GUIDS; ++i)
        {
            if (_tracedGuids[i].load(std::memory_order_relaxed) == guid)
                return true;
        }
        return false;
    }

    [[nodiscard]] size_t GetTracedCount() const
    {
        size_t count = 0;
        for (size_t i = 0; i < MAX_TRACED_GUIDS; ++i)
        {
            if (_tracedGuids[i].load(std::memory_order_relaxed) != 0)
                ++count;
        }
        return count;
    }

    // Debug query methods
    [[nodiscard]] CellActor* GetCell(uint32_t cellId) const
    {
        auto it = _cellActors.find(cellId);
        return it != _cellActors.end() ? it->second.get() : nullptr;
    }

    [[nodiscard]] size_t GetTotalEntityCount() const
    {
        size_t total = 0;
        for (const auto& [id, cell] : _cellActors)
            total += cell->GetEntityCount();
        return total;
    }

    [[nodiscard]] size_t GetTotalGhostCount() const
    {
        size_t total = 0;
        for (const auto& [id, cell] : _cellActors)
            total += cell->GetGhostCount();
        return total;
    }

    [[nodiscard]] const EntityGhostInfo* GetEntityGhostInfo(uint64_t guid) const
    {
        auto it = _entityGhostInfo.find(guid);
        return it != _entityGhostInfo.end() ? &it->second : nullptr;
    }

    [[nodiscard]] std::vector<uint32_t> GetNeighborCellIds(uint32_t cellId) const
    {
        std::vector<uint32_t> neighbors;
        neighbors.reserve(8);
        uint32_t cellX = cellId & 0xFFFF;
        uint32_t cellY = cellId >> 16;

        // All 8 neighbors
        neighbors.push_back(((cellY + 1) << 16) | (cellX - 1));  // NW
        neighbors.push_back(((cellY + 1) << 16) | cellX);        // N
        neighbors.push_back(((cellY + 1) << 16) | (cellX + 1));  // NE
        neighbors.push_back((cellY << 16) | (cellX - 1));        // W
        neighbors.push_back((cellY << 16) | (cellX + 1));        // E
        neighbors.push_back(((cellY - 1) << 16) | (cellX - 1));  // SW
        neighbors.push_back(((cellY - 1) << 16) | cellX);        // S
        neighbors.push_back(((cellY - 1) << 16) | (cellX + 1));  // SE

        return neighbors;
    }

private:
    static uint32_t MakeCellId(uint32_t cellX, uint32_t cellY)
    {
        return (cellY << 16) | cellX;
    }

    static void ExtractCellCoords(uint32_t cellId, uint32_t& cellX, uint32_t& cellY)
    {
        cellX = cellId & 0xFFFF;
        cellY = cellId >> 16;
    }

    void CreateGhostInCell(uint32_t cellId, const GhostSnapshot& snapshot);
    void UpdateGhostInCell(uint32_t cellId, uint64_t guid, const ActorMessage& msg);
    void DestroyGhostInCell(uint32_t cellId, uint64_t guid);
    GhostSnapshot CreateSnapshotFromEntity(WorldObject* obj);

    MigrationSnapshot CreateMigrationSnapshot(WorldObject* obj);
    void InitiateMigration(WorldObject* obj, uint32_t oldCellId, uint32_t newCellId);
    void CompleteMigration(uint64_t guid);
    void AbortMigration(uint64_t guid);
    void ForwardBufferedMessages(EntityMigrationInfo& info);
    void CleanupGhostsForMigration(uint64_t guid, uint32_t oldCellId, uint32_t newCellId);
    uint64_t GenerateMigrationId();

    Map* _map;
    std::unordered_map<uint32_t, std::unique_ptr<CellActor>> _cellActors;
    std::vector<CellActor*> _activeCells;
    std::unordered_map<uint64_t, EntityGhostInfo> _entityGhostInfo;
    std::unordered_map<uint64_t, EntityMigrationInfo> _entityMigrations;
    std::atomic<uint64_t> _nextMigrationId{1};

    WorkStealingPool* _workPool{nullptr};
    alignas(64) std::atomic<size_t> _pendingCellUpdates{0};

    PerformanceStats _perfStats;
    std::array<std::atomic<uint64_t>, MAX_TRACED_GUIDS> _tracedGuids{};
};

} // namespace GhostActor

#endif // GHOST_ACTOR_SYSTEM_H
