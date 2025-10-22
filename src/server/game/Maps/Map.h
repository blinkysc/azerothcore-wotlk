/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ACORE_MAP_H
#define ACORE_MAP_H

#include "Cell.h"
#include "DBCStructure.h"
#include "DataMap.h"
#include "Define.h"
#include "DynamicTree.h"
#include "GameObjectModel.h"
#include "GridDefines.h"
#include "GridRefMgr.h"
#include "MapGridManager.h"
#include "MapRefMgr.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PathGenerator.h"
#include "Position.h"
#include "SharedDefines.h"
#include "TaskScheduler.h"
#include "Timer.h"
#include "GridTerrainData.h"
#include <bitset>
#include <list>
#include <memory>
#include <shared_mutex>

class Unit;
class WorldPacket;
class InstanceScript;
class Group;
class InstanceSave;
class Object;
class Weather;
class WorldObject;
class TempSummon;
class Player;
class CreatureGroup;
struct ScriptInfo;
struct ScriptAction;
struct Position;
class Battleground;
class MapInstanced;
class InstanceMap;
class BattlegroundMap;
class Transport;
class StaticTransport;
class MotionTransport;
class PathGenerator;
class WorldSession;

enum WeatherState : uint32;

namespace VMAP
{
    enum class ModelIgnoreFlags : uint32;
}

namespace Acore
{
    struct ObjectUpdater;
    struct LargeObjectUpdater;
    class WorkStealingScheduler;
}

struct ScriptAction
{
    ObjectGuid sourceGUID;
    ObjectGuid targetGUID;
    ObjectGuid ownerGUID;                                   // owner of source if source is item
    ScriptInfo const* script;                               // pointer to static script data
};

#define DEFAULT_HEIGHT_SEARCH     50.0f                     // default search distance to find height at nearby locations
#define MIN_UNLOAD_DELAY      1                             // immediate unload
#define UPDATABLE_OBJECT_LIST_RECHECK_TIMER 30 * IN_MILLISECONDS // Time to recheck update object list

struct PositionFullTerrainStatus
{
    PositionFullTerrainStatus()  = default;
    uint32 areaId{0};
    float floorZ{INVALID_HEIGHT};
    bool outdoors{false};
    LiquidData liquidInfo;
};

enum LineOfSightChecks
{
    LINEOFSIGHT_CHECK_VMAP          = 0x1, // check static floor layout data
    LINEOFSIGHT_CHECK_GOBJECT_WMO   = 0x2, // check dynamic game object data (wmo models)
    LINEOFSIGHT_CHECK_GOBJECT_M2    = 0x4, // check dynamic game object data (m2 models)

    LINEOFSIGHT_CHECK_GOBJECT_ALL   = LINEOFSIGHT_CHECK_GOBJECT_WMO | LINEOFSIGHT_CHECK_GOBJECT_M2,

    LINEOFSIGHT_ALL_CHECKS          = LINEOFSIGHT_CHECK_VMAP | LINEOFSIGHT_CHECK_GOBJECT_ALL
};

// GCC have alternative #pragma pack(N) syntax and old gcc version not support pack(push, N), also any gcc version not support it at some platform
#if defined(__GNUC__)
#pragma pack(1)
#else
#pragma pack(push, 1)
#endif

struct InstanceTemplate
{
    uint32 Parent;
    uint32 ScriptId;
    bool AllowMount;
};

enum LevelRequirementVsMode
{
    LEVELREQUIREMENT_HEROIC = 70
};

struct ZoneDynamicInfo
{
    ZoneDynamicInfo();

    uint32 MusicId;
    std::unique_ptr<Weather> DefaultWeather;
    WeatherState WeatherId;
    float WeatherGrade;
    uint32 OverrideLightId;
    uint32 LightFadeInTime;
};

#if defined(__GNUC__)
#pragma pack()
#else
#pragma pack(pop)
#endif

typedef std::map<uint32/*leaderDBGUID*/, CreatureGroup*>        CreatureGroupHolderType;
typedef std::unordered_map<uint32 /*zoneId*/, ZoneDynamicInfo> ZoneDynamicInfoMap;
typedef std::unordered_set<Transport*> TransportsContainer;
typedef std::unordered_set<WorldObject*> ZoneWideVisibleWorldObjectsSet;
typedef std::unordered_map<uint32 /*ZoneId*/, ZoneWideVisibleWorldObjectsSet> ZoneWideVisibleWorldObjectsMap;

