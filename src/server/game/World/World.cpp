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

/** \file
    \ingroup world
*/

#include "World.h"
#include "AccountMgr.h"
#include "AchievementMgr.h"
#include "AddonMgr.h"
#include "ArenaTeamMgr.h"
#include "ArenaSeasonMgr.h"
#include "AuctionHouseMgr.h"
#include "AutobroadcastMgr.h"
#include "BattlefieldMgr.h"
#include "BattlegroundMgr.h"
#include "CalendarMgr.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "CharacterDatabaseCleaner.h"
#include "Chat.h"
#include "ChatPackets.h"
#include "Common.h"
#include "ConditionMgr.h"
#include "Config.h"
#include "CreatureAIRegistry.h"
#include "CreatureGroups.h"
#include "CreatureTextMgr.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "DisableMgr.h"
#include "DynamicVisibility.h"
#include "GameEventMgr.h"
#include "GameGraveyard.h"
#include "GameTime.h"
#include "GitRevision.h"
#include "GridNotifiersImpl.h"
#include "GroupMgr.h"
#include "GuildMgr.h"
#include "IPLocation.h"
#include "InstanceSaveMgr.h"
#include "ItemEnchantmentMgr.h"
#include "LFGMgr.h"
#include "Log.h"
#include "LootItemStorage.h"
#include "LootMgr.h"
#include "M2Stores.h"
#include "MMapFactory.h"
#include "MapMgr.h"
#include "Metric.h"
#include "MotdMgr.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "OutdoorPvPMgr.h"
#include "PetitionMgr.h"
#include "Player.h"
#include "PlayerDump.h"
#include "PoolMgr.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "ServerMailMgr.h"
#include "SkillDiscovery.h"
#include "SkillExtraItems.h"
#include "SmartAI.h"
#include "SpellMgr.h"
#include "TaskScheduler.h"
#include "TicketMgr.h"
#include "Transport.h"
#include "TransportMgr.h"
#include "UpdateTime.h"
#include "Util.h"
#include "VMapFactory.h"
#include "VMapMgr2.h"
#include "Warden.h"
#include "WardenCheckMgr.h"
#include "WaypointMovementGenerator.h"
#include "WeatherMgr.h"
#include "WhoListCacheMgr.h"
#include "WorldGlobals.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "WorldState.h"
#include "WorldStateDefines.h"
#include <boost/asio/ip/address.hpp>
#include <cmath>

std::atomic_long World::_stopEvent = false;
uint8 World::_exitCode = SHUTDOWN_EXIT_CODE;
uint32 World::m_worldLoopCounter = 0;

float World::_maxVisibleDistanceOnContinents = DEFAULT_VISIBILITY_DISTANCE;
float World::_maxVisibleDistanceInInstances  = DEFAULT_VISIBILITY_INSTANCE;
float World::_maxVisibleDistanceInBGArenas   = DEFAULT_VISIBILITY_BGARENAS;

Realm realm;

/// World constructor
World::World()
{
    _allowedSecurityLevel = SEC_PLAYER;
    _allowMovement = true;
    _shutdownMask = 0;
    _shutdownTimer = 0;
    _nextDailyQuestReset = 0s;
    _nextWeeklyQuestReset = 0s;
    _nextMonthlyQuestReset = 0s;
    _nextRandomBGReset = 0s;
    _nextCalendarOldEventsDeletionTime = 0s;
    _nextGuildReset = 0s;
    _defaultDbcLocale = LOCALE_enUS;
    _mail_expire_check_timer = 0s;
    _isClosed = false;
    _cleaningFlags = 0;
    _dbClientCacheVersion = 0;
}

/// World destructor
World::~World()
{
    CliCommandHolder* command = nullptr;
    while (_cliCmdQueue.next(command))
        delete command;

    VMAP::VMapFactory::clear();
    MMAP::MMapFactory::clear();
}

std::unique_ptr<IWorld>& getWorldInstance()
{
    static std::unique_ptr<IWorld> instance = std::make_unique<World>();
    return instance;
}

bool World::IsClosed() const
{
    return _isClosed;
}

void World::SetClosed(bool val)
{
    _isClosed = val;

    // Invert the value, for simplicity for scripters.
    sScriptMgr->OnOpenStateChange(!val);
}

