/*
 * Ghost Actor System - Design Document
 *
 * A lock-free, work-stealing concurrent architecture for MMO game worlds.
 *
 * CORE CONCEPTS:
 *
 * 1. CELL ACTORS
 *    - Each grid cell is an independent Actor with its own message queue
 *    - Cells process their entities without locks (single-writer principle)
 *    - Cell boundaries are defined by existing grid coordinates
 *
 * 2. ENTITY OWNERSHIP
 *    - Every entity has exactly ONE owning cell (the "home" cell)
 *    - The home cell has exclusive write access to that entity's state
 *    - Entity position determines home cell (recomputed on movement)
 *
 * 3. GHOST ENTITIES
 *    - Read-only projections of entities visible in neighboring cells
 *    - Updated via one-way state sync messages from home cell
 *    - Cannot be modified directly - interactions route to home cell
 *
 * 4. MESSAGE PASSING
 *    - Cross-cell interactions become async messages
 *    - Example: Player in Cell A casts spell on Mob in Cell B
 *      → Cell A queues SpellHitMessage to Cell B
 *      → Cell B processes message, applies damage to real Mob
 *      → Cell B queues StateUpdateMessage back (HP changed)
 *
 * 5. WORK STEALING
 *    - Workers have local deques (double-ended queues)
 *    - Push/pop own work from one end (no contention)
 *    - Steal from other workers' opposite end when idle
 *    - Based on Chase-Lev deque algorithm
 *
 * IMPLEMENTATION PHASES:
 *
 * Phase 1: Work-Stealing Thread Pool
 *   - Replace MapUpdater's simple queue with work-stealing deques
 *   - Cells become work items that can be stolen
 *   - No ghosting yet - just better load balancing
 *
 * Phase 2: Cell Actors with Message Queues
 *   - Each cell gets an MPSC (multi-producer, single-consumer) queue
 *   - Intra-cell updates remain direct
 *   - Cross-cell interactions route through messages
 *
 * Phase 3: Ghost Entity System
 *   - Implement read-only entity projections
 *   - State synchronization protocol
 *   - Visibility integration with existing system
 *
 * Phase 4: Cell Migration
 *   - Handle entities moving between cells
 *   - Ownership transfer protocol
 *   - Ghost lifecycle management
 */

#ifndef GHOST_ACTOR_SYSTEM_H
#define GHOST_ACTOR_SYSTEM_H

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

// Forward declarations
class WorldObject;
class Cell;
class Map;

namespace GhostActor
{

// Forward declarations within namespace
class GhostEntity;
struct GhostSnapshot;

// ============================================================================
// PHASE 2: Cell Actor Message Types
// ============================================================================

enum class MessageType : uint8_t
{
    // Combat messages
    SPELL_HIT,
    MELEE_DAMAGE,
    HEAL,
    AURA_APPLY,
    AURA_REMOVE,

    // Movement messages
    ENTITY_ENTERING,      // Entity moving into this cell
    ENTITY_LEAVING,       // Entity leaving this cell
    POSITION_UPDATE,      // Ghost position sync

    // State sync messages
    HEALTH_CHANGED,
    POWER_CHANGED,
    AURA_STATE_SYNC,
    COMBAT_STATE_CHANGED,

    // Visibility messages
    GHOST_CREATE,
    GHOST_UPDATE,
    GHOST_DESTROY,

    // Phase 4: Cell Migration messages
    MIGRATION_REQUEST,    // Old cell → New cell: "Take ownership of this entity"
    MIGRATION_ACK,        // New cell → Old cell: "Ownership accepted"
    MIGRATION_COMPLETE,   // Old cell → New cell: "Buffered messages forwarded, done"
    MIGRATION_FORWARD,    // Forwarded message during migration handoff

    // Phase 6D: Threat/AI messages
    THREAT_UPDATE,        // Notify threat change across cells
    AGGRO_REQUEST,        // Request nearby entities enter combat
    COMBAT_INITIATED,     // Confirm combat started with entity
    TARGET_SWITCH,        // AI changed its target
};

struct ActorMessage
{
    MessageType type;
    uint64_t sourceGuid;
    uint64_t targetGuid;
    uint32_t sourceCellId;
    uint32_t targetCellId;

