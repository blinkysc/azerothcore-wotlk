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
#include <set>
#include <unordered_map>
#include <vector>

// Forward declarations
class WorldObject;
class Unit;
class Player;
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
    SPELL_INTERRUPT,
    SPELL_DISPEL,
    POWER_DRAIN,
    SPELLSTEAL,
    SPELLSTEAL_APPLY,  // Response: apply stolen aura to caster
    REFLECT_DAMAGE,    // Bi-directional: thorns/damage shield response

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
    CONTROL_STATE_CHANGED,  // Stun, root, fear, etc.

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

    // =========================================================================
    // Control Effects - Cross-cell crowd control
    // TODO: Integrate with Unit::SetControlled(), SpellAuras, and AI reaction
    // =========================================================================
    STUN,              // Apply stun - target cannot act
    ROOT,              // Apply root - target cannot move, can still cast
    FEAR,              // Apply fear - target flees, needs position sync
    CHARM,             // Apply charm/mind control - changes faction temporarily
    KNOCKBACK,         // Positional displacement with velocity
    SILENCE,           // Apply silence - cannot cast spells
    POLYMORPH,         // Transform target (sheep, pig, etc.)

    // =========================================================================
    // Combat Feedback - Attack result notifications for AI and procs
    // TODO: Integrate with Unit::AttackerStateUpdate(), CalcDamageInfo
    // =========================================================================
    DODGE,             // Attack dodged - no damage dealt
    PARRY,             // Attack parried - no damage, can trigger riposte
    BLOCK,             // Attack blocked - reduced/no damage
    MISS,              // Attack missed - no damage
    IMMUNE,            // Target immune to this effect/spell
    ABSORB_NOTIFICATION, // Damage absorbed by shield effect

    // =========================================================================
    // Spell Cast Notifications - For AI reactions and proc system
    // TODO: Integrate with Spell::prepare(), Spell::finish(), Spell::cancel()
    // =========================================================================
    SPELL_CAST_START,  // Caster began casting - notify ghosts for interrupt AI
    SPELL_CAST_FAILED, // Cast was interrupted or failed
    SPELL_CAST_SUCCESS,// Cast completed successfully - for proc triggers

    // =========================================================================
    // Taunt - Tank gameplay, forces target switching
    // Integrated with ThreatManager and Unit::TauntApply/TauntFadeOut
    // =========================================================================
    TAUNT,             // Force creature to attack taunter
    DETAUNT,           // Remove taunt effect / drop threat

    // =========================================================================
    // Resurrection - Cross-cell resurrection support
    // TODO: Integrate with Player::ResurrectPlayer(), corpse handling
    // =========================================================================
    RESURRECT_REQUEST, // Resurrection spell cast on dead player
    RESURRECT_ACCEPT,  // Dead player accepted resurrection

    // =========================================================================
    // Player Social - Cross-cell social interactions
    // TODO: Integrate with DuelHandler, TradeHandler
    // =========================================================================
    DUEL_REQUEST,      // Player challenged another to duel
    DUEL_STARTED,      // Duel countdown finished, combat begins
    DUEL_ENDED,        // Duel completed (winner, loser, or fled)
    TRADE_REQUEST,     // Trade window request sent
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

    // Lock-free activity tracking for Update() iteration
    [[nodiscard]] bool IsActive() const { return _isActive.load(std::memory_order_acquire); }
    void SetActive(bool active) { _isActive.store(active, std::memory_order_release); }

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

    // Per-cell update timing for performance monitoring
    [[nodiscard]] uint64_t GetLastUpdateUs() const { return _lastUpdateUs.load(std::memory_order_relaxed); }

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
    std::atomic<uint64_t> _lastUpdateUs{0};  // Last update duration in microseconds
    std::atomic<bool> _isActive{false};  // Lock-free activity flag
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
    void SyncControlState(uint32_t state, bool apply)
    {
        if (apply)
            _controlState |= state;
        else
            _controlState &= ~state;
    }
    [[nodiscard]] uint32_t GetControlState() const { return _controlState; }
    [[nodiscard]] bool HasControlState(uint32_t state) const { return (_controlState & state) != 0; }

    // Aura tracking for ghosts
    void SyncAuraApplied(uint32_t spellId, uint8_t effectMask);
    void SyncAuraRemoved(uint32_t spellId);
    [[nodiscard]] bool HasAura(uint32_t spellId) const { return _activeAuras.count(spellId) > 0; }
    [[nodiscard]] size_t GetActiveAuraCount() const { return _activeAuras.size(); }