enum EncounterCreditType : uint8
{
    ENCOUNTER_CREDIT_KILL_CREATURE  = 0,
    ENCOUNTER_CREDIT_CAST_SPELL     = 1,
};

class Map : public GridRefMgr<MapGridType>
{
    friend class MapReference;
    friend class GridObjectLoader;
public:
    Map(uint32 id, uint32 InstanceId, uint8 SpawnMode, Map* _parent = nullptr);
    ~Map() override;

    [[nodiscard]] MapEntry const* GetEntry() const { return i_mapEntry; }

    // currently unused for normal maps
    bool CanUnload(uint32 diff)
    {
        if (!m_unloadTimer)
            return false;

        if (m_unloadTimer <= diff)
            return true;

        m_unloadTimer -= diff;
        return false;
    }

    virtual bool AddPlayerToMap(Player*);
    virtual void RemovePlayerFromMap(Player*, bool);
    virtual void AfterPlayerUnlinkFromMap();
    template<class T> bool AddToMap(T*, bool checkTransport = false);
    template<class T> void RemoveFromMap(T*, bool);

    void MarkNearbyCellsOf(WorldObject* obj);

    virtual void Update(const uint32, const uint32, bool thread = true);

    [[nodiscard]] float GetVisibilityRange() const { return m_VisibleDistance; }
    void SetVisibilityRange(float range) { m_VisibleDistance = range; }
    void OnCreateMap();
    //function for setting up visibility distance for maps on per-type/per-Id basis
    virtual void InitVisibilityDistance();

    void PlayerRelocation(Player*, float x, float y, float z, float o);
    void CreatureRelocation(Creature* creature, float x, float y, float z, float o);
    void GameObjectRelocation(GameObject* go, float x, float y, float z, float o);
    void DynamicObjectRelocation(DynamicObject* go, float x, float y, float z, float o);

    template<class T, class CONTAINER> void Visit(const Cell& cell, TypeContainerVisitor<T, CONTAINER>& visitor);

    bool IsGridLoaded(GridCoord const& gridCoord) const;
    bool IsGridLoaded(float x, float y) const
    {
        return IsGridLoaded(Acore::ComputeGridCoord(x, y));
    }
    bool IsGridCreated(GridCoord const& gridCoord) const;
    bool IsGridCreated(float x, float y) const
    {
        return IsGridCreated(Acore::ComputeGridCoord(x, y));
    }

    void LoadGrid(float x, float y);
    void LoadAllGrids();
    void LoadGridsInRange(Position const& center, float radius);
    bool UnloadGrid(MapGridType& grid);
    virtual void UnloadAll();

    std::shared_ptr<GridTerrainData> GetGridTerrainDataSharedPtr(GridCoord const& gridCoord);
    GridTerrainData* GetGridTerrainData(GridCoord const& gridCoord);
    GridTerrainData* GetGridTerrainData(float x, float y);

    [[nodiscard]] uint32 GetId() const { return i_mapEntry->MapID; }

    [[nodiscard]] Map const* GetParent() const { return m_parentMap; }

    // pussywizard: movemaps, mmaps
    [[nodiscard]] std::shared_mutex& GetMMapLock() const { return *(const_cast<std::shared_mutex*>(&MMapLock)); }
    // pussywizard:
    std::unordered_set<Unit*> i_objectsForDelayedVisibility;
    void HandleDelayedVisibility();