    // Payload - could use variant or union for efficiency
    int32_t intParam1;
    int32_t intParam2;
    float floatParam1;
    float floatParam2;
    float floatParam3;

    // For complex payloads
    std::shared_ptr<void> complexPayload;
};

// ============================================================================
// PHASE 2: Lock-Free MPSC Queue for Cell Message Inbox
// ============================================================================

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

    // Called by multiple producer threads
    void Push(T item)
    {
        Node* node = new Node();
        node->data = std::move(item);

        Node* prev = _head.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    // Called by single consumer (cell owner thread)
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

    // Check if queue is empty (approximate - may have race)
    [[nodiscard]] bool Empty() const
    {
        return _tail->next.load(std::memory_order_acquire) == nullptr;
    }

private:
    std::atomic<Node*> _head;
    Node* _tail;  // Only accessed by consumer
};

// ============================================================================
// PHASE 2: Cell Actor
// ============================================================================

class CellActor
{
public:
    CellActor(uint32_t cellId, Map* map)
        : _cellId(cellId), _map(map), _lastUpdateTime(0)
    {
    }

    // Process all pending messages and update entities
    void Update(uint32_t diff);

    // Send message to this cell (thread-safe)
    void SendMessage(ActorMessage msg)
    {
        _inbox.Push(std::move(msg));
    }

    // Add/remove entities (called during cell transfer)
    void AddEntity(WorldObject* obj);
    void RemoveEntity(WorldObject* obj);

    uint32_t GetCellId() const { return _cellId; }
    bool HasWork() const { return !_entities.empty() || !_inbox.Empty(); }

private:
    void ProcessMessages();
    void UpdateEntities(uint32_t diff);
    void HandleMessage(ActorMessage& msg);

    uint32_t _cellId;
    Map* _map;
    uint32_t _lastUpdateTime;

    MPSCQueue<ActorMessage> _inbox;
    std::vector<WorldObject*> _entities;  // Entities owned by this cell

    // Ghost tracking - read-only projections of entities in neighboring cells
    std::unordered_map<uint64_t, std::unique_ptr<GhostEntity>> _ghosts;
};

// ============================================================================
// PHASE 3: Ghost Entity System
// ============================================================================

// Ghost visibility threshold - entities within this distance of grid boundary
// Phase 5: Using actual 66-yard cells instead of 533-yard grids
constexpr float GHOST_VISIBILITY_DISTANCE = 250.0f;  // MAX_VISIBILITY_DISTANCE
constexpr float CELL_SIZE = 66.6666f;                // SIZE_OF_GRIDS / MAX_NUMBER_OF_CELLS
constexpr float CENTER_CELL_OFFSET = 256.0f;         // 512 cells / 2

// Flags for which neighbor grids need ghosts
enum class NeighborFlags : uint8_t
{
    NONE        = 0x00,
    NORTH       = 0x01,  // +Y
    SOUTH       = 0x02,  // -Y
    EAST        = 0x04,  // +X
    WEST        = 0x08,  // -X
    NORTH_EAST  = 0x10,  // +X+Y
    NORTH_WEST  = 0x20,  // -X+Y
    SOUTH_EAST  = 0x40,  // +X-Y
    SOUTH_WEST  = 0x80,  // -X-Y
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

/**
 * @brief Snapshot of entity state for ghost synchronization
 */
struct GhostSnapshot
{
    uint64_t guid{0};
    float posX{0}, posY{0}, posZ{0}, orientation{0};
    uint32_t health{0}, maxHealth{0};
    uint32_t displayId{0};
    uint32_t moveFlags{0};
    bool inCombat{false};
    bool isDead{false};
};

/**
 * @brief Read-only projection of an entity in a neighboring cell
 */
class GhostEntity
{
public:
    GhostEntity(uint64_t guid, uint32_t ownerCellId);

    // Read-only accessors
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

    // State sync (called when home cell sends update)
    void SyncFromSnapshot(const GhostSnapshot& snapshot);
    void SyncPosition(float x, float y, float z, float o);
    void SyncHealth(uint32_t health, uint32_t maxHealth);
    void SyncCombatState(bool inCombat);

private:
    uint64_t _guid;
    uint32_t _ownerCellId;