private:
    void RecalculateAuraState();

    uint64_t _guid;
    uint32_t _ownerCellId;
    float _posX{0}, _posY{0}, _posZ{0}, _orientation{0};
    uint32_t _health{0}, _maxHealth{0};
    uint32_t _displayId{0};
    uint32_t _moveFlags{0};
    uint64_t _targetGuid{0};
    uint32_t _auraState{0};
    uint32_t _phaseMask{1};
    uint32_t _controlState{0};  // UnitState flags for control effects (stun, root, fear, etc.)
    std::array<uint32_t, 7> _power{};
    std::array<uint32_t, 7> _maxPower{};
    std::set<uint32_t> _activeAuras;  // Spell IDs of active auras
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
    int32_t percentModify{0};  // For ModifyThreatByPercent cross-cell
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

struct SpellInterruptPayload
{
    uint64_t casterGuid{0};
    uint64_t targetGuid{0};
    uint32_t interruptSpellId{0};      // The spell doing the interrupt
    uint32_t interruptedSpellId{0};    // The spell being interrupted
    uint32_t schoolMask{0};            // School to lock out
    int32_t lockoutDuration{0};        // Duration in ms
};

struct SpellDispelPayload
{
    uint64_t casterGuid{0};
    uint64_t targetGuid{0};
    uint32_t dispelSpellId{0};         // The dispel spell
    std::vector<std::pair<uint32_t, uint8_t>> dispelList;  // spellId, charges
};

struct PowerDrainPayload
{
    uint64_t casterGuid{0};
    uint64_t targetGuid{0};
    uint32_t spellId{0};
    uint8_t powerType{0};              // Powers enum value
    int32_t amount{0};                 // Amount to drain
    float gainMultiplier{0.0f};        // Multiplier for caster gain (0 for PowerBurn)
    bool isPowerBurn{false};           // PowerBurn deals damage, PowerDrain gives to caster
};

struct SpellstealPayload
{
    uint64_t casterGuid{0};
    uint64_t targetGuid{0};
    uint32_t spellstealSpellId{0};     // The spellsteal spell (e.g., 30449)
    std::vector<std::pair<uint32_t, uint64_t>> stealList;  // spellId, originalCasterGuid
};

struct StolenAuraData
{
    uint32_t spellId{0};
    uint64_t originalCasterGuid{0};
    int32_t duration{0};               // Remaining duration in ms
    int32_t maxDuration{0};            // Max duration in ms
    uint8_t stackAmount{1};
    uint8_t charges{0};
    int32_t baseAmount[3]{0, 0, 0};    // Base amounts for each effect
};

struct SpellstealApplyPayload
{
    uint64_t stealerGuid{0};           // Who stole the aura (mage)
    uint64_t targetGuid{0};            // Who had the aura stolen
    uint32_t spellstealSpellId{0};
    std::vector<StolenAuraData> stolenAuras;
};

struct ReflectDamagePayload
{
    uint64_t reflectorGuid{0};         // Who has thorns (victim of original attack)
    uint64_t attackerGuid{0};          // Who to deal damage to (original attacker)
    uint32_t spellId{0};               // Thorns spell ID for logging
    int32_t damage{0};                 // Damage to deal
    uint32_t schoolMask{0};            // Damage school
    uint32_t absorb{0};                // Absorbed damage
    uint32_t resist{0};                // Resisted damage
};

// ---------------------------------------------------------------------------
// Control Effect Payloads
// ---------------------------------------------------------------------------

/// Payload for control effects (stun, root, fear, charm, silence, polymorph)
///
/// Cross-cell control effects are complex because they modify target state
/// and may require ongoing synchronization (e.g., fear pathing).
///
/// Integration points:
/// - Unit::SetControlled() for applying the effect
/// - SpellAuras for duration tracking
/// - MovementGenerator for fear pathing
/// - CreatureAI::SpellHit() for NPC reactions
struct ControlEffectPayload
{
    uint64_t casterGuid{0};        // Who applied the effect
    uint64_t targetGuid{0};        // Who is affected
    uint32_t spellId{0};           // Spell that caused the effect
    int32_t duration{0};           // Duration in milliseconds
    uint32_t controlType{0};       // Maps to UnitState flags (uint32 for large values like ROOT=1024)
    uint8_t auraType{0};           // SPELL_AURA_* type

    // Polymorph-specific
    uint32_t transformDisplayId{0}; // Display ID for transform effects