    // some calls like isInWater should not use vmaps due to processor power
    // can return INVALID_HEIGHT if under z+2 z pos not found height
    [[nodiscard]] float GetHeight(float x, float y, float z, bool checkVMap = true, float maxSearchDist = DEFAULT_HEIGHT_SEARCH) const;
    [[nodiscard]] float GetHeight(uint32 phasemask, float x, float y, float z, bool vmap = true, float maxSearchDist = DEFAULT_HEIGHT_SEARCH) const;
    [[nodiscard]] float GetWaterOrGroundLevel(uint32 phasemask, float x, float y, float z, float* ground = nullptr, bool swim = false, float collisionHeight = DEFAULT_COLLISION_HEIGHT) const;
    [[nodiscard]] bool GetAreaInfo(float x, float y, float z, uint32& mogpflags, int32& adtId, int32& rootId, int32& groupId) const;
    [[nodiscard]] bool IsInWater(uint32 phaseMask, float x, float y, float z, float collisionHeight) const;
    [[nodiscard]] bool HasEnoughWater(WorldObject const* searcher, float x, float y, float z) const;
    [[nodiscard]] bool IsUnderWater(uint32 phaseMask, float x, float y, float z) const;
    [[nodiscard]] void GetFullTerrainStatusForPosition(uint32 phaseMask, float x, float y, float z, PositionFullTerrainStatus& data, uint8 reqLiquidType = MAP_ALL_LIQUIDS, float collisionHeight = DEFAULT_COLLISION_HEIGHT);

    [[nodiscard]] uint16 GetAreaFlag(float x, float y, float z, bool* isOutdoors = nullptr) const;
    [[nodiscard]] uint8 GetTerrainType(float x, float y) const;
    [[nodiscard]] float GetWaterLevel(float x, float y) const;
    [[nodiscard]] bool IsOutdoors(float x, float y, float z) const;

    [[nodiscard]] uint32 GetAreaId(float x, float y, float z, bool* isOutdoors = nullptr) const;
    [[nodiscard]] uint32 GetAreaId(uint32 phaseMask, float x, float y, float z, bool* isOutdoors = nullptr) const;
    [[nodiscard]] uint32 GetZoneId(uint32 phaseMask, float x, float y, float z) const;
    [[nodiscard]] uint32 GetZoneId(float x, float y, float z) const;
    void GetZoneAndAreaId(uint32 phaseMask, uint32& zoneid, uint32& areaid, float x, float y, float z) const;
    void GetZoneAndAreaId(uint32& zoneid, uint32& areaid, float x, float y, float z) const { GetZoneAndAreaId(PHASEMASK_NORMAL, zoneid, areaid, x, y, z); }

    void MoveAllCreaturesInMoveList();
    void MoveAllGameObjectsInMoveList();
    void MoveAllDynamicObjectsInMoveList();
    void RemoveAllObjectsInRemoveList();
    virtual void RemoveAllPlayers();

    [[nodiscard]] bool CreatureRespawnRelocation(Creature* c, bool diffGridOnly);
    [[nodiscard]] bool GameObjectRespawnRelocation(GameObject* go, bool diffGridOnly);

    // pussywizard: global source of guid for units spawned by spells (one per map, to avoid conflicts)
    ObjectGuid::LowType GenerateSpawnedUnitGuid() { return _spawnedUnitGuidGenerator++; }

    [[nodiscard]] uint32 GetInstanceId() const { return i_InstanceId; }
    [[nodiscard]] uint8 GetSpawnMode() const { return (i_spawnMode); }
    virtual EnterState CannotEnter(Player* /*player*/, bool /*loginCheck = false*/) { return CAN_ENTER; }
    [[nodiscard]] const char* GetMapName() const;

    // have meaning only for instanced map (that have set script)
    [[nodiscard]] virtual uint32 GetScriptId() const { return 0; }
    [[nodiscard]] virtual std::string const& GetScriptName() const;
    [[nodiscard]] InstanceScript* ToInstanceScript() { if (i_InstanceId) return i_data; return nullptr; }