/// Initialize config values
void World::LoadConfigSettings(bool reload)
{
    if (reload)
    {
        if (!sConfigMgr->Reload())
        {
            LOG_ERROR("server.loading", "World settings reload fail: can't read settings.");
            return;
        }

        sLog->LoadFromConfig();
        sMetric->LoadFromConfigs();
    }

    // Set realm id and enable db logging
    sLog->SetRealmId(realm.Id.Realm);

    sScriptMgr->OnBeforeConfigLoad(reload);

    // load update time related configs
    sWorldUpdateTime.LoadFromConfig();

    ///- Read the player limit and the Message of the day from the config file
    if (!reload)
        sWorldSessionMgr->SetPlayerAmountLimit(sConfigMgr->GetOption<int32>("PlayerLimit", 1000));

    _worldConfig.Initialize(reload);

    for (uint8 i = 0; i < MAX_MOVE_TYPE; ++i)
        playerBaseMoveSpeed[i] = baseMoveSpeed[i] * getRate(RATE_MOVESPEED_PLAYER);

    for (uint8 i = 0; i < MAX_MOVE_TYPE; ++i)
        baseMoveSpeed[i] *= getRate(RATE_MOVESPEED_NPC);

    if (reload)
    {
        sMapMgr->SetMapUpdateInterval(getIntConfig(CONFIG_INTERVAL_MAPUPDATE));

        _timers[WUPDATE_UPTIME].SetInterval(getIntConfig(CONFIG_UPTIME_UPDATE) * MINUTE* IN_MILLISECONDS);
        _timers[WUPDATE_UPTIME].Reset();

        _timers[WUPDATE_CLEANDB].SetInterval(getIntConfig(CONFIG_LOGDB_CLEARINTERVAL) * MINUTE * IN_MILLISECONDS);
        _timers[WUPDATE_CLEANDB].Reset();

        _timers[WUPDATE_AUTOBROADCAST].SetInterval(getIntConfig(CONFIG_AUTOBROADCAST_INTERVAL));
        _timers[WUPDATE_AUTOBROADCAST].Reset();

        // ====================================================================
        // WORK-STEALING SCHEDULER: Reload Configuration
        // ====================================================================
        // When configuration is reloaded at runtime, update MapManager's
        // work-stealing settings dynamically
        if (sMapMgr)
        {
            bool workStealingEnabled = sConfigMgr->GetOption<bool>("WorkStealing.Enabled", false);
            uint32 numThreads = sConfigMgr->GetOption<uint32>("WorkStealing.Threads", 0);
            
            LOG_INFO("server.worldserver", "Work-Stealing Scheduler config reload:");
            LOG_INFO("server.worldserver", "  Enabled: {}", workStealingEnabled ? "YES" : "NO");
            
            if (workStealingEnabled)
            {
                LOG_INFO("server.worldserver", "  Threads: {}", numThreads > 0 ? std::to_string(numThreads) : "auto-detect");
                LOG_INFO("server.worldserver", "  MinPlayersForParallel: {}", 
                         sConfigMgr->GetOption<uint32>("WorkStealing.MinPlayersForParallel", 10));
                LOG_INFO("server.worldserver", "  GrainSize: {}", 
                         sConfigMgr->GetOption<uint32>("WorkStealing.GrainSize", 64));
            }
            
            // Note: Actual scheduler restart would require MapManager support
            // This is informational for now - full reload requires server restart
            LOG_WARN("server.worldserver", "Note: Work-stealing thread count changes require server restart");
        }
    }

    if (getIntConfig(CONFIG_CLIENTCACHE_VERSION) == 0)
    {
        _worldConfig.OverwriteConfigValue<uint32>(CONFIG_CLIENTCACHE_VERSION, _dbClientCacheVersion);
        LOG_INFO("server.loading", "Client cache version set to: {}", _dbClientCacheVersion);
    }

    //visibility on continents
    _maxVisibleDistanceOnContinents = sConfigMgr->GetOption<float>("Visibility.Distance.Continents", DEFAULT_VISIBILITY_DISTANCE);
    if (_maxVisibleDistanceOnContinents < 45 * getRate(RATE_CREATURE_AGGRO))
    {
        LOG_ERROR("server.loading", "Visibility.Distance.Continents can't be less max aggro radius {}", 45 * getRate(RATE_CREATURE_AGGRO));
        _maxVisibleDistanceOnContinents = 45 * getRate(RATE_CREATURE_AGGRO);
    }
    else if (_maxVisibleDistanceOnContinents > MAX_VISIBILITY_DISTANCE)
    {
        LOG_ERROR("server.loading", "Visibility.Distance.Continents can't be greater {}", MAX_VISIBILITY_DISTANCE);
        _maxVisibleDistanceOnContinents = MAX_VISIBILITY_DISTANCE;
    }

    //visibility in instances
    _maxVisibleDistanceInInstances = sConfigMgr->GetOption<float>("Visibility.Distance.Instances", DEFAULT_VISIBILITY_INSTANCE);
    if (_maxVisibleDistanceInInstances < 45 * getRate(RATE_CREATURE_AGGRO))
    {
        LOG_ERROR("server.loading", "Visibility.Distance.Instances can't be less max aggro radius {}", 45 * getRate(RATE_CREATURE_AGGRO));
        _maxVisibleDistanceInInstances = 45 * getRate(RATE_CREATURE_AGGRO);
    }
    else if (_maxVisibleDistanceInInstances > MAX_VISIBILITY_DISTANCE)
    {
        LOG_ERROR("server.loading", "Visibility.Distance.Instances can't be greater {}", MAX_VISIBILITY_DISTANCE);
        _maxVisibleDistanceInInstances = MAX_VISIBILITY_DISTANCE;
    }

    //visibility in BG/Arenas
    _maxVisibleDistanceInBGArenas = sConfigMgr->GetOption<float>("Visibility.Distance.BGArenas", DEFAULT_VISIBILITY_BGARENAS);
    if (_maxVisibleDistanceInBGArenas < 45 * getRate(RATE_CREATURE_AGGRO))
    {
        LOG_ERROR("server.loading", "Visibility.Distance.BGArenas can't be less max aggro radius {}", 45 * getRate(RATE_CREATURE_AGGRO));
        _maxVisibleDistanceInBGArenas = 45 * getRate(RATE_CREATURE_AGGRO);
    }
    else if (_maxVisibleDistanceInBGArenas > MAX_VISIBILITY_DISTANCE)
    {
        LOG_ERROR("server.loading", "Visibility.Distance.BGArenas can't be greater {}", MAX_VISIBILITY_DISTANCE);
        _maxVisibleDistanceInBGArenas = MAX_VISIBILITY_DISTANCE;
    }

    sScriptMgr->OnAfterConfigLoad(reload);
}