    // Fear-specific: destination for fear pathing
    float fearDestX{0.0f};
    float fearDestY{0.0f};
    float fearDestZ{0.0f};
};

/// Payload for knockback effects
///
/// Knockbacks are position-changing effects that must update both
/// the target's actual position and all ghost projections.
///
/// Integration points:
/// - Unit::KnockbackFrom() for physics calculation
/// - MovementHandler for client sync
/// - All ghost projections need position updates
struct KnockbackPayload
{
    uint64_t casterGuid{0};
    uint64_t targetGuid{0};
    uint32_t spellId{0};
    float originX{0.0f};           // Knockback origin point
    float originY{0.0f};
    float originZ{0.0f};
    float speedXY{0.0f};           // Horizontal velocity
    float speedZ{0.0f};            // Vertical velocity
    float destX{0.0f};             // Calculated landing position
    float destY{0.0f};
    float destZ{0.0f};
};

// ---------------------------------------------------------------------------
// Combat Feedback Payloads
// ---------------------------------------------------------------------------

/// Payload for combat result notifications (dodge, parry, block, miss, immune)
///
/// Combat feedback is important for:
/// - Proc triggers (e.g., Overpower after dodge)
/// - AI behavior (some creatures react to parry/dodge)
/// - Combat log entries for cross-cell attacks
///
/// Integration points:
/// - Unit::AttackerStateUpdate() for result determination
/// - CalcDamageInfo for mitigation calculation
/// - Proc system for result-based triggers
struct CombatResultPayload
{
    uint64_t attackerGuid{0};
    uint64_t victimGuid{0};
    uint32_t spellId{0};           // 0 for melee attacks
    uint8_t attackType{0};         // WeaponAttackType enum
    uint8_t hitInfo{0};            // HITINFO_* flags
    uint8_t resultType{0};         // MELEE_HIT_DODGE, PARRY, BLOCK, MISS
    int32_t blockedAmount{0};      // For partial blocks
    int32_t absorbedAmount{0};     // For absorb notifications
    uint32_t procAttacker{0};      // PROC_FLAG_* for attacker
    uint32_t procVictim{0};        // PROC_FLAG_* for victim
    uint32_t procEx{0};            // PROC_EX_* flags
};

// ---------------------------------------------------------------------------
// Spell Cast Notification Payloads
// ---------------------------------------------------------------------------

/// Payload for spell cast notifications
///
/// Spell cast notifications enable:
/// - AI interrupt decisions (creatures can interrupt casts)
/// - Proc triggers on cast start/finish
/// - Ghost state synchronization for cast bars
///
/// Integration points:
/// - Spell::prepare() for CAST_START
/// - Spell::finish() for CAST_SUCCESS
/// - Spell::cancel() for CAST_FAILED
/// - CreatureAI for interrupt AI
struct SpellCastPayload
{
    uint64_t casterGuid{0};
    uint64_t targetGuid{0};        // Primary target, may be 0
    uint32_t spellId{0};
    int32_t castTime{0};           // Total cast time in ms (for CAST_START)
    int32_t remainingTime{0};      // Remaining time if interrupted
    uint8_t failReason{0};         // SPELL_FAILED_* for CAST_FAILED
    uint32_t schoolMask{0};        // For AI interrupt priority
    bool isChanneled{false};       // Channeled spells have different behavior
};

// ---------------------------------------------------------------------------
// Taunt Payloads
// ---------------------------------------------------------------------------

/// Payload for taunt effects
///
/// Taunts force creatures to attack the taunter, overriding normal
/// threat-based targeting. This is critical for tank gameplay.
///
/// Integration points:
/// - ThreatManager::Taunt() to apply taunt
/// - ThreatManager::TauntFadeOut() when taunt expires
/// - CreatureAI::TauntApply()/TauntFadeOut() callbacks
struct TauntPayload
{
    uint64_t taunterGuid{0};       // Tank who taunted
    uint64_t targetGuid{0};        // Creature being taunted
    uint32_t spellId{0};           // Taunt spell (e.g., 355 Taunt, 694 Mocking Blow)
    int32_t duration{0};           // Taunt duration in ms (usually 3 sec)
    bool isSingleTarget{true};     // false for AoE taunts (Challenging Shout)
    float threatAmount{0.0f};      // Threat granted with taunt (some taunts add threat)
};