    [[nodiscard]] bool Instanceable() const { return i_mapEntry && i_mapEntry->Instanceable(); }
    [[nodiscard]] bool IsDungeon() const { return i_mapEntry && i_mapEntry->IsDungeon(); }
    [[nodiscard]] bool IsNonRaidDungeon() const { return i_mapEntry && i_mapEntry->IsNonRaidDungeon(); }
    [[nodiscard]] bool IsRaid() const { return i_mapEntry && i_mapEntry->IsRaid(); }
    [[nodiscard]] bool IsRaidOrHeroicDungeon() const { return IsRaid() || (i_spawnMode > DUNGEON_DIFFICULTY_NORMAL); }
    [[nodiscard]] bool IsHeroic() const { return IsRaid() ? (i_spawnMode >= RAID_DIFFICULTY_10MAN_HEROIC) : (i_spawnMode >= DUNGEON_DIFFICULTY_HEROIC); }
    [[nodiscard]] bool Is25ManRaid() const { return IsRaid() && (i_spawnMode & RAID_DIFFICULTY_MASK_25MAN); }   // since 25man difficulties are 1 and 3, we can check them like that
    [[nodiscard]] bool IsBattleground() const { return i_mapEntry && i_mapEntry->IsBattleground(); }
    [[nodiscard]] bool IsBattleArena() const { return i_mapEntry && i_mapEntry->IsBattleArena(); }
    [[nodiscard]] bool IsBattlegroundOrArena() const { return i_mapEntry && i_mapEntry->IsBattlegroundOrArena(); }
    [[nodiscard]] bool GetEntrancePos(int32& mapid, float& x, float& y)
    {
        if (!i_mapEntry)
            return false;
        return i_mapEntry->GetEntrancePos(mapid, x, y);
    }

    void AddObjectToRemoveList(WorldObject* obj);
    void AddObjectToSwitchList(WorldObject* obj, bool on);
    virtual void DelayedUpdate(const uint32 diff);

    void UpdateObjectVisibility(WorldObject* obj, Cell cell, CellCoord cellpair);
    void UpdateObjectsVisibilityFor(Player* player, Cell cell, CellCoord cellpair);

    void resetMarkedCells() { marked_cells.reset(); }
    [[nodiscard]] bool isCellMarked(uint32 pCellId) { return marked_cells.test(pCellId); }
    void markCell(uint32 pCellId) { marked_cells.set(pCellId); }

    [[nodiscard]] bool HavePlayers() const { return !m_mapRefMgr.isEmpty(); }
    [[nodiscard]] uint32 GetPlayersCountExceptGMs() const;
    [[nodiscard]] bool ActiveObjectsNearGrid(MapGridType const& ngrid) const;

    void AddWorldObject(WorldObject* obj) { i_worldObjects.insert(obj); }
    void RemoveWorldObject(WorldObject* obj) { i_worldObjects.erase(obj); }

    void SendToPlayers(WorldPacket const* data) const;

    typedef MapRefMgr PlayerList;
    [[nodiscard]] PlayerList const& GetPlayers() const { return m_mapRefMgr; }

    //per-map script storage
    void ScriptsStart(std::map<uint32, std::multimap<uint32, ScriptInfo> > const& scripts, uint32 id, Object* source, Object* target);
    void ScriptCommandStart(ScriptInfo const& script, uint32 delay, Object* source, Object* target);

    // must called with AddToWorld
    template<class T>
    void AddToActive(T* obj);

    // must called with RemoveFromWorld
    template<class T>
    void RemoveFromActive(T* obj);

    Creature* GetCreature(ObjectGuid const& guid);
    GameObject* GetGameObject(ObjectGuid const& guid);
    Transport* GetTransport(ObjectGuid const& guid);
    DynamicObject* GetDynamicObject(ObjectGuid const& guid);
    Pet* GetPet(ObjectGuid const& guid);

    MapStoredObjectTypesContainer& GetObjectsStore() { return _objectsStore; }
    CreatureBySpawnIdContainer& GetCreatureBySpawnIdStore() { return _creatureBySpawnIdStore; }
    GameObjectBySpawnIdContainer& GetGameObjectBySpawnIdStore() { return _gameobjectBySpawnIdStore; }

    std::unordered_set<Corpse*>* GetCorpsesInCell(uint32 cellId)
    {
        auto itr = _corpsesByGrid.find(cellId);
        if (itr != _corpsesByGrid.end())
            return &itr->second;

        return nullptr;
    }

    Corpse* GetCorpseByPlayer(ObjectGuid const& ownerGuid) const
    {
        auto itr = _corpsesByPlayer.find(ownerGuid);
        if (itr != _corpsesByPlayer.end())
            return itr->second;

        return nullptr;
    }

    MapInstanced* ToMapInstanced() { if (Instanceable()) return reinterpret_cast<MapInstanced*>(this); return nullptr; }
    MapInstanced const* ToMapInstanced() const { if (Instanceable()) return reinterpret_cast<MapInstanced const*>(this); return nullptr; }