void World::SetInitialWorldSettings()
{
    // ====================================================================
    // WORK-STEALING SCHEDULER: Startup Banner
    // ====================================================================
    LOG_INFO("server.worldserver", "");
    LOG_INFO("server.worldserver", "========================================");
    LOG_INFO("server.worldserver", "  Work-Stealing Scheduler Status");
    LOG_INFO("server.worldserver", "========================================");
    
    bool workStealingEnabled = sConfigMgr->GetOption<bool>("WorkStealing.Enabled", false);
    
    if (workStealingEnabled && sMapMgr && sMapMgr->UseWorkStealing())
    {
        auto* scheduler = sMapMgr->GetWorkStealingScheduler();
        if (scheduler)
        {
            LOG_INFO("server.worldserver", "Status: ENABLED");
            LOG_INFO("server.worldserver", "Worker Threads: {}", scheduler->GetNumWorkers());
            LOG_INFO("server.worldserver", "Grain Size: {}", 
                     sConfigMgr->GetOption<uint32>("WorkStealing.GrainSize", 64));
            LOG_INFO("server.worldserver", "Min Players (Parallel): {}", 
                     sConfigMgr->GetOption<uint32>("WorkStealing.MinPlayersForParallel", 10));
            LOG_INFO("server.worldserver", "Metrics Logging: {}", 
                     sConfigMgr->GetOption<bool>("WorkStealing.LogMetrics", false) ? "ON" : "OFF");
        }
        else
        {
            LOG_WARN("server.worldserver", "Status: ENABLED but scheduler initialization FAILED");
        }
    }
    else
    {
        LOG_INFO("server.worldserver", "Status: DISABLED");
        LOG_INFO("server.worldserver", "Using sequential map updates");
    }
    
    LOG_INFO("server.worldserver", "========================================");
    LOG_INFO("server.worldserver", "");

    // Server startup message
    LOG_INFO("server.worldserver", "{} (worldserver-daemon)", GitRevision::GetFullVersion());
    LOG_INFO("server.worldserver", "<Ctrl-C> to stop.\n");

    // Set starting time
    GameTime::UpdateGameTimers();

    // Load config settings
    LoadConfigSettings();

    // Database system initialization
    LOG_INFO("server.worldserver", "Initialize data stores...");
    LoadDBVersion();

    // Load data tables
    LOG_INFO("server.worldserver", "Loading DataStores...");
    LoadDataStores();

    LOG_INFO("server.worldserver", "Loading GameObjects...");
    sObjectMgr->LoadGameObjectTemplate();

    LOG_INFO("server.worldserver", "Loading Item templates...");
    sObjectMgr->LoadItemTemplates();

    LOG_INFO("server.worldserver", "Loading Player Level Stats...");
    sObjectMgr->LoadPlayerInfo();

    LOG_INFO("server.worldserver", "Loading Exploration BaseXP...");
    sObjectMgr->LoadExplorationBaseXP();

    LOG_INFO("server.worldserver", "Loading Pet Name Parts...");
    sObjectMgr->LoadPetNames();

    LOG_INFO("server.worldserver", "Loading Base Creature data...");
    sObjectMgr->LoadCreatureTemplate();

    LOG_INFO("server.worldserver", "Loading Creature model based info...");
    sObjectMgr->LoadCreatureModelInfo();

    LOG_INFO("server.worldserver", "Loading Equipment templates...");
    sObjectMgr->LoadEquipmentTemplates();

    LOG_INFO("server.worldserver", "Loading Creature templates...");
    sObjectMgr->LoadCreatureTemplates();

    LOG_INFO("server.worldserver", "Loading Reputation Reward Rates...");
    sObjectMgr->LoadReputationRewardRate();

    LOG_INFO("server.worldserver", "Loading Creature Reputation OnKill Data...");
    sObjectMgr->LoadReputationOnKill();

    LOG_INFO("server.worldserver", "Loading Reputation Spillover Data...");
    sObjectMgr->LoadReputationSpilloverTemplate();

    LOG_INFO("server.worldserver", "Loading Points Of Interest Data...");
    sObjectMgr->LoadPointsOfInterest();

    LOG_INFO("server.worldserver", "Loading Creature Addon Data...");
    sObjectMgr->LoadCreatureAddons();

    LOG_INFO("server.worldserver", "Loading Creature Summon Data...");
    sObjectMgr->LoadTempSummons();

    LOG_INFO("server.worldserver", "Loading Gameobject Data...");
    sObjectMgr->LoadGameObjectLocales();
    sObjectMgr->LoadGameobjects();

    LOG_INFO("server.worldserver", "Loading Spell Custom Attributes...");
    sSpellMgr->LoadSpellCustomAttr();

    LOG_INFO("server.worldserver", "Loading Game Event Data...");
    sGameEventMgr->LoadFromDB();

    LOG_INFO("server.worldserver", "Loading Creature Pool Data...");
    sPoolMgr->LoadCreaturePools();

    LOG_INFO("server.worldserver", "Loading Gameobject Pool Data...");
    sPoolMgr->LoadGameobjectPools();

    LOG_INFO("server.worldserver", "Loading Quest Pool Data...");
    sPoolMgr->LoadQuestPools();

    LOG_INFO("server.worldserver", "Initializing Dailies Reset Time...");
    InitDailyQuestResetTime();

    LOG_INFO("server.worldserver", "Initializing Weekly Reset Time...");
    InitWeeklyQuestResetTime();

    LOG_INFO("server.worldserver", "Initializing Monthly Reset Time...");
    InitMonthlyQuestResetTime();

    LOG_INFO("server.worldserver", "Initializing Random BG Reset Time...");
    InitRandomBGResetTime();

    LOG_INFO("server.worldserver", "Initializing Calendar Old Events Deletion Time...");
    InitCalendarOldEventsDeletionTime();

    LOG_INFO("server.worldserver", "Initializing Guild Reset Time...");
    InitGuildResetTime();

    LOG_INFO("server.worldserver", "Loading Autobroadcasts...");
    LoadAutobroadcasts();

    LOG_INFO("server.worldserver", "Server loaded.");

    // ====================================================================
    // WORK-STEALING SCHEDULER: Post-Load Validation
    // ====================================================================
    if (workStealingEnabled)
    {
        LOG_INFO("server.worldserver", "");
        LOG_INFO("server.worldserver", "Validating Work-Stealing Scheduler...");
        
        if (sMapMgr && sMapMgr->UseWorkStealing())
        {
            auto* scheduler = sMapMgr->GetWorkStealingScheduler();
            if (scheduler)
            {
                LOG_INFO("server.worldserver", "  ✓ Scheduler initialized successfully");
                LOG_INFO("server.worldserver", "  ✓ {} worker threads ready", scheduler->GetNumWorkers());
                
                // Validate configuration sanity
                uint32 grainSize = sConfigMgr->GetOption<uint32>("WorkStealing.GrainSize", 64);
                uint32 minPlayers = sConfigMgr->GetOption<uint32>("WorkStealing.MinPlayersForParallel", 10);
                
                if (grainSize < 16)
                {
                    LOG_WARN("server.worldserver", "  ⚠ GrainSize ({}) is very small, may cause overhead", grainSize);
                }
                
                if (minPlayers < 5)
                {
                    LOG_WARN("server.worldserver", "  ⚠ MinPlayersForParallel ({}) is very low", minPlayers);
                }
                
                if (scheduler->GetNumWorkers() > std::thread::hardware_concurrency())
                {
                    LOG_WARN("server.worldserver", "  ⚠ Worker threads ({}) exceed hardware threads ({})", 
                             scheduler->GetNumWorkers(), std::thread::hardware_concurrency());
                }
            }
            else
            {
                LOG_ERROR("server.worldserver", "  ✗ Scheduler initialization FAILED!");
                LOG_ERROR("server.worldserver", "  ✗ Falling back to sequential updates");
            }
        }
        
        LOG_INFO("server.worldserver", "");
    }
}