/// Payload for detaunt/threat drop effects
///
/// Detaunts remove taunt effects and may reduce threat.
/// Examples: Fade, Cower, Hand of Salvation
struct DetauntPayload
{
    uint64_t sourceGuid{0};        // Who is dropping threat
    uint64_t targetGuid{0};        // Creature to reduce threat on
    uint32_t spellId{0};
    float threatReductionPct{0.0f}; // Percentage threat reduction (0-100)
    bool removeTaunt{false};       // Remove active taunt effect
};

// ---------------------------------------------------------------------------
// Resurrection Payloads
// ---------------------------------------------------------------------------

/// Payload for resurrection spells
///
/// Resurrection across cells requires:
/// - Corpse location awareness (may be different cell than spirit)
/// - Proper health/mana restoration
/// - Resurrection sickness handling
///
/// Integration points:
/// - Player::ResurrectPlayer() for actual resurrection
/// - Corpse system for corpse removal
/// - SpellEffects for resurrection spells
struct ResurrectPayload
{
    uint64_t casterGuid{0};        // Resurrector
    uint64_t targetGuid{0};        // Dead player
    uint32_t spellId{0};
    uint32_t healthPct{0};         // Health restored (percentage)
    uint32_t manaPct{0};           // Mana restored (percentage)
    float destX{0.0f};             // Resurrection location
    float destY{0.0f};
    float destZ{0.0f};
    bool applySickness{false};     // Apply resurrection sickness
    int32_t sicknessLevel{0};      // Sickness level (based on level difference)
};

// ---------------------------------------------------------------------------
// Player Social Payloads
// ---------------------------------------------------------------------------

/// Payload for duel requests and state changes
///
/// Duels involve:
/// - Challenge/accept handshake
/// - Duel area flagging
/// - Combat state that doesn't affect others
/// - Win/loss determination
///
/// Integration points:
/// - DuelHandler for duel logic
/// - Player::DuelComplete() for results
struct DuelPayload
{
    uint64_t challengerGuid{0};
    uint64_t challengedGuid{0};
    float duelX{0.0f};             // Duel flag location
    float duelY{0.0f};
    float duelZ{0.0f};
    uint8_t state{0};              // 0=request, 1=accepted, 2=started, 3=ended
    uint8_t result{0};             // 0=none, 1=challenger won, 2=challenged won, 3=fled
};

/// Payload for trade requests
///
/// Trade windows may involve cross-cell players.
/// Most trade logic is handled client-side, but initiation needs routing.
struct TradePayload
{
    uint64_t initiatorGuid{0};
    uint64_t targetGuid{0};
    uint8_t state{0};              // 0=request, 1=accepted, 2=cancelled
};

// ---------------------------------------------------------------------------
// Performance Monitoring
// ---------------------------------------------------------------------------

struct PerformanceStats
{
    std::atomic<uint64_t> lastUpdateUs{0};
    uint64_t avgUpdateUs{0};
    uint64_t maxUpdateUs{0};

    static constexpr size_t MAX_MESSAGE_TYPES = 64;  // Room for 54 current + future growth
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
    ~CellActorManager();

    // Get or create CellActor for a grid position - INLINED FOR PERFORMANCE
    // These are the hottest paths in the system, called millions of times per second
    [[gnu::hot]] [[gnu::always_inline]]
    inline CellActor* GetOrCreateCellActor(uint32_t gridX, uint32_t gridY) noexcept
    {
        // O(1) lock-free array access - cells are pre-allocated
        // No bounds check needed: cells pre-allocated for all 512x512
        return _cellActors[(gridY << 9) + gridX];  // y*512 + x, using shift for speed
    }

    [[gnu::hot]] [[gnu::always_inline]]
    inline CellActor* GetCellActor(uint32_t gridX, uint32_t gridY) noexcept
    {
        return _cellActors[(gridY << 9) + gridX];
    }

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
    void OnEntityControlStateChanged(WorldObject* obj, uint32_t state, bool apply);
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
    void ModifyThreatByPercentCellAware(WorldObject* attacker, WorldObject* victim, int32_t percent);
    void SendThreatUpdate(uint64_t attackerGuid, uint64_t victimGuid, uint32_t victimCellId,
                          float threatDelta, bool isNewThreat, bool isRemoval, int32_t percentModify = 0);