    InstanceMap* ToInstanceMap() { if (IsDungeon()) return reinterpret_cast<InstanceMap*>(this); return nullptr; }
    InstanceMap const* ToInstanceMap() const { if (IsDungeon()) return reinterpret_cast<InstanceMap const*>(this); return nullptr; }

    BattlegroundMap* ToBattlegroundMap() { if (IsBattlegroundOrArena()) return reinterpret_cast<BattlegroundMap*>(this); return nullptr; }
    BattlegroundMap const* ToBattlegroundMap() const { if (IsBattlegroundOrArena()) return reinterpret_cast<BattlegroundMap const*>(this); return nullptr; }

    [[nodiscard]] float GetGridHeight(float x, float y) const;

    typedef std::unordered_set<WorldObject*> ActiveNonPlayers;
    [[nodiscard]] ActiveNonPlayers const& GetActiveNonPlayers() const { return m_activeNonPlayers; }

    void DoDelayedMovesAndRemoves();

    [[nodiscard]] bool IsRemovalGrid(float x, float y) const
    {
        GridCoord p = Acore::ComputeGridCoord(x, y);
        return !IsGridCreated(p) || IsGridMarkedForRemoval(p);
    }

    [[nodiscard]] bool IsGridMarkedForRemoval(GridCoord const& p) const { return _mapGridManager.IsGridMarkedForRemoval(p.x_coord, p.y_coord); }

    template<HighGuid high> [[nodiscard]] ObjectGuidGeneratorBase const& GetGuidSequenceGenerator() const { return const_cast<Map*>(this)->GetGuidSequenceGenerator<high>(); }

    [[nodiscard]] time_t GetInstanceResetPeriod() const { return _instanceResetPeriod; }
    void InitInstanceResetPeriod(time_t period) { _instanceResetPeriod = period; }

    void AddCorpse(Corpse* corpse);
    void RemoveCorpse(Corpse* corpse);
    Corpse* ConvertCorpseToBones(ObjectGuid const& ownerGuid, bool insignia = false);
    void RemoveOldCorpses();
    void SendInitTransports(Player* player);
    void SendRemoveTransports(Player* player);
    void SendUpdateTransports(Player* player);
    void SendZoneDynamicInfo(Player* player);

    void SetZoneMusic(uint32 zoneId, uint32 musicId);
    void SetZoneWeather(uint32 zoneId, WeatherState weatherId, float weatherGrade);
    void SetZoneOverrideLight(uint32 zoneId, uint32 lightId, uint32 fadeInTime);

    void UpdateAreaDependentAuras();

    ZoneDynamicInfo* GetZoneDynamicInfo(uint32 zoneId);

    template<typename Worker>
    void VisitAllObjects(float const& x, float const& y, float radius, Worker&& worker, bool dont_load = true);

    template<typename Worker>
    void VisitAllWorldObjects(float const& x, float const& y, float radius, Worker&& worker, bool dont_load = true);

    template<typename Notifier>
    void VisitGrid(const float& x, const float& y, float radius, Notifier& notifier, bool dont_load = true);

    template<typename Notifier>
    void VisitWorld(const float& x, const float& y, float radius, Notifier& notifier, bool dont_load = true);

    void UpdateIteratorBack(Player* player);

    TempSummon* SummonCreature(uint32 entry, Position const& pos, SummonPropertiesEntry const* properties = nullptr, uint32 duration = 0, Unit* summoner = nullptr, uint32 spellId = 0, uint32 vehId = 0, ObjectGuid privateObjectOwner = ObjectGuid::Empty);
    GameObject* SummonGameObject(uint32 entry, float x, float y, float z, float ang, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime, ObjectGuid privateObjectOwner = ObjectGuid::Empty);
    GameObject* SummonGameObject(uint32 entry, Position const& pos, float rotation0, float rotation1, float rotation2, float rotation3, uint32 respawnTime, ObjectGuid privateObjectOwner = ObjectGuid::Empty);
    void SummonCreatureGroup(uint8 group, std::list<TempSummon*>* list = nullptr);