void World::LoadDataStores()
{
    uint32 oldMSTime = getMSTime();

    LOG_INFO("server.loading", "Loading Achievement Reward Data...");
    sAchievementMgr->LoadRewards();

    LOG_INFO("server.loading", "Loading Addon Mgr Data...");
    sAddonMgr->LoadFromDB();

    LOG_INFO("server.loading", "Loading Achievement Reward Locale...");
    sAchievementMgr->LoadRewardLocales();

    LOG_INFO("server.loading", "Loading Skill Extra Item Table...");
    LoadSkillExtraItemTable();

    LOG_INFO("server.loading", "Loading Skill Perfect Item Table...");
    LoadSkillPerfectItemTable();

    LOG_INFO("server.loading", "Loading Spell effect dbc data corrections...");
    sSpellMgr->LoadSpellInfoCorrections();

    LOG_INFO("server.loading", "Loading Spell Enchant Proc data...");
    sSpellMgr->LoadSpellEnchantProcData();

    LOG_INFO("server.loading", "Loading Charm Infos...");
    sObjectMgr->LoadCharmedInfos();

    LOG_INFO("server.loading", "Loading Disabled Spells...");
    sDisableMgr->LoadDisabledSpells();

    LOG_INFO("server.loading", "Loading Skill Tier Info...");
    sObjectMgr->LoadSkillTiers();

    LOG_INFO("server.loading", "Loading Game Tele data...");
    sObjectMgr->LoadGameTele();

    LOG_INFO("server.loading", "Loading Achievement Criteria data...");
    sAchievementMgr->LoadAchievementCriteriaData();

    LOG_INFO("server.loading", "Loaded Achievement Criteria data in {} ms", getMSTimeDiff(oldMSTime, getMSTime()));
}