    // Cached state (read-only snapshot)
    float _posX{0}, _posY{0}, _posZ{0}, _orientation{0};
    uint32_t _health{0}, _maxHealth{0};
    uint32_t _displayId{0};
    uint32_t _moveFlags{0};
    bool _inCombat{false};
    bool _isDead{false};
};

/**
 * @brief Tracks which neighbors have ghosts of an entity
 */
struct EntityGhostInfo
{
    uint64_t guid{0};
    uint32_t homeCellId{0};
    NeighborFlags activeGhosts{NeighborFlags::NONE};  // Which neighbors have ghosts
    GhostSnapshot lastSnapshot;
};

// Helper functions for ghost boundary detection
namespace GhostBoundary
{
    /**
     * @brief Get position within current cell (0 to CELL_SIZE)
     */
    inline void GetPositionInCell(float worldX, float worldY, float& cellLocalX, float& cellLocalY)
    {
        // Convert to cell-local coordinates (Phase 5: 66-yard cells)
        float cellFloatX = CENTER_CELL_OFFSET - (worldX / CELL_SIZE);
        float cellFloatY = CENTER_CELL_OFFSET - (worldY / CELL_SIZE);

        // Get fractional part (position within cell)
        cellLocalX = (cellFloatX - std::floor(cellFloatX)) * CELL_SIZE;
        cellLocalY = (cellFloatY - std::floor(cellFloatY)) * CELL_SIZE;
    }

    /**
     * @brief Determine which neighbor cells need ghosts based on position
     *
     * Phase 5: With 66-yard cells and 250-yard visibility, entities are always
     * within visibility distance of all 8 neighbors (66 < 250). So we always
     * return ALL neighbors for ghost propagation.
     */
    inline NeighborFlags GetNeighborsNeedingGhosts([[maybe_unused]] float worldX, [[maybe_unused]] float worldY)
    {
        // With 66-yard cells and 250-yard visibility, every entity is visible
        // from all 8 neighboring cells. Always create ghosts in all neighbors.
        return NeighborFlags::ALL;
    }

    /**
     * @brief Get neighbor cell ID from current cell and direction
     */
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

// ============================================================================
// PHASE 4: Cell Migration System
// ============================================================================

/**
 * @brief Migration state machine
 *
 * Protocol:
 * 1. Entity crosses cell boundary → IDLE → PENDING
 * 2. Old cell sends MIGRATION_REQUEST with snapshot → TRANSFERRING
 * 3. New cell receives, takes ownership, sends MIGRATION_ACK
 * 4. Old cell receives ACK, forwards buffered messages → COMPLETING
 * 5. Old cell sends MIGRATION_COMPLETE, removes entity → IDLE
 *
 * During TRANSFERRING state, messages to entity are buffered in old cell
 * and forwarded after ACK is received.
 */
enum class MigrationState : uint8_t
{
    IDLE,           // Not migrating
    PENDING,        // Detected boundary cross, preparing to migrate
    TRANSFERRING,   // Sent request, waiting for ACK (buffering messages)
    COMPLETING,     // Received ACK, forwarding buffered messages
};

/**
 * @brief Full entity snapshot for migration (more complete than GhostSnapshot)
 */
struct MigrationSnapshot
{
    // Identity
    uint64_t guid{0};
    uint32_t entry{0};
    uint8_t typeId{0};  // TYPEID_UNIT, TYPEID_PLAYER, etc.

    // Position
    float posX{0}, posY{0}, posZ{0}, orientation{0};
    uint32_t mapId{0};

    // State
    uint32_t health{0}, maxHealth{0};
    uint32_t power{0}, maxPower{0};
    uint8_t powerType{0};
    uint32_t displayId{0};
    uint32_t nativeDisplayId{0};
    uint32_t moveFlags{0};
    float speed{0};

    // Combat state
    bool inCombat{false};
    bool isDead{false};
    uint64_t targetGuid{0};

    // For creatures: AI state
    uint32_t aiState{0};
    uint32_t reactState{0};
};

/**
 * @brief Tracks migration state for an entity
 */
struct EntityMigrationInfo
{
    uint64_t guid{0};
    uint32_t oldCellId{0};
    uint32_t newCellId{0};
    MigrationState state{MigrationState::IDLE};
    uint64_t migrationStartTime{0};  // For timeout detection

    // Buffered messages during TRANSFERRING state
    std::vector<ActorMessage> bufferedMessages;