    MotionTransport* GetMotionTransport(ObjectGuid::LowType guid);

    // pussywizard:
    std::unordered_set<Corpse*> const* GetCorpseBones() const { return &_corpseBones; }
    void DeleteCorpseData();
    void LoadCorpseData();

    // Checks collision with all GameObjects in this Map including M2 GOs
    [[nodiscard]] bool GetObjectHitPos(uint32 phasemask, float x1, float y1, float z1, float x2, float y2, float z2, float& rx, float& ry, float& rz, float modifyDist) const;

    [[nodiscard]] bool CanReachPositionAndGetValidCoords(WorldObject const* source, PathGenerator const& path, float& destX, float& destY, float& destZ, bool failOnCollision = true, bool failOnSlopes = true) const;
    [[nodiscard]] bool CanReachPositionAndGetValidCoords(WorldObject const* source, float& destX, float& destY, float& destZ, bool failOnCollision = true, bool failOnSlopes = true) const;
    [[nodiscard]] bool CheckCollisionAndGetValidCoords(WorldObject const* source, float startX, float startY, float startZ, float& destX, float& destY, float& destZ, bool failOnCollision = false) const;

    [[nodiscard]] TransportsContainer const& GetAllTransports() const { return _transports; }

    DataMap CustomData;

    template<HighGuid high>
    inline ObjectGuid::LowType GenerateLowGuid()
    {
        static_assert(ObjectGuidTraits<high>::MapSpecific, "Only map specific guid can be generated in Map context");
        return GetGuidSequenceGenerator<high>().Generate();
    }

    void AddUpdateObject(Object* obj)
    {
        _updateObjects.insert(obj);
    }

    void RemoveUpdateObject(Object* obj)
    {
        _updateObjects.erase(obj);
    }

    size_t GetUpdatableObjectsCount() const { return _updatableObjectList.size(); }

    virtual std::string GetDebugInfo() const;

    uint32 GetCreatedGridsCount();
    uint32 GetLoadedGridsCount();
    uint32 GetCreatedCellsInGridCount(uint16 const x, uint16 const y);
    uint32 GetCreatedCellsInMapCount();

    void AddObjectToPendingUpdateList(WorldObject* obj);
    void RemoveObjectFromMapUpdateList(WorldObject* obj);

    typedef std::vector<WorldObject*> UpdatableObjectList;
    typedef std::unordered_set<WorldObject*> PendingAddUpdatableObjectList;

    void AddWorldObjectToFarVisibleMap(WorldObject* obj);
    void RemoveWorldObjectFromFarVisibleMap(WorldObject* obj);
    void AddWorldObjectToZoneWideVisibleMap(uint32 zoneId, WorldObject* obj);
    void RemoveWorldObjectFromZoneWideVisibleMap(uint32 zoneId, WorldObject* obj);
    ZoneWideVisibleWorldObjectsSet const* GetZoneWideVisibleWorldObjectsForZone(uint32 zoneId) const;

    [[nodiscard]] uint32 GetPlayerCountInZone(uint32 zoneId) const
    {
        if (auto const& it = _zonePlayerCountMap.find(zoneId); it != _zonePlayerCountMap.end())
            return it->second;

        return 0;
    };

    // Work-stealing scheduler integration
    void UpdateSequential(uint32 t_diff, uint32 p_diff);
    void UpdateParallel(uint32 t_diff, uint32 p_diff, Acore::WorkStealingScheduler* scheduler);
    void UpdatePlayersParallel(uint32 t_diff, uint32 p_diff, Acore::WorkStealingScheduler* scheduler);
    void UpdateCreaturesParallel(uint32 t_diff, Acore::WorkStealingScheduler* scheduler);

private:

    template<class T> void InitializeObject(T* obj);
    void AddCreatureToMoveList(Creature* c);
    void RemoveCreatureFromMoveList(Creature* c);
    void AddGameObjectToMoveList(GameObject* go);
    void RemoveGameObjectFromMoveList(GameObject* go);
    void AddDynamicObjectToMoveList(DynamicObject* go);
    void RemoveDynamicObjectFromMoveList(DynamicObject* go);