void World::InitDailyQuestResetTime()
{
    Seconds wstime = Seconds(sWorldState->getWorldState(WORLD_STATE_CUSTOM_DAILY_QUEST_RESET_TIME));
    _nextDailyQuestReset = wstime > 0s ? wstime : Seconds(Acore::Time::GetNextTimeWithDayAndHour(-1, 6));

    if (wstime == 0s)
    {
        sWorldState->setWorldState(WORLD_STATE_CUSTOM_DAILY_QUEST_RESET_TIME, _nextDailyQuestReset.count());
    }
}

void World::InitWeeklyQuestResetTime()
{
    Seconds wstime = Seconds(sWorldState->getWorldState(WORLD_STATE_CUSTOM_WEEKLY_QUEST_RESET_TIME));
    _nextWeeklyQuestReset = wstime > 0s ? wstime : Seconds(Acore::Time::GetNextTimeWithDayAndHour(4, 6));

    if (wstime == 0s)
    {
        sWorldState->setWorldState(WORLD_STATE_CUSTOM_WEEKLY_QUEST_RESET_TIME, _nextWeeklyQuestReset.count());
    }
}

void World::InitMonthlyQuestResetTime()
{
    Seconds wstime = Seconds(sWorldState->getWorldState(WORLD_STATE_CUSTOM_MONTHLY_QUEST_RESET_TIME));
    _nextMonthlyQuestReset = wstime > 0s ? wstime : Seconds(Acore::Time::GetNextTimeWithDayAndHour(-1, 6));

    if (wstime == 0s)
    {
        sWorldState->setWorldState(WORLD_STATE_CUSTOM_MONTHLY_QUEST_RESET_TIME, _nextMonthlyQuestReset.count());
    }
}