    // Migration snapshot
    MigrationSnapshot snapshot;
};

/**
 * @brief Payload for MIGRATION_REQUEST message
 */
struct MigrationRequestPayload
{
    MigrationSnapshot snapshot;
    uint64_t migrationId;  // Unique ID for this migration (for ACK matching)
};

/**
 * @brief Payload for MIGRATION_ACK message
 */
struct MigrationAckPayload
{
    uint64_t migrationId;
    bool accepted;  // True if new cell accepted ownership
};

// Migration timeout (ms) - if ACK not received, abort and keep in old cell
constexpr uint32_t MIGRATION_TIMEOUT_MS = 5000;

// ============================================================================
// PHASE 6C: Cross-Cell Combat Payloads
// ============================================================================

/**
 * @brief Payload for SPELL_HIT message
 *
 * Contains pre-calculated spell damage/healing to apply to target.
 * Used when caster and target are in different cells.
 */
struct SpellHitPayload
{
    uint32_t spellId{0};
    uint8_t effectMask{0};
    uint8_t missInfo{0};        // SpellMissInfo
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

/**
 * @brief Payload for MELEE_DAMAGE message
 *
 * Contains pre-calculated melee damage to apply to target.
 */
struct MeleeDamagePayload
{
    int32_t damage{0};
    uint32_t schoolMask{0};
    uint32_t blocked{0};
    uint32_t absorbed{0};
    uint32_t resisted{0};
    uint8_t attackType{0};      // WeaponAttackType
    uint8_t hitInfo{0};
    bool isCritical{false};
    uint32_t procAttacker{0};
    uint32_t procVictim{0};
    uint32_t procEx{0};
};

/**
 * @brief Payload for HEAL message
 *
 * Contains pre-calculated healing to apply to target.
 */
struct HealPayload
{
    uint32_t spellId{0};
    int32_t healAmount{0};
    int32_t effectiveHeal{0};   // After overheal calculation
    int32_t absorbed{0};
    bool isCritical{false};
    uint32_t procAttacker{0};
    uint32_t procVictim{0};
};

// ============================================================================
// PHASE 6D: Threat/AI Message Payloads
// ============================================================================

/**
 * @brief Payload for THREAT_UPDATE message
 *
 * Sent when a creature adds/modifies threat on a target in a different cell.
 * The target's cell processes this to update bidirectional threat links.
 */
struct ThreatUpdatePayload
{
    uint64_t attackerGuid{0};   // Creature adding threat
    uint64_t victimGuid{0};     // Target receiving threat
    float threatDelta{0.0f};    // Amount of threat to add/subtract
    bool isNewThreat{false};    // True if this is a new threat entry
    bool isRemoval{false};      // True if removing from threat list
};

/**
 * @brief Payload for AGGRO_REQUEST message
 *
 * Sent by DoZoneInCombatCellAware to request nearby cells add
 * their eligible entities to the creature's threat list.
 */
struct AggroRequestPayload
{
    uint64_t creatureGuid{0};   // Creature requesting aggro
    uint32_t creatureCellId{0}; // Home cell of creature
    float creatureX{0.0f};      // Position for range check
    float creatureY{0.0f};
    float creatureZ{0.0f};
    float maxRange{0.0f};       // Maximum aggro range
    float initialThreat{0.0f};  // Initial threat amount
};

/**
 * @brief Payload for COMBAT_INITIATED message
 *
 * Sent as response to AGGRO_REQUEST when an entity enters combat.
 */
struct CombatInitiatedPayload
{
    uint64_t entityGuid{0};     // Entity that entered combat
    uint64_t attackerGuid{0};   // Creature that initiated
    float threatAmount{0.0f};   // Initial threat value
};

/**
 * @brief Payload for TARGET_SWITCH message
 *
 * Sent when a creature's AI switches target, allowing ghosts
 * and other cells to update their target tracking.
 */
struct TargetSwitchPayload
{
    uint64_t creatureGuid{0};   // Creature switching target
    uint64_t oldTargetGuid{0};  // Previous target (0 if none)
    uint64_t newTargetGuid{0};  // New target (0 if none)
};

// ============================================================================
// Cell Actor Manager - Integrates with Map (Phase 2 + Phase 3 + Phase 4)
// ============================================================================

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