    std::vector<Creature*> _creaturesToMove;
    std::vector<GameObject*> _gameObjectsToMove;
    std::vector<DynamicObject*> _dynamicObjectsToMove;

    bool EnsureGridLoaded(Cell const& cell);
    MapGridType* GetMapGrid(uint16 const x, uint16 const y);

    void ScriptsProcess();

    void SendObjectUpdates();

    // Work-stealing helper structs
    struct PlayerUpdateData
    {
        Player* player;
        uint32 t_diff;
        uint32 p_diff;
    };

    struct CreatureUpdateData
    {
        Creature* creature;
        uint32 diff;
    };

protected:
    // Type specific code for add/remove to/from grid
    template<class T>
    void AddToGrid(T* object, Cell const& cell);

    std::mutex Lock;
    std::shared_mutex MMapLock;

    MapGridManager _mapGridManager;
    MapEntry const* i_mapEntry;
    uint8 i_spawnMode;
    uint32 i_InstanceId;
    uint32 m_unloadTimer;
    float m_VisibleDistance;
    DynamicMapTree _dynamicTree;
    time_t _instanceResetPeriod; // pussywizard

    MapRefMgr m_mapRefMgr;
    MapRefMgr::iterator m_mapRefIter;

    TransportsContainer _transports;
    TransportsContainer::iterator _transportsUpdateIter;

private:
    Player* _GetScriptPlayerSourceOrTarget(Object* source, Object* target, const ScriptInfo* scriptInfo) const;
    Creature* _GetScriptCreatureSourceOrTarget(Object* source, Object* target, const ScriptInfo* scriptInfo, bool bReverse = false) const;
    Unit* _GetScriptUnit(Object* obj, bool isSource, const ScriptInfo* scriptInfo) const;
    Player* _GetScriptPlayer(Object* obj, bool isSource, const ScriptInfo* scriptInfo) const;
    Creature* _GetScriptCreature(Object* obj, bool isSource, const ScriptInfo* scriptInfo) const;
    WorldObject* _GetScriptWorldObject(Object* obj, bool isSource, const ScriptInfo* scriptInfo) const;
    void _ScriptProcessDoor(Object* source, Object* target, const ScriptInfo* scriptInfo) const;
    GameObject* _FindGameObject(WorldObject* pWorldObject, ObjectGuid::LowType guid) const;

    //used for fast base_map (e.g. MapInstanced class object) search for
    //InstanceMaps and BattlegroundMaps...
    Map* m_parentMap;

    std::bitset<TOTAL_NUMBER_OF_CELLS_PER_MAP * TOTAL_NUMBER_OF_CELLS_PER_MAP> marked_cells;

    bool i_scriptLock;
    std::unordered_set<WorldObject*> i_objectsToRemove;

    typedef std::multimap<time_t, ScriptAction> ScriptScheduleMap;
    ScriptScheduleMap m_scriptSchedule;

    template<class T>
    void DeleteFromWorld(T*);

    void UpdateNonPlayerObjects(uint32 const diff);

    void _AddObjectToUpdateList(WorldObject* obj);
    void _RemoveObjectFromUpdateList(WorldObject* obj);

    std::unordered_map<ObjectGuid::LowType /*dbGUID*/, time_t> _creatureRespawnTimes;
    std::unordered_map<ObjectGuid::LowType /*dbGUID*/, time_t> _goRespawnTimes;

    std::unordered_map<uint32, uint32> _zonePlayerCountMap;

    ZoneDynamicInfoMap _zoneDynamicInfo;
    IntervalTimer _weatherUpdateTimer;
    uint32 _defaultLight;

    IntervalTimer _corpseUpdateTimer;

    template<HighGuid high>
    inline ObjectGuidGeneratorBase& GetGuidSequenceGenerator()
    {
        auto itr = _guidGenerators.find(high);
        if (itr == _guidGenerators.end())
            itr = _guidGenerators.insert(std::make_pair(high, std::unique_ptr<ObjectGuidGenerator<high>>(new ObjectGuidGenerator<high>()))).first;

        return *itr->second;
    }