void World::InitRandomBGResetTime()
{
    Seconds wstime = Seconds(sWorldState->getWorldState(WORLD_STATE_CUSTOM_BG_DAILY_RESET_TIME));
    _nextRandomBGReset = wstime > 0s ? wstime : Seconds(Acore::Time::GetNextTimeWithDayAndHour(-1, 6));

    if (wstime == 0s)
    {
        sWorldState->setWorldState(WORLD_STATE_CUSTOM_BG_DAILY_RESET_TIME, _nextRandomBGReset.count());
    }
}

void World::InitCalendarOldEventsDeletionTime()
{
    Seconds currentDeletionTime = Seconds(sWorldState->getWorldState(WORLD_STATE_CUSTOM_DAILY_CALENDAR_DELETION_OLD_EVENTS_TIME));
    Seconds nextDeletionTime = currentDeletionTime > 0s ? currentDeletionTime : Seconds(Acore::Time::GetNextTimeWithDayAndHour(-1, getIntConfig(CONFIG_CALENDAR_DELETE_OLD_EVENTS_HOUR)));

    if (currentDeletionTime == 0s)
    {
        sWorldState->setWorldState(WORLD_STATE_CUSTOM_DAILY_CALENDAR_DELETION_OLD_EVENTS_TIME, nextDeletionTime.count());
    }
}

void World::InitGuildResetTime()
{
    Seconds wstime = Seconds(sWorldState->getWorldState(WORLD_STATE_CUSTOM_GUILD_DAILY_RESET_TIME));
    _nextGuildReset = wstime > 0s ? wstime : Seconds(Acore::Time::GetNextTimeWithDayAndHour(-1, 6));

    if (wstime == 0s)
    {
        sWorldState->setWorldState(WORLD_STATE_CUSTOM_GUILD_DAILY_RESET_TIME, _nextGuildReset.count());
    }
}

void World::ResetDailyQuests()
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_DAILY);
    CharacterDatabase.Execute(stmt);

    WorldSessionMgr::SessionMap const& sessionMap = sWorldSessionMgr->GetAllSessions();
    for (WorldSessionMgr::SessionMap::const_iterator itr = sessionMap.begin(); itr != sessionMap.end(); ++itr)
        if (itr->second->GetPlayer())
            itr->second->GetPlayer()->ResetDailyQuestStatus();

    _nextDailyQuestReset = Seconds(Acore::Time::GetNextTimeWithDayAndHour(-1, 6));
    sWorldState->setWorldState(WORLD_STATE_CUSTOM_DAILY_QUEST_RESET_TIME, _nextDailyQuestReset.count());

    // change available dailies
    sPoolMgr->ChangeDailyQuests();
}

void World::LoadDBAllowedSecurityLevel()
{
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_REALMLIST_SECURITY_LEVEL);
    stmt->SetData(0, int32(realm.Id.Realm));
    PreparedQueryResult result = LoginDatabase.Query(stmt);

    if (result)
        SetPlayerSecurityLimit(AccountTypes(result->Fetch()->Get<uint8>()));
}

void World::SetPlayerSecurityLimit(AccountTypes _sec)
{
    AccountTypes sec = _sec < SEC_CONSOLE ? _sec : SEC_PLAYER;
    bool update = sec > _allowedSecurityLevel;
    _allowedSecurityLevel = sec;
    if (update)
        sWorldSessionMgr->KickAllLess(_allowedSecurityLevel);
}

void World::ResetWeeklyQuests()
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_WEEKLY);
    CharacterDatabase.Execute(stmt);

    WorldSessionMgr::SessionMap const& sessionMap = sWorldSessionMgr->GetAllSessions();
    for (WorldSessionMgr::SessionMap::const_iterator itr = sessionMap.begin(); itr != sessionMap.end(); ++itr)
        if (itr->second->GetPlayer())
            itr->second->GetPlayer()->ResetWeeklyQuestStatus();

    _nextWeeklyQuestReset = Seconds(Acore::Time::GetNextTimeWithDayAndHour(4, 6));
    sWorldState->setWorldState(WORLD_STATE_CUSTOM_WEEKLY_QUEST_RESET_TIME, _nextWeeklyQuestReset.count());

    // change available weeklies
    sPoolMgr->ChangeWeeklyQuests();
}