    // Cross-cell damage/healing
    void SendSpellHitMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t damage, int32_t healing);
    void SendMeleeDamageMessage(Unit* attacker, Unit* target, int32_t damage, bool isCrit);
    void SendHealMessage(Unit* healer, Unit* target, uint32_t spellId, int32_t amount);
    void SendReflectDamageMessage(Unit* reflector, Unit* attacker, uint32_t spellId, int32_t damage,
                                  uint32_t schoolMask, uint32_t absorb, uint32_t resist);
    void SendTargetSwitchMessage(Unit* creature, uint64_t oldTargetGuid, uint64_t newTargetGuid);
    void BroadcastEvadeTriggered(Unit* creature);

    // Cross-cell spell effects
    void SendInterruptMessage(Unit* caster, Unit* target, uint32_t interruptSpellId,
                              uint32_t interruptedSpellId, uint32_t schoolMask, int32_t lockoutDuration);
    void SendDispelMessage(Unit* caster, Unit* target, uint32_t dispelSpellId,
                           const std::vector<std::pair<uint32_t, uint8_t>>& dispelList);
    void SendPowerDrainMessage(Unit* caster, Unit* target, uint32_t spellId, uint8_t powerType,
                               int32_t amount, float gainMultiplier, bool isPowerBurn);
    void SendSpellstealMessage(Unit* caster, Unit* target, uint32_t spellstealSpellId,
                               const std::vector<std::pair<uint32_t, uint64_t>>& stealList);

    // =========================================================================
    // Cross-cell Control Effects
    // TODO: Implement - these integrate with Unit::SetControlled() and SpellAuras
    // =========================================================================

    /// Send stun notification to target's cell.
    /// The target cell should apply the stun via Unit::SetControlled(true, UNIT_STATE_STUNNED).
    /// Ghost projections in neighboring cells should update their state to show stunned.
    void SendStunMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration);

    /// Send root notification to target's cell.
    /// Target cannot move but can still cast. Position is frozen.
    void SendRootMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration);

    /// Send fear notification with calculated flee path.
    /// Fear causes target to run in fear - requires ongoing position sync.
    /// The destination should be pre-calculated to avoid cross-cell pathfinding.
    void SendFearMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration,
                         float destX, float destY, float destZ);

    /// Send charm/mind control notification.
    /// Target faction changes temporarily. Very complex - affects targeting, threat, etc.
    void SendCharmMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration);

    /// Send knockback notification with velocity.
    /// Target position changes. Must update all ghost projections.
    void SendKnockbackMessage(Unit* caster, Unit* target, uint32_t spellId,
                              float speedXY, float speedZ, float destX, float destY, float destZ);

    /// Send silence notification.
    /// Target cannot cast spells. Different from interrupt (which stops current cast).
    void SendSilenceMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration);

    /// Send polymorph notification.
    /// Target transforms and is incapacitated. Display ID changes.
    void SendPolymorphMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t duration,
                              uint32_t transformDisplayId);

    // =========================================================================
    // Combat Feedback Messages
    // TODO: Implement - for procs and AI reactions to attack results
    // =========================================================================

    /// Notify that an attack was dodged.
    /// Important for procs (e.g., Overpower) and AI reactions.
    void SendDodgeMessage(Unit* attacker, Unit* victim, uint32_t spellId);

    /// Notify that an attack was parried.
    /// May trigger parry-haste on creatures.
    void SendParryMessage(Unit* attacker, Unit* victim, uint32_t spellId);

    /// Notify that an attack was blocked.
    /// Includes blocked amount for partial blocks.
    void SendBlockMessage(Unit* attacker, Unit* victim, uint32_t spellId, int32_t blockedAmount);

    /// Notify that an attack missed.
    void SendMissMessage(Unit* attacker, Unit* victim, uint32_t spellId);

    /// Notify that target was immune to effect.
    void SendImmuneMessage(Unit* caster, Unit* target, uint32_t spellId);

    /// Notify that damage was absorbed by a shield.
    /// Separate from damage event for proper absorb tracking.
    void SendAbsorbMessage(Unit* attacker, Unit* victim, uint32_t spellId, int32_t absorbedAmount);

    // =========================================================================
    // Spell Cast Notifications
    // TODO: Implement - for AI interrupt decisions and proc system
    // =========================================================================

    /// Notify neighbors that a unit started casting.
    /// Creatures may decide to interrupt based on spell school/danger.
    void SendSpellCastStartMessage(Unit* caster, Unit* target, uint32_t spellId, int32_t castTime);

    /// Notify neighbors that a cast failed.
    /// Could be interrupted, out of range, etc.
    void SendSpellCastFailedMessage(Unit* caster, uint32_t spellId, uint8_t failReason);

    /// Notify neighbors that a cast succeeded.
    /// For proc triggers on successful casts.
    void SendSpellCastSuccessMessage(Unit* caster, Unit* target, uint32_t spellId);

    // =========================================================================
    // Taunt Messages
    // Integrated with ThreatManager, Unit::TauntApply/TauntFadeOut, HandleModTaunt
    // =========================================================================

    /// Send taunt notification to creature's cell.
    /// Forces creature to attack taunter for duration.
    void SendTauntMessage(Unit* taunter, Unit* target, uint32_t spellId, int32_t duration);

    /// Send detaunt/threat drop notification.
    /// Removes taunt and/or reduces threat.
    void SendDetauntMessage(Unit* source, Unit* target, uint32_t spellId, float threatReductionPct);

    // =========================================================================
    // Resurrection Messages
    // TODO: Implement - for cross-cell resurrection
    // =========================================================================

    /// Send resurrection request to dead player's cell.
    /// Player will receive accept/decline dialog.
    void SendResurrectRequestMessage(Unit* caster, Unit* target, uint32_t spellId,
                                     uint32_t healthPct, uint32_t manaPct);

    /// Handle resurrection acceptance.
    /// Routes to original caster to complete the resurrection.
    void SendResurrectAcceptMessage(Unit* target, uint64_t casterGuid);

    // =========================================================================
    // Player Social Messages
    // TODO: Implement - for cross-cell duels and trades
    // =========================================================================

    /// Send duel request to target's cell.
    void SendDuelRequestMessage(Player* challenger, Player* challenged);

    /// Notify duel state change (accepted, started, ended).
    void SendDuelStateMessage(Player* player1, Player* player2, uint8_t state, uint8_t result);

    /// Send trade request to target's cell.
    void SendTradeRequestMessage(Player* initiator, Player* target);

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
    [[nodiscard]] size_t GetActiveCellCount() const
    {
        size_t count = 0;
        for (uint32_t i = 0; i < TOTAL_CELLS; ++i)
            if (_cellActors[i] && _cellActors[i]->IsActive())
                ++count;
        return count;
    }
    [[nodiscard]] size_t GetGhostCount() const { return _entityGhostInfo.size(); }
    [[nodiscard]] size_t GetMigratingCount() const { return _entityMigrations.size(); }

    // Performance stats access
    [[nodiscard]] PerformanceStats& GetPerfStats() { return _perfStats; }
    [[nodiscard]] PerformanceStats const& GetPerfStats() const { return _perfStats; }

    // Get hotspot cells sorted by message count (top N)
    [[nodiscard]] std::vector<std::pair<uint32_t, uint32_t>> GetHotspotCells(size_t count) const
    {
        std::vector<std::pair<uint32_t, uint32_t>> cells;
        for (uint32_t i = 0; i < TOTAL_CELLS; ++i)
        {
            if (_cellActors[i] && _cellActors[i]->IsActive())
                cells.emplace_back(_cellActors[i]->GetCellId(), _cellActors[i]->GetMessagesProcessedLastTick());
        }

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
        uint32_t cellX, cellY;
        ExtractCellCoords(cellId, cellX, cellY);
        uint32_t index = CellIndex(cellX, cellY);
        if (index < TOTAL_CELLS)
            return _cellActors[index];
        return nullptr;
    }

    [[nodiscard]] size_t GetTotalEntityCount() const
    {
        size_t total = 0;
        for (uint32_t i = 0; i < TOTAL_CELLS; ++i)
            if (_cellActors[i])
                total += _cellActors[i]->GetEntityCount();
        return total;
    }

    [[nodiscard]] size_t GetTotalGhostCount() const
    {
        size_t total = 0;
        for (uint32_t i = 0; i < TOTAL_CELLS; ++i)
            if (_cellActors[i])
                total += _cellActors[i]->GetGhostCount();
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

    // Grid dimension constants (64 grids × 8 cells = 512 cells per dimension)
    static constexpr uint32_t CELLS_PER_DIMENSION = 512;
    static constexpr uint32_t TOTAL_CELLS = CELLS_PER_DIMENSION * CELLS_PER_DIMENSION;  // 262,144

private:
    // Linear index for O(1) array access (0-262143)
    static uint32_t CellIndex(uint32_t cellX, uint32_t cellY)
    {
        return cellY * CELLS_PER_DIMENSION + cellX;
    }

    // Packed ID for CellActor identity (y << 16 | x format)
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
    CellActor** _cellActors;  // Raw pointer array for O(1) access, no unique_ptr overhead
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