    std::map<HighGuid, std::unique_ptr<ObjectGuidGeneratorBase>> _guidGenerators;
    MapStoredObjectTypesContainer _objectsStore;
    CreatureBySpawnIdContainer _creatureBySpawnIdStore;
    GameObjectBySpawnIdContainer _gameobjectBySpawnIdStore;
    std::unordered_map<uint32/*gridId*/, std::unordered_set<Corpse*>> _corpsesByGrid;
    std::unordered_map<ObjectGuid, Corpse*> _corpsesByPlayer;
    std::unordered_set<Corpse*> _corpseBones;

    std::unordered_set<Object*> _updateObjects;

    UpdatableObjectList _updatableObjectList;
    PendingAddUpdatableObjectList _pendingAddUpdatableObjectList;
    IntervalTimer _updatableObjectListRecheckTimer;
    ZoneWideVisibleWorldObjectsMap _zoneWideVisibleWorldObjectsMap;

    ActiveNonPlayers m_activeNonPlayers;
    ActiveNonPlayers::iterator m_activeNonPlayersIter;

    InstanceScript* i_data;
    ObjectGuid::LowType _spawnedUnitGuidGenerator;

    std::unordered_set<WorldObject*> i_worldObjects;
};

enum InstanceResetMethod
{
    INSTANCE_RESET_ALL,                 // reset all option under portrait, resets only normal 5-mans
    INSTANCE_RESET_CHANGE_DIFFICULTY,   // on changing difficulty
    INSTANCE_RESET_GLOBAL,              // global id reset
    INSTANCE_RESET_GROUP_JOIN,          // on joining group
    INSTANCE_RESET_GROUP_LEAVE          // on leaving group
};

class InstanceMap : public Map
{
public:
    InstanceMap(uint32 id, uint32 InstanceId, uint8 SpawnMode, Map* _parent);
    ~InstanceMap() override;
    bool AddPlayerToMap(Player*) override;
    void RemovePlayerFromMap(Player*, bool) override;
    void AfterPlayerUnlinkFromMap() override;
    void Update(const uint32, const uint32, bool thread = true) override;
    void CreateInstanceScript(bool load, std::string data, uint32 completedEncounterMask);
    bool Reset(uint8 method, GuidList* globalSkipList = nullptr);
    [[nodiscard]] uint32 GetScriptId() const { return i_script_id; }
    [[nodiscard]] std::string const& GetScriptName() const;
    [[nodiscard]] InstanceScript* GetInstanceScript() { return instance_data; }
    [[nodiscard]] InstanceScript const* GetInstanceScript() const { return instance_data; }
    void PermBindAllPlayers();
    void UnloadAll() override;
    EnterState CannotEnter(Player* player, bool loginCheck = false) override;
    void SendResetWarnings(uint32 timeLeft) const;

    [[nodiscard]] uint32 GetMaxPlayers() const;
    [[nodiscard]] uint32 GetMaxResetDelay() const;

    void InitVisibilityDistance() override;

    std::string GetDebugInfo() const override;

private:
    bool m_resetAfterUnload;
    bool m_unloadWhenEmpty;
    InstanceScript* instance_data;
    uint32 i_script_id;
};

class BattlegroundMap : public Map
{
public:
    BattlegroundMap(uint32 id, uint32 InstanceId, Map* _parent, uint8 spawnMode);
    ~BattlegroundMap() override;

    bool AddPlayerToMap(Player*) override;
    void RemovePlayerFromMap(Player*, bool) override;
    EnterState CannotEnter(Player* player, bool loginCheck = false) override;
    void SetUnload();
    //void UnloadAll(bool pForce);
    void RemoveAllPlayers() override;

    void InitVisibilityDistance() override;
    Battleground* GetBG() { return m_bg; }
    void SetBG(Battleground* bg) { m_bg = bg; }
private:
    Battleground* m_bg;
};

template<class T, class CONTAINER>
inline void Map::Visit(Cell const& cell, TypeContainerVisitor<T, CONTAINER>& visitor)
{
    uint32 const grid_x = cell.GridX();
    uint32 const grid_y = cell.GridY();

    // If grid is not loaded, nothing to visit.
    if (!IsGridLoaded(GridCoord(grid_x, grid_y)))
        return;

    GetMapGrid(grid_x, grid_y)->VisitCell(cell.CellX(), cell.CellY(), visitor);
}

#endif