void World::ResetMonthlyQuests()
{
    LOG_INFO("server.worldserver", "Monthly quests reset for all characters.");

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_MONTHLY);
    CharacterDatabase.Execute(stmt);

    WorldSessionMgr::SessionMap const& sessionMap = sWorldSessionMgr->GetAllSessions();
    for (WorldSessionMgr::SessionMap::const_iterator itr = sessionMap.begin(); itr != sessionMap.end(); ++itr)
        if (itr->second->GetPlayer())
            itr->second->GetPlayer()->ResetMonthlyQuestStatus();

    _nextMonthlyQuestReset = Seconds(Acore::Time::GetNextTimeWithMonthAndHour(-1, 6));
    sWorldState->setWorldState(WORLD_STATE_CUSTOM_MONTHLY_QUEST_RESET_TIME, _nextMonthlyQuestReset.count());
}

void World::ResetEventSeasonalQuests(uint16 event_id)
{
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_QUEST_STATUS_SEASONAL);
    stmt->SetData(0, event_id);
    CharacterDatabase.Execute(stmt);

    WorldSessionMgr::SessionMap const& sessionMap = sWorldSessionMgr->GetAllSessions();
    for (WorldSessionMgr::SessionMap::const_iterator itr = sessionMap.begin(); itr != sessionMap.end(); ++itr)
        if (itr->second->GetPlayer())
            itr->second->GetPlayer()->ResetSeasonalQuestStatus(event_id);
}

void World::ResetRandomBG()
{
    LOG_DEBUG("server.worldserver", "Random BG status reset for all characters.");

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_BATTLEGROUND_RANDOM);
    CharacterDatabase.Execute(stmt);

    WorldSessionMgr::SessionMap const& sessionMap = sWorldSessionMgr->GetAllSessions();
    for (WorldSessionMgr::SessionMap::const_iterator itr = sessionMap.begin(); itr != sessionMap.end(); ++itr)
        if (itr->second->GetPlayer())
            itr->second->GetPlayer()->SetRandomWinner(false);

    _nextRandomBGReset = Seconds(Acore::Time::GetNextTimeWithDayAndHour(-1, 6));
    sWorldState->setWorldState(WORLD_STATE_CUSTOM_BG_DAILY_RESET_TIME, _nextRandomBGReset.count());
}

void World::CalendarDeleteOldEvents()
{
    LOG_INFO("server.worldserver", "Calendar deletion of old events.");

    _nextCalendarOldEventsDeletionTime = Seconds(Acore::Time::GetNextTimeWithDayAndHour(-1, getIntConfig(CONFIG_CALENDAR_DELETE_OLD_EVENTS_HOUR)));
    sWorldState->setWorldState(WORLD_STATE_CUSTOM_DAILY_CALENDAR_DELETION_OLD_EVENTS_TIME, _nextCalendarOldEventsDeletionTime.count());
    sCalendarMgr->DeleteOldEvents();
}

void World::ResetGuildCap()
{
    LOG_INFO("server.worldserver", "Guild Daily Cap reset.");

    _nextGuildReset = Seconds(Acore::Time::GetNextTimeWithDayAndHour(-1, 6));
    sWorldState->setWorldState(WORLD_STATE_CUSTOM_GUILD_DAILY_RESET_TIME, _nextGuildReset.count());

    sGuildMgr->ResetTimes();
}

void World::LoadDBVersion()
{
    QueryResult result = WorldDatabase.Query("SELECT db_version, cache_id FROM version LIMIT 1");
    if (result)
    {
        Field* fields = result->Fetch();

        _dbVersion = fields[0].Get<std::string>();

        // will be overwrite by config values if different and non-0
        _dbClientCacheVersion = fields[1].Get<uint32>();
    }

    if (_dbVersion.empty())
        _dbVersion = "Unknown world database.";
}

void World::UpdateAreaDependentAuras()
{
    WorldSessionMgr::SessionMap const& sessionMap = sWorldSessionMgr->GetAllSessions();
    for (WorldSessionMgr::SessionMap::const_iterator itr = sessionMap.begin(); itr != sessionMap.end(); ++itr)
        if (itr->second && itr->second->GetPlayer() && itr->second->GetPlayer()->IsInWorld())
        {
            itr->second->GetPlayer()->UpdateAreaDependentAuras(itr->second->GetPlayer()->GetAreaId());
            itr->second->GetPlayer()->UpdateZoneDependentAuras(itr->second->GetPlayer()->GetZoneId());
        }
}