    // Phase 3: Ghost management
    void UpdateEntityGhosts(WorldObject* obj);
    void OnEntityHealthChanged(WorldObject* obj, uint32_t health, uint32_t maxHealth);
    void OnEntityCombatStateChanged(WorldObject* obj, bool inCombat);
    void BroadcastToGhosts(uint64_t guid, const ActorMessage& msg);

    // Phase 4: Cell Migration
    void CheckAndInitiateMigration(WorldObject* obj, float oldX, float oldY);
    void ProcessMigrationRequest(const ActorMessage& msg);
    void ProcessMigrationAck(const ActorMessage& msg);
    void ProcessMigrationComplete(const ActorMessage& msg);
    void UpdateMigrations(uint32_t diff);
    bool IsEntityMigrating(uint64_t guid) const;
    void BufferMessageForMigrating(uint64_t guid, const ActorMessage& msg);

    // Phase 6C: Cross-cell combat helpers
    uint32_t GetCellIdForPosition(float x, float y) const;
    uint32_t GetCellIdForEntity(WorldObject* obj) const;
    bool AreInSameCell(WorldObject* a, WorldObject* b) const;
    bool AreInSameCell(float x1, float y1, float x2, float y2) const;

    // Phase 6D: Threat/AI integration
    std::vector<uint32_t> GetCellsInRadius(float x, float y, float radius) const;
    void DoZoneInCombatCellAware(WorldObject* creature, float maxRange);
    void BroadcastAggroRequest(WorldObject* creature, float maxRange, float initialThreat);

    // Phase 6D-3: Cell-aware threat management
    // These methods route threat operations through messages when targets are in different cells
    void AddThreatCellAware(WorldObject* attacker, WorldObject* victim, float threat);
    void RemoveThreatCellAware(WorldObject* attacker, WorldObject* victim);
    void SendThreatUpdate(uint64_t attackerGuid, uint64_t victimGuid, uint32_t victimCellId,
                          float threatDelta, bool isNewThreat, bool isRemoval);

    // Phase 6D-4: Cell-aware victim selection
    // Returns victim GUID and whether it's local (true) or cross-cell ghost (false)
    struct VictimInfo
    {
        uint64_t guid{0};
        bool isLocal{false};
        float posX{0}, posY{0}, posZ{0};
    };
    VictimInfo GetVictimCellAware(WorldObject* attacker);

    // Stats
    [[nodiscard]] size_t GetActiveCellCount() const { return _activeCells.size(); }
    [[nodiscard]] size_t GetGhostCount() const { return _entityGhostInfo.size(); }
    [[nodiscard]] size_t GetMigratingCount() const { return _entityMigrations.size(); }

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

    // Ghost lifecycle helpers
    void CreateGhostInCell(uint32_t cellId, const GhostSnapshot& snapshot);
    void UpdateGhostInCell(uint32_t cellId, uint64_t guid, const ActorMessage& msg);
    void DestroyGhostInCell(uint32_t cellId, uint64_t guid);
    GhostSnapshot CreateSnapshotFromEntity(WorldObject* obj);

    // Phase 4: Migration helpers
    MigrationSnapshot CreateMigrationSnapshot(WorldObject* obj);
    void InitiateMigration(WorldObject* obj, uint32_t oldCellId, uint32_t newCellId);
    void CompleteMigration(uint64_t guid);
    void AbortMigration(uint64_t guid);
    void ForwardBufferedMessages(EntityMigrationInfo& info);
    void CleanupGhostsForMigration(uint64_t guid, uint32_t oldCellId, uint32_t newCellId);
    uint64_t GenerateMigrationId();

    Map* _map;
    std::unordered_map<uint32_t, std::unique_ptr<CellActor>> _cellActors;
    std::vector<CellActor*> _activeCells;  // Cells with work to do

    // Phase 3: Ghost tracking per entity
    std::unordered_map<uint64_t, EntityGhostInfo> _entityGhostInfo;

    // Phase 4: Migration tracking per entity
    std::unordered_map<uint64_t, EntityMigrationInfo> _entityMigrations;
    std::atomic<uint64_t> _nextMigrationId{1};
};

} // namespace GhostActor

#endif // GHOST_ACTOR_SYSTEM_H