void World::ProcessQueryCallbacks()
{
    _queryProcessor.ProcessReadyCallbacks();
}

bool World::IsPvPRealm() const
{
    return getIntConfig(CONFIG_GAME_TYPE) == REALM_TYPE_PVP || getIntConfig(CONFIG_GAME_TYPE) == REALM_TYPE_RPPVP || getIntConfig(CONFIG_GAME_TYPE) == REALM_TYPE_FFA_PVP;
}

bool World::IsFFAPvPRealm() const
{
    return getIntConfig(CONFIG_GAME_TYPE) == REALM_TYPE_FFA_PVP;
}

uint32 World::GetNextWhoListUpdateDelaySecs()
{
    if (_timers[WUPDATE_5_SECS].Passed())
        return 1;

    uint32 t = _timers[WUPDATE_5_SECS].GetInterval() - _timers[WUPDATE_5_SECS].GetCurrent();
    t = std::min(t, (uint32)_timers[WUPDATE_5_SECS].GetInterval());

    return uint32(std::ceil(t / 1000.0f));
}

// ====================================================================
// WORK-STEALING SCHEDULER: Periodic Metrics Logging
// ====================================================================
void World::LogWorkStealingMetrics()
{
    if (!sConfigMgr->GetOption<bool>("WorkStealing.LogMetrics", false))
        return;
        
    if (!sMapMgr || !sMapMgr->UseWorkStealing())
        return;
        
    auto* scheduler = sMapMgr->GetWorkStealingScheduler();
    if (!scheduler)
        return;
        
    const auto& metrics = scheduler->GetMetrics();
    
    LOG_INFO("server.worldserver", "======= Work-Stealing Metrics =======");
    LOG_INFO("server.worldserver", "Tasks Spawned: {}", metrics.tasksSpawned.load());
    LOG_INFO("server.worldserver", "Steal Attempts: {}", metrics.stealAttempts.load());
    LOG_INFO("server.worldserver", "Successful Steals: {}", metrics.successfulSteals.load());
    
    if (metrics.stealAttempts.load() > 0)
    {
        double stealSuccessRate = 100.0 * metrics.successfulSteals.load() / metrics.stealAttempts.load();
        LOG_INFO("server.worldserver", "Steal Success Rate: {:.2f}%", stealSuccessRate);
    }
    
    if (metrics.totalUpdateTimeUs.load() > 0)
    {
        LOG_INFO("server.worldserver", "Total Update Time: {} us", metrics.totalUpdateTimeUs.load());
        
        if (metrics.tasksSpawned.load() > 0)
        {
            uint64 avgTimePerTask = metrics.totalUpdateTimeUs.load() / metrics.tasksSpawned.load();
            LOG_INFO("server.worldserver", "Avg Time per Task: {} us", avgTimePerTask);
        }
    }
    
    LOG_INFO("server.worldserver", "====================================");
}

// ====================================================================
// WORK-STEALING SCHEDULER: Shutdown Cleanup
// ====================================================================
void World::ShutdownWorkStealingScheduler()
{
    LOG_INFO("server.worldserver", "Shutting down Work-Stealing Scheduler...");
    
    // MapManager handles scheduler shutdown in its destructor
    // This is just for logging and graceful state transition
    
    if (sMapMgr && sMapMgr->UseWorkStealing())
    {
        auto* scheduler = sMapMgr->GetWorkStealingScheduler();
        if (scheduler)
        {
            // Log final metrics before shutdown
            if (sConfigMgr->GetOption<bool>("WorkStealing.LogMetrics", false))
            {
                LOG_INFO("server.worldserver", "Final Work-Stealing Statistics:");
                LogWorkStealingMetrics();
            }
            
            LOG_INFO("server.worldserver", "Work-Stealing Scheduler shutdown complete");
        }
    }
}

CliCommandHolder::CliCommandHolder(void* callbackArg, char const* command, Print zprint, CommandFinished commandFinished)
    : m_callbackArg(callbackArg), m_command(strdup(command)), m_print(zprint), m_commandFinished(commandFinished)
{
}

CliCommandHolder::~CliCommandHolder()
{
    free(m_command);
}
