/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "Playerbots.h"
#include "BattleGroundTactics.h"
#include "BattlefieldScript.h"
#include "Channel.h"
#include "CheckMountStateAction.h"
#include "Config.h"
#include "BuiltInConfig.h"
#include "DBUpdater.h"
#include "DatabaseEnv.h"
#include "PlayerbotsDatabase.h"
#include <mysqld_error.h>
#include "AllMapScript.h"
#include "GatherNodeMgr.h"
#include "GlobalScript.h"
#include "GuildTaskMgr.h"
#include "Chat.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "GroupScript.h"
#include "LFGMgr.h"
#include "CharacterCache.h"
#include "PlayerScript.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotCommandScript.h"
#include "PlayerbotFactory.h"
#include "PlayerbotGuildMgr.h"
#include "PlayerbotSpellRepository.h"
#include "PlayerbotWorldThreadProcessor.h"
#include "PlayerbotOperations.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "ServerScript.h"
#include "SessionScript.h"
#include "WorldScript.h"
#include "cmath"
#include <algorithm>
#include <random>
#include <ctime>
#include <mutex>
#include <unordered_map>

// Maps (classId, role) -> premade spec number (Playerbots.PremadeSpecName.<class>.<specNo>).
// -1 means unknown combination.
static int SpecNoForClassRole(uint8 classId, std::string const& roleIn)
{
    // Built-in rosters say "healer", config templates say "heal".
    std::string const role = (roleIn == "healer") ? "heal" : roleIn;
    static std::map<std::pair<uint8, std::string>, int> const table = {
        {{1,  "tank"}, 2}, {{1,  "dps"}, 0},
        {{2,  "tank"}, 1}, {{2,  "heal"}, 0}, {{2,  "dps"}, 2},
        {{3,  "dps"}, 0},
        {{4,  "dps"}, 1},
        {{5,  "heal"}, 0}, {{5,  "dps"}, 2},
        {{6,  "tank"}, 0}, {{6,  "dps"}, 1},
        {{7,  "heal"}, 2}, {{7,  "dps"}, 1},
        {{8,  "dps"}, 2},
        {{9,  "dps"}, 0},
        {{11, "tank"}, 1}, {{11, "heal"}, 2}, {{11, "dps"}, 3},
    };
    auto it = table.find({classId, role});
    if (it == table.end())
        LOG_WARN("playerbots", "RaidBrowserAutoFill: no spec mapping for class {} role '{}'", (uint32)classId, role);
    return it != table.end() ? it->second : -1;
}

struct RaidBrowserPendingInvite
{
    std::vector<ObjectGuid> botGuids;
    std::vector<bool> invited;
    std::vector<uint8> classIds;         // parallel to botGuids
    std::vector<std::string> roles;      // parallel to botGuids
    time_t createdAt = 0;
    Difficulty difficulty = REGULAR_DIFFICULTY;
    bool difficultySet = false;
    uint32 dungeonId = 0;
};

struct RaidBrowserPendingTeleport
{
    ObjectGuid playerGuid{};
    std::vector<ObjectGuid> botGuids;
    uint32 dungeonId = 0;
};

static std::unordered_map<uint32, std::string> const RaidBrowserTeleportTargets = {
    {279, "IcecrownCitadelRaid"},
    {280, "IcecrownCitadelRaid"}
};

static char const* GetRaidBrowserTeleportBlockReason(Player* player)
{
    if (player == nullptr)
        return "player not found";

    if (!player->IsAlive())
        return "player is dead";

    if (player->IsInCombat())
        return "player is in combat";

    if (player->GetDirectTransport() != nullptr)
        return "player is on a transport";

    if (player->IsInFlight())
        return "player is taxi flying";

    if (player->InBattleground())
        return "player is in a battleground or arena";

    if (player->GetMap() != nullptr && player->GetMap()->IsDungeon())
        return "player is in an instance";

    if (player->IsBeingTeleported())
        return "player is already being teleported";

    return nullptr;
}

static std::mutex raidBrowserPendingMutex;
static std::unordered_map<ObjectGuid, RaidBrowserPendingInvite> raidBrowserPending;
static std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> raidBrowserAddedBots;
static std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> raidBrowserLogoutQueue;

static void AbandonIfLoading(ObjectGuid const& g, char const* why)
{
    if (PlayerbotHolder::IsBotLoading(g))
    {
        PlayerbotHolder::AbandonPendingLogin(g);
        LOG_INFO("playerbots", "RaidBrowserDiag: bot {} still loading, login abandoned ({})", g.ToString(), why);
    }
}

static void MoveRaidBrowserBotsToLogoutQueue(ObjectGuid guid, Group* group = nullptr)
{
    std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);

    // Case 1: guid is a raid-browser owner (human player): move all their bots.
    std::vector<ObjectGuid> guids;
    auto itAdded = raidBrowserAddedBots.find(guid);
    if (itAdded != raidBrowserAddedBots.end())
    {
        guids.insert(guids.end(), itAdded->second.begin(), itAdded->second.end());
        raidBrowserAddedBots.erase(itAdded);
    }
    auto itPending = raidBrowserPending.find(guid);
    if (itPending != raidBrowserPending.end())
    {
        guids.insert(guids.end(), itPending->second.botGuids.begin(), itPending->second.botGuids.end());
        raidBrowserPending.erase(itPending);
    }
    if (!guids.empty())
    {
        LOG_INFO("playerbots", "RaidBrowserDiag: owner {} left or disbanded, queued {} bots for logout",
                 guid.ToString(), guids.size());
        for (ObjectGuid const& g : guids)
            AbandonIfLoading(g, "owner left or disbanded");
        auto& q = raidBrowserLogoutQueue[guid];
        q.insert(q.end(), guids.begin(), guids.end());
        return;
    }

    // Case 2: guid is an individual bot added by raid browser; queue only that bot.
    ObjectGuid addedOwnerToErase{};
    bool eraseAddedOwner = false;

    for (auto& [ownerGuid, botGuids] : raidBrowserAddedBots)
    {
        auto botIt = std::find(botGuids.begin(), botGuids.end(), guid);
        if (botIt != botGuids.end())
        {
            botGuids.erase(botIt);
            LOG_INFO("playerbots", "RaidBrowserDiag: added bot {} removed from group, queued for logout (owner {})",
                     guid.ToString(), ownerGuid.ToString());
            AbandonIfLoading(guid, "added bot removed from group");
            auto& q = raidBrowserLogoutQueue[ownerGuid];
            q.push_back(guid);
            if (botGuids.empty())
            {
                addedOwnerToErase = ownerGuid;
                eraseAddedOwner = true;
            }
            break;
        }
    }
    if (eraseAddedOwner)
        raidBrowserAddedBots.erase(addedOwnerToErase);

    for (auto& [ownerGuid, entry] : raidBrowserPending)
    {
        auto botIt = std::find(entry.botGuids.begin(), entry.botGuids.end(), guid);
        if (botIt != entry.botGuids.end())
        {
            if (group != nullptr)
            {
                // A pending bot leaving some OTHER (stale) group at login is not a kick from our assembly
                Player* owner = ObjectAccessor::FindPlayer(ownerGuid);
                if (owner == nullptr || owner->GetGroup() != group)
                {
                    LOG_INFO("playerbots", "RaidBrowserDiag: pending bot {} left a foreign group, kept in assembly (owner {})",
                             guid.ToString(), ownerGuid.ToString());
                    break;
                }
            }
            size_t idx = static_cast<size_t>(botIt - entry.botGuids.begin());
            entry.botGuids.erase(botIt);
            if (idx < entry.invited.size())
                entry.invited.erase(entry.invited.begin() + idx);
            if (idx < entry.classIds.size())
                entry.classIds.erase(entry.classIds.begin() + idx);
            if (idx < entry.roles.size())
                entry.roles.erase(entry.roles.begin() + idx);
            LOG_INFO("playerbots", "RaidBrowserDiag: pending bot {} removed from group, queued for logout (owner {})",
                     guid.ToString(), ownerGuid.ToString());
            AbandonIfLoading(guid, "pending bot removed from group");
            auto& q = raidBrowserLogoutQueue[ownerGuid];
            q.push_back(guid);
            break;
        }
    }
}

class PlayerbotsDatabaseScript : public DatabaseScript
{
public:
    PlayerbotsDatabaseScript() : DatabaseScript("PlayerbotsDatabaseScript") {}

    bool OnModuleDatabasesLoading() override
    {
        std::string const dbString = sConfigMgr->GetOption<std::string>("PlayerbotsDatabaseInfo", "");
        if (dbString.empty())
        {
            LOG_ERROR("server.playerbots", "Playerbots database is not specified in configuration file");
            return false;
        }

        uint8 const workerThreads = sConfigMgr->GetOption<uint8>("PlayerbotsDatabase.WorkerThreads", 1);
        uint8 const synchThreads = sConfigMgr->GetOption<uint8>("PlayerbotsDatabase.SynchThreads", 2);
        PlayerbotsDatabase.SetConnectionInfo(dbString, workerThreads, synchThreads);

        bool const updatesEnabled = sConfigMgr->GetOption<bool>("Playerbots.Updates.EnableDatabases", true);
        if (updatesEnabled && !DBUpdaterUtil::CheckExecutable())
            return false;

        uint32 error = PlayerbotsDatabase.Open();
        if (error == ER_BAD_DB_ERROR && updatesEnabled)
        {
            // Database missing: create it through the mysql CLI and connect again
            if (!ModuleDBUpdater::Create(PlayerbotsDatabase))
                return false;

            error = PlayerbotsDatabase.Open();
        }

        if (error)
        {
            LOG_ERROR("server.playerbots", "Cannot connect to the playerbots database, error {}", error);
            return false;
        }

        if (updatesEnabled)
        {
            DBUpdaterInfo const info = {
                "Playerbots",
                BuiltInConfig::GetSourceDirectory() + "/modules/mod-playerbots",
                BuiltInConfig::GetSourceDirectory() + "/modules/mod-playerbots/data/sql/playerbots/base/",
                "db_playerbot"
            };

            if (!ModuleDBUpdater::Populate(PlayerbotsDatabase, info))
            {
                LOG_ERROR("server.playerbots", "Could not populate the playerbots database, see log for details.");
                return false;
            }

            if (!ModuleDBUpdater::Update(PlayerbotsDatabase, info))
            {
                LOG_ERROR("server.playerbots", "Could not update the playerbots database, see log for details.");
                return false;
            }
        }

        if (!PlayerbotsDatabase.PrepareStatements())
        {
            LOG_ERROR("server.playerbots", "Could not prepare statements of the playerbots database, see log for details.");
            return false;
        }

        return true;
    }

    void OnModuleDatabasesKeepAlive() override { PlayerbotsDatabase.KeepAlive(); }

    void OnModuleDatabasesClosing() override { PlayerbotsDatabase.Close(); }

    void OnDatabaseWarnAboutSyncQueries(bool apply) override { PlayerbotsDatabase.WarnAboutSyncQueries(apply); }

    void OnDatabaseGetDBRevision(std::map<std::string, std::string>& revisions) override
    {
        std::string revision;
        if (QueryResult resultPlayerbot =
                PlayerbotsDatabase.Query("SELECT date FROM version_db_playerbots ORDER BY date DESC LIMIT 1"))
        {
            Field* fields = resultPlayerbot->Fetch();
            revision = fields[0].Get<std::string>();
        }

        if (revision.empty())
            revision = "Unknown Playerbots Database Revision";

        revisions["Playerbots"] = revision;
    }
};

class PlayerbotsPlayerScript : public PlayerScript
{
public:
    PlayerbotsPlayerScript() : PlayerScript("PlayerbotsPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_BEFORE_LOGOUT,
        PLAYERHOOK_ON_AFTER_UPDATE,
        PLAYERHOOK_ON_CREATURE_KILL_CREDIT,
        PLAYERHOOK_ON_BEFORE_PETITION_SIGN,
        PLAYERHOOK_ON_BEFORE_CRITERIA_PROGRESS,
        PLAYERHOOK_ON_BEFORE_ACHI_COMPLETE,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_BEFORE_TELEPORT,
        PLAYERHOOK_CAN_JOIN_LFG
    }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player->GetSession()->IsHeadless())
        {
            PlayerbotsMgr::instance().AddPlayerbotData(player, false);
            sRandomPlayerbotMgr.OnPlayerLogin(player);

            // Before modifying the following messages, please make sure it does not violate the GNU GPLv2
            // license especially if you are distributing a repack or hosting a public server
            // e.g. you can replace the URL with your own repository,
            // but it should be publicly accessible and include all modifications you've made
            if (sPlayerbotAIConfig.Enabled)
            {
                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cff00ff00This server runs with |cff00ccffmod-playerbots|r "
                    "|cffcccccchttps://github.com/mod-playerbots/mod-playerbots|r");
            }

            if (sPlayerbotAIConfig.Enabled || sPlayerbotAIConfig.RandomBotAutologin)
            {
                std::string maxAllowedBotCount = std::to_string(sRandomPlayerbotMgr.GetMaxAllowedBotCount());

                ChatHandler(player->GetSession()).SendSysMessage(
                    "|cff00ff00Playerbots:|r The server is configured with " + maxAllowedBotCount + " bots.");
            }
        }
    }

    void OnPlayerBeforeLogout(Player* player) override
    {
        {
            std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);
            ObjectGuid const ownerGuid = player->GetGUID();
            auto itP = raidBrowserPending.find(ownerGuid);
            if (itP != raidBrowserPending.end())
                for (ObjectGuid const& g : itP->second.botGuids)
                    AbandonIfLoading(g, "owner logout, pending");
            auto itA = raidBrowserAddedBots.find(ownerGuid);
            if (itA != raidBrowserAddedBots.end())
                for (ObjectGuid const& g : itA->second)
                    AbandonIfLoading(g, "owner logout, added");
            auto itQ = raidBrowserLogoutQueue.find(ownerGuid);
            if (itQ != raidBrowserLogoutQueue.end())
                for (ObjectGuid const& g : itQ->second)
                    AbandonIfLoading(g, "owner logout, queued");
            raidBrowserPending.erase(ownerGuid);
            raidBrowserAddedBots.erase(ownerGuid);
            raidBrowserLogoutQueue.erase(ownerGuid);
        }

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
        {
            PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

            if (botAI == nullptr || IsSelfBot(player))
                playerbotMgr->LogoutAllBots();
        }

        sRandomPlayerbotMgr.OnPlayerLogout(player);
    }

    void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
    {
        GuildTaskMgr::instance().CheckKillTask(player, killed);
    }

    void OnPlayerBeforePetitionSign(Player* player, ObjectGuid /*petitionGuid*/, bool& alreadySignedByAccount) override
    {
        if (!alreadySignedByAccount)
            return;

        // bots share accounts, so the same-account rule must not stop them from signing each other
        if (PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr)
            alreadySignedByAccount = false;
    }

    bool OnPlayerBeforeTeleport(Player* /*player*/, uint32 /*mapid*/, float /*x*/, float /*y*/, float /*z*/,
                                float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        return true;
    }

    bool OnPlayerCanJoinLfg(Player* player, uint8 roles, std::set<uint32>& dungeons, std::string const& /*comment*/) override
    {
        if (!sPlayerbotAIConfig.RaidBrowserAutoFill)
            return true;

        if (player == nullptr || PlayerbotsMgr::instance().GetPlayerbotAI(player) != nullptr)
            return true;

        for (uint32 dungeonId : dungeons)
        {
            lfg::LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(dungeonId);

            if (dungeon == nullptr || dungeon->type != lfg::LFG_TYPE_RAID)
                continue;

            LOG_INFO("playerbots", "Player {} (guid {}) LFG raid request: roles={}, dungeon={}, map={}, difficulty={}, minlevel={}, maxlevel={}",
                     player->GetName(), player->GetGUID().ToString(), roles, dungeon->id, dungeon->map,
                     static_cast<uint8>(dungeon->difficulty), dungeon->minlevel, dungeon->maxlevel);
            if (dungeon->id == 279 || dungeon->id == 280)
            {
                char const* blockReason = GetRaidBrowserTeleportBlockReason(player);
                if (blockReason != nullptr)
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("Raid browser assembly skipped: {}", blockReason);
                    LOG_INFO("playerbots", "RaidBrowserAutoFill: assembly refused for player {} ({}): {}",
                             player->GetName(), player->GetGUID().ToString(), blockReason);
                    return false;
                }

                if (player->GetGroup() != nullptr)
                {
                    bool hasRaidBots = false;
                    {
                        std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);
                        auto itA = raidBrowserAddedBots.find(player->GetGUID());
                        hasRaidBots = (itA != raidBrowserAddedBots.end() && !itA->second.empty()) ||
                                      raidBrowserPending.find(player->GetGUID()) != raidBrowserPending.end();
                    }
                    if (hasRaidBots)
                    {
                        ChatHandler(player->GetSession()).PSendSysMessage("Raid browser assembly skipped: you are already in a raid browser group");
                        LOG_INFO("playerbots", "RaidBrowserAutoFill: assembly refused for player {} ({}): already in a raid browser group",
                                 player->GetName(), player->GetGUID().ToString());
                        return false;
                    }
                }

                if (Group* botCheckGroup = player->GetGroup())
                {
                    uint32 foreignBots = 0;
                    for (Group::member_citerator itr = botCheckGroup->GetMemberSlots().begin();
                         itr != botCheckGroup->GetMemberSlots().end(); ++itr)
                    {
                        Player* member = ObjectAccessor::FindPlayer(itr->guid);
                        if (!member || member == player)
                            continue;
                        if (PlayerbotsMgr::instance().GetPlayerbotAI(member) != nullptr)
                            ++foreignBots;
                    }
                    if (foreignBots > 0)
                    {
                        ChatHandler(player->GetSession()).PSendSysMessage("Raid browser assembly skipped: your group already has bots, remove them first");
                        LOG_INFO("playerbots", "RaidBrowserAutoFill: assembly refused for player {} ({}): group has {} foreign bot(s)",
                                 player->GetName(), player->GetGUID().ToString(), foreignBots);
                        return false;
                    }
                }

                bool const is25 = (dungeon->id == 280);

                uint8 teamId = player->GetTeamId(true);
                bool isAlliance = (teamId == TEAM_ALLIANCE);

                struct RaidRosterSlot
                {
                    uint8 classId;
                    char const* className;
                    char const* role;
                };

                std::vector<RaidRosterSlot> roster;
                {
                    auto const& templates = sPlayerbotAIConfig.FillRaidTemplates;
                    auto tmplIt = templates.find(is25 ? "25" : "10");
                    if (tmplIt != templates.end() && !tmplIt->second.empty())
                    {
                        static std::map<uint8, char const*> const classNames = {
                            {CLASS_WARRIOR, "warrior"}, {CLASS_PALADIN, "paladin"},
                            {CLASS_HUNTER, "hunter"},   {CLASS_ROGUE, "rogue"},
                            {CLASS_PRIEST, "priest"},   {CLASS_DEATH_KNIGHT, "death knight"},
                            {CLASS_SHAMAN, "shaman"},   {CLASS_MAGE, "mage"},
                            {CLASS_WARLOCK, "warlock"}, {CLASS_DRUID, "druid"},
                        };

                        uint32 realTanks = 0, realHeals = 0, realDps = 0;
                        if (Group* existingGroup = player->GetGroup())
                        {
                            for (Group::member_citerator itr = existingGroup->GetMemberSlots().begin();
                                 itr != existingGroup->GetMemberSlots().end(); ++itr)
                            {
                                Player* member = ObjectAccessor::FindPlayer(itr->guid);
                                if (!member || member == player)
                                    continue;
                                if (PlayerbotsMgr::instance().GetPlayerbotAI(member) != nullptr)
                                    continue;
                                if (PlayerbotAI::IsTank(member, true))       ++realTanks;
                                else if (PlayerbotAI::IsHeal(member, true))  ++realHeals;
                                else                                          ++realDps;
                            }
                        }

                        if (realTanks || realHeals || realDps)
                        {
                            LOG_INFO("playerbots", "RaidBrowserAutoFill: live group tanks={}, heals={}, dps={}",
                                     realTanks, realHeals, realDps);
                        }

                        for (auto const& slot : tmplIt->second)
                        {
                            if (slot.role == "tank" && realTanks > 0) { --realTanks; continue; }
                            if (slot.role == "heal" && realHeals > 0) { --realHeals; continue; }
                            if (slot.role == "dps"  && realDps   > 0) { --realDps;   continue; }

                            uint8 cls = slot.classId;
                            auto nameIt = classNames.find(cls);
                            char const* name = nameIt != classNames.end() ? nameIt->second : "unknown";
                            roster.push_back({cls, name, slot.role.c_str()});
                        }
                    }
                    else if (is25)
                    {
                        roster.push_back({CLASS_WARRIOR, "warrior", "tank"});
                        roster.push_back({CLASS_PALADIN, "paladin", "tank"});
                        roster.push_back({CLASS_DEATH_KNIGHT, "death knight", "tank"});

                        roster.push_back({CLASS_PRIEST, "priest", "healer"});
                        roster.push_back({CLASS_PRIEST, "priest", "healer"});
                        roster.push_back({CLASS_DRUID, "druid", "healer"});
                        roster.push_back({CLASS_DRUID, "druid", "healer"});
                        roster.push_back({CLASS_SHAMAN, "shaman", "healer"});
                        roster.push_back({CLASS_PALADIN, "paladin", "healer"});

                        for (int k = 0; k < 3; ++k) roster.push_back({CLASS_MAGE, "mage", "dps"});
                        for (int k = 0; k < 3; ++k) roster.push_back({CLASS_WARLOCK, "warlock", "dps"});
                        for (int k = 0; k < 2; ++k) roster.push_back({CLASS_HUNTER, "hunter", "dps"});
                        for (int k = 0; k < 2; ++k) roster.push_back({CLASS_ROGUE, "rogue", "dps"});
                        for (int k = 0; k < 2; ++k) roster.push_back({CLASS_DEATH_KNIGHT, "death knight", "dps"});
                        roster.push_back({CLASS_WARRIOR, "warrior", "dps"});
                        roster.push_back({CLASS_PALADIN, "paladin", "dps"});
                        roster.push_back({CLASS_SHAMAN, "shaman", "dps"});
                    }
                    else
                    {
                        roster.push_back({CLASS_WARRIOR, "warrior", "tank"});
                        roster.push_back({CLASS_PALADIN, "paladin", "healer"});
                        roster.push_back({CLASS_DRUID, "druid", "healer"});
                        roster.push_back({CLASS_PRIEST, "priest", "healer"});
                        roster.push_back({CLASS_SHAMAN, "shaman", "dps"});
                        roster.push_back({CLASS_MAGE, "mage", "dps"});
                        roster.push_back({CLASS_WARLOCK, "warlock", "dps"});
                        roster.push_back({CLASS_HUNTER, "hunter", "dps"});
                        roster.push_back({CLASS_DEATH_KNIGHT, "death knight", "tank"});
                    }
                }

                {
                    // Cap the roster by the free places in the owner's group
                    Group* capGroup = player->GetGroup();
                    uint32 const raidSize = is25 ? 25 : 10;
                    uint32 const occupied = capGroup ? capGroup->GetMembersCount() : 1;
                    uint32 const maxBots = raidSize > occupied ? raidSize - occupied : 0;
                    if (maxBots == 0)
                    {
                        ChatHandler(player->GetSession()).PSendSysMessage("Raid browser assembly skipped: your group is already full");
                        LOG_INFO("playerbots", "RaidBrowserAutoFill: assembly refused for player {}: group already full ({}/{})",
                                 player->GetName(), occupied, raidSize);
                        return false;
                    }
                    if (roster.size() > maxBots)
                    {
                        LOG_INFO("playerbots", "RaidBrowserAutoFill: roster trimmed {} -> {} (group occupies {}/{})",
                                 roster.size(), maxBots, occupied, raidSize);
                        roster.erase(roster.begin() + maxBots, roster.end());
                    }
                }

                std::unordered_set<ObjectGuid> usedGuids;
                std::vector<ObjectGuid> pickedBots;
                std::vector<uint8> pickedClassIds;
                std::vector<std::string> pickedRoles;

                // Bots already reserved by any pending assembly (including ones that failed to start loading)
                std::vector<ObjectGuid> busyGuids;
                {
                    std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);
                    for (auto const& [ownerGuid, pendingEntry] : raidBrowserPending)
                        busyGuids.insert(busyGuids.end(), pendingEntry.botGuids.begin(), pendingEntry.botGuids.end());
                }

                auto isEligible = [&](ObjectGuid const& guid) -> bool
                {
                    if (usedGuids.find(guid) != usedGuids.end())
                        return false;
                    if (std::find(busyGuids.begin(), busyGuids.end(), guid) != busyGuids.end())
                        return false;
                    if (PlayerbotHolder::IsBotLoading(guid))
                        return false;
                    if (ObjectAccessor::FindConnectedPlayer(guid) != nullptr)
                        return false;
                    uint32 guildId = sCharacterCache->GetCharacterGuildIdByGuid(guid);
                    if (guildId && PlayerbotGuildMgr::instance().IsRealGuild(guildId))
                        return false;
                    return true;
                };

                std::map<uint8, uint32> requiredCount;
                for (auto const& slot : roster)
                    requiredCount[slot.classId]++;

                for (auto const& [classId, count] : requiredCount)
                {
                    char const* name = "unknown";
                    for (auto const& slot : roster)
                    {
                        if (slot.classId == classId)
                        {
                            name = slot.className;
                            break;
                        }
                    }

                    auto cacheIt = sRandomPlayerbotMgr.addclassCache.find(RandomPlayerbotMgr::GetTeamClassIdx(isAlliance, classId));
                    if (cacheIt == sRandomPlayerbotMgr.addclassCache.end())
                    {
                        if (count > 0)
                            LOG_WARN("playerbots", "RaidBrowserAutoFill: too few candidates for class {} (required {}, available {})", name, count, 0);
                        continue;
                    }

                    uint32 available = 0;
                    for (ObjectGuid const& guid : cacheIt->second)
                    {
                        if (isEligible(guid))
                            ++available;
                    }

                    if (available < count)
                        LOG_WARN("playerbots", "RaidBrowserAutoFill: too few candidates for class {} (required {}, available {})", name, count, available);
                }

                for (auto const& slot : roster)
                {
                    auto cacheIt = sRandomPlayerbotMgr.addclassCache.find(RandomPlayerbotMgr::GetTeamClassIdx(isAlliance, slot.classId));
                    if (cacheIt == sRandomPlayerbotMgr.addclassCache.end())
                        continue;

                    ObjectGuid picked = ObjectGuid::Empty;
                    std::vector<ObjectGuid> candidates;
                    candidates.reserve(cacheIt->second.size());
                    for (ObjectGuid const& guid : cacheIt->second)
                    {
                        if (isEligible(guid))
                            candidates.push_back(guid);
                    }
                    if (!candidates.empty())
                    {
                        static thread_local std::mt19937 rng(std::random_device{}());
                        std::shuffle(candidates.begin(), candidates.end(), rng);
                        picked = candidates.front();
                    }

                    if (picked.IsEmpty())
                        continue;

                    usedGuids.insert(picked);
                    pickedBots.push_back(picked);
                    pickedClassIds.push_back(slot.classId);
                    pickedRoles.push_back(slot.role);
                    LOG_INFO("playerbots", "RaidBrowserAutoFill: class={}, guid={}, role={}", slot.className, picked.ToString(), slot.role);
                }

                Group* fillGroup = player->GetGroup();
                bool const canFillGroup = (fillGroup == nullptr) ||
                    (fillGroup->GetLeaderGUID() == player->GetGUID() && !fillGroup->isLFGGroup() &&
                     !fillGroup->isBGGroup() && !fillGroup->isBFGroup());
                if (!canFillGroup)
                {
                    LOG_INFO("playerbots", "RaidBrowserAutoFill: skipping auto-add, player is in a group and not its leader (or LFG/BG group)");
                }
                else
                {
                    bool alreadyPending = false;
                    {
                        std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);
                        auto pendingIt = raidBrowserPending.find(player->GetGUID());
                        if (pendingIt != raidBrowserPending.end() &&
                            (time(nullptr) - pendingIt->second.createdAt) <= 120)
                        {
                            alreadyPending = true;
                        }
                    }

                    if (alreadyPending)
                    {
                        LOG_INFO("playerbots", "RaidBrowserAutoFill: player already has pending invites");
                    }
                    else
                    {
                        PlayerbotMgr* playerMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
                        if (playerMgr == nullptr)
                        {
                            LOG_WARN("playerbots", "RaidBrowserAutoFill: no PlayerbotMgr for player {}", player->GetName());
                        }
                        else
                        {
                            std::vector<ObjectGuid> toAdd;
                            for (ObjectGuid const& guid : pickedBots)
                                toAdd.push_back(guid);

                            uint32 masterAccountId = player->GetSession() ? player->GetSession()->GetAccountId() : 0;
                            for (ObjectGuid const& guid : toAdd)
                                playerMgr->AddPlayerBot(guid, masterAccountId);

                            if (!toAdd.empty())
                            {
                                RaidBrowserPendingInvite entry;
                                entry.botGuids = toAdd;
                                entry.invited.assign(toAdd.size(), false);
                                entry.classIds = pickedClassIds;
                                entry.roles = pickedRoles;
                                entry.createdAt = time(nullptr);
                                entry.difficulty = dungeon->difficulty;
                                entry.difficultySet = false;
                                entry.dungeonId = dungeon->id;
                                std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);
                                raidBrowserPending[player->GetGUID()] = std::move(entry);
                                LOG_INFO("playerbots", "RaidBrowserAutoFill: queued {} bots for player {}", toAdd.size(), player->GetName());
                            }
                        }
                    }
                }
            }

        }

        return true;
    }

    void OnPlayerAfterUpdate(Player* player, uint32 diff) override
    {
        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
        {
            botAI->UpdateAI(diff);
        }

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
        {
            playerbotMgr->UpdateAI(diff);
        }
    }

    using PlayerScript::OnPlayerCanUseChat;

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || lang == LANG_ADDON)
        {
            return true;
        }

        PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(receiver);

        if (botAI == nullptr)
        {
            return true;
        }

        botAI->HandleCommand(type, msg, player);

        if (msg == "logout")
            return false;

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override
    {
        if (lang == LANG_ADDON)
            return true;

        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* const member = itr->GetSource();

            if (member == nullptr)
                continue;

            PlayerbotAI* const botAI = PlayerbotsMgr::instance().GetPlayerbotAI(member);

            if (botAI == nullptr)
                continue;

            botAI->HandleCommand(type, msg, player);
        }

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Guild* /*guild*/) override
    {
        if (type != CHAT_MSG_GUILD || lang == LANG_ADDON)
            return true;

        PlayerbotMgr* playerbotMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);

        if (playerbotMgr == nullptr)
            return true;

        for (PlayerBotMap::const_iterator it = playerbotMgr->GetPlayerBotsBegin(); it != playerbotMgr->GetPlayerBotsEnd(); ++it)
        {
            Player* const bot = it->second;

            if (bot == nullptr)
                continue;

            if (bot->GetGuildId() != player->GetGuildId())
                continue;

            PlayerbotsMgr::instance().GetPlayerbotAI(bot)->HandleCommand(type, msg, player);
        }

        return true;
    }

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& msg, Channel* channel) override
    {
        PlayerbotMgr* const playerbotMgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);

        if (playerbotMgr != nullptr && channel->GetFlags() & 0x18)
            playerbotMgr->HandleCommand(type, msg);

        sRandomPlayerbotMgr.HandleCommand(type, msg, player);

        return true;
    }

    bool OnPlayerBeforeAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if ((sRandomPlayerbotMgr.IsRandomBot(player) || sRandomPlayerbotMgr.IsAddclassBot(player)) &&
            (achievement->flags & (ACHIEVEMENT_FLAG_REALM_FIRST_REACH | ACHIEVEMENT_FLAG_REALM_FIRST_KILL)))
        {
            return false;
        }

        return true;
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (sPlayerbotAIConfig.RandomBotXPRate == 1.0 || !player)
            return;

        if (!player->GetSession()->IsHeadless() || !sRandomPlayerbotMgr.IsRandomBot(player))
            return;

        if (Group* group = player->GetGroup())
        {
            for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
            {
                Player* member = gref->GetSource();
                if (!member)
                    continue;

                if (!member->GetSession()->IsHeadless())
                    return;
            }
        }

        amount = static_cast<uint32>(std::round(static_cast<float>(amount) * sPlayerbotAIConfig.RandomBotXPRate));
    }
};

class PlayerbotsMiscScript : public MiscScript
{
public:
    PlayerbotsMiscScript() : MiscScript("PlayerbotsMiscScript", {MISCHOOK_ON_DESTRUCT_PLAYER}) {}

    void OnDestructPlayer(Player* player) override
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
            delete botAI;

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
            delete playerbotMgr;
    }
};

class PlayerbotsServerScript : public ServerScript
{
public:
    PlayerbotsServerScript() : ServerScript("PlayerbotsServerScript", {
        SERVERHOOK_ON_PACKET_SENT,
        SERVERHOOK_CAN_PACKET_RECEIVE
    }) {}

    void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
    {
        Player* player = session->GetPlayer();

        if (player == nullptr)
            return;

        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI != nullptr)
            botAI->HandleBotOutgoingPacket(packet);

        if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
            playerbotMgr->HandleMasterOutgoingPacket(packet);
    }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        if (Player* player = session->GetPlayer())
            if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
                playerbotMgr->HandleMasterIncomingPacket(packet);

        return true;
    }
};

class PlayerbotsSessionScript : public SessionScript
{
public:
    PlayerbotsSessionScript() : SessionScript("PlayerbotsSessionScript", {
        SESSIONHOOK_ON_UPDATE
    }) {}

    void OnSessionUpdate(WorldSession* session, uint32 /*diff*/) override
    {
        if (Player* player = session->GetPlayer())
            if (PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player))
                playerbotMgr->UpdateSessions();
    }
};

class PlayerbotsAllMapScript : public AllMapScript
{
public:
    PlayerbotsAllMapScript() : AllMapScript("PlayerbotsAllMapScript", {
        ALLMAPHOOK_CAN_SEND_OBJECT_UPDATES_TO_PLAYER
    }) {}

    bool CanSendObjectUpdatesToPlayer(Map* /*map*/, Player* player) override
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(player);

        if (botAI == nullptr)
            return true;

        return IsSelfBot(player);
    }
};

class PlayerbotsGlobalScript : public GlobalScript
{
public:
    PlayerbotsGlobalScript() : GlobalScript("PlayerbotsGlobalScript", {
        GLOBALHOOK_CAN_CREATE_LFG_PROPOSAL
    }) {}

    bool CanCreateLfgProposal(lfg::Lfg5Guids const& guids) override
    {
        for (ObjectGuid const& guid : guids.guids)
        {
            Player* player = ObjectAccessor::FindPlayer(guid);

            if (guid.IsGroup() || IsRealPlayer(player) || IsSelfBot(player))
                return true;
        }

        return false;
    }
};

class PlayerbotsWorldScript : public WorldScript
{
public:
    PlayerbotsWorldScript() : WorldScript("PlayerbotsWorldScript", {
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED,
        WORLDHOOK_ON_UPDATE,
        WORLDHOOK_ON_SHUTDOWN
    }) {}

    void OnBeforeWorldInitialized() override
    {
        LOG_INFO("server.loading", "╔══════════════════════════════════════════════════════════╗");
        LOG_INFO("server.loading", "║                                                          ║");
        LOG_INFO("server.loading", "║              AzerothCore Playerbots Module               ║");
        LOG_INFO("server.loading", "║                                                          ║");
        LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
        LOG_INFO("server.loading", "║     mod-playerbots is a community-driven open-source     ║");
        LOG_INFO("server.loading", "║  project based on AzerothCore, licensed under GNU GPLv2  ║");
        LOG_INFO("server.loading", "╟──────────────────────────────────────────────────────────╢");
        LOG_INFO("server.loading", "║     https://github.com/mod-playerbots/mod-playerbots     ║");
        LOG_INFO("server.loading", "╚══════════════════════════════════════════════════════════╝");

        uint32 oldMSTime = getMSTime();

        LOG_INFO("server.loading", " ");
        LOG_INFO("server.loading", "Load Playerbots Config...");

        sPlayerbotAIConfig.Initialize();

        LOG_INFO("server.loading", ">> Loaded playerbots config in {} ms", GetMSTimeDiffToNow(oldMSTime));
        LOG_INFO("server.loading", " ");

        PlayerbotSpellRepository::Instance().Initialize();
        CheckMountStateAction::LoadPreferredMounts();

        LOG_INFO("server.loading", "Loading gather node index...");
        GatherNodeMgr::instance().Load();

        LOG_INFO("server.loading", "Playerbots World Thread Processor initialized");
    }

    void OnUpdate(uint32 diff) override
    {
        PlayerbotHolder::UpdatePendingLogins();
        sRandomPlayerbotMgr.UpdateSessions();
        PlayerbotWorldThreadProcessor::instance().Update(diff);
        sRandomPlayerbotMgr.UpdateAI(diff);

        // 9c.2: raid-browser bots that left the raid zone are reset and returned to the gathering point
        {
            static uint32 raidZoneTimer = 0;
            raidZoneTimer += diff;
            if (raidZoneTimer >= 1000)
            {
                raidZoneTimer = 0;
                std::vector<ObjectGuid> zoneBots;
                {
                    std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);
                    for (auto const& entry : raidBrowserAddedBots)
                        zoneBots.insert(zoneBots.end(), entry.second.begin(), entry.second.end());
                }
                for (ObjectGuid const& zoneBotGuid : zoneBots)
                {
                    Player* zoneBot = ObjectAccessor::FindPlayer(zoneBotGuid);
                    if (!zoneBot || !zoneBot->IsInWorld() || zoneBot->IsBeingTeleported())
                        continue;
                    PlayerbotAI* zoneAI = GET_PLAYERBOT_AI(zoneBot);
                    if (!zoneAI || !zoneAI->IsRaidBrowserBot())
                        continue;
                    if (PlayerbotAI::IsInRaidBrowserZone(zoneBot))
                    {
                        zoneAI->SetRaidZoneArmed(true);
                        continue;
                    }
                    if (!zoneAI->IsRaidZoneArmed())
                        continue;
                    zoneAI->SetRaidZoneArmed(false);
                    zoneBot->GetMotionMaster()->Clear(true);
                    zoneBot->StopMoving();
                    zoneAI->ChangeStrategy("+stay,-follow,-passive,-move from group", BOT_STATE_NON_COMBAT);
                    zoneAI->ChangeStrategy("+stay,-follow,-passive,-move from group", BOT_STATE_COMBAT);
                    zoneBot->TeleportTo(571, 5873.82f, 2110.98f, 636.011f, 3.5523f);
                    {
                        Player* zoneOwner = ObjectAccessor::FindPlayer(zoneAI->GetRaidBrowserOwner());
                        LOG_DEBUG("playerbots",
                                 "9c.2: raid bot {} left raid zone: map={} x={:.1f} y={:.1f} z={:.1f} dist={:.1f} "
                                 "alive={} ownerMap={}",
                                 zoneBot->GetName(), zoneBot->GetMapId(), zoneBot->GetPositionX(),
                                 zoneBot->GetPositionY(), zoneBot->GetPositionZ(),
                                 zoneBot->GetExactDist2d(5873.82f, 2110.98f), zoneBot->IsAlive(),
                                 zoneOwner ? zoneOwner->GetMapId() : 0u);
                    }
                }
            }
        }

        static uint32 raidBrowserPendingTimer = 0;
        raidBrowserPendingTimer += diff;
        if (raidBrowserPendingTimer < 1000)
            return;
        raidBrowserPendingTimer = 0;

        time_t now = time(nullptr);
        std::vector<ObjectGuid> toErase;
        std::vector<RaidBrowserPendingTeleport> raidBrowserTeleports;
        {
            std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);
            uint32 invitesThisTick = 0;
            for (auto& [playerGuid, entry] : raidBrowserPending)
            {
                Player* player = ObjectAccessor::FindPlayer(playerGuid);
                if (!player)
                {
                    // Owner connected but not in the world (map change / teleport): wait, keep the assembly
                    if (ObjectAccessor::FindConnectedPlayer(playerGuid) != nullptr && now - entry.createdAt <= 120)
                        continue;
                    for (ObjectGuid const& g : entry.botGuids)
                        AbandonIfLoading(g, "owner not in world");
                    toErase.push_back(playerGuid);
                    continue;
                }

                if (now - entry.createdAt > 120)
                {
                    LOG_WARN("playerbots", "RaidBrowserAutoFill: pending invites for player {} expired",
                             player->GetName());
                    // The assembly failed: do not leave its bots in the world.
                    auto& logoutQ = raidBrowserLogoutQueue[playerGuid];
                    for (ObjectGuid& g : entry.botGuids)
                    {
                        if (PlayerbotHolder::IsBotLoading(g))
                            PlayerbotHolder::AbandonPendingLogin(g);
                        logoutQ.push_back(g);
                    }
                    entry.botGuids.clear();
                    toErase.push_back(playerGuid);
                    continue;
                }

                // Soft deadline: a bot that never entered the world (login refused, bot cap,
                // failed load) must not block the whole assembly.
                if (now - entry.createdAt > 60)
                {
                    uint32 dropped = 0;
                    Group* softDeadlineGroup = player->GetGroup();
                    for (size_t i = entry.botGuids.size(); i-- > 0;)
                    {
                        bool const inGroup = (softDeadlineGroup != nullptr && softDeadlineGroup->IsMember(entry.botGuids[i]));
                        if (inGroup)
                            continue;

                        LOG_WARN("playerbots", "RaidBrowserAutoFill: bot {} did not join in 60s (invited={}, inWorld={}), dropped from assembly for player {}",
                                 entry.botGuids[i].ToString(), static_cast<bool>(entry.invited[i]),
                                 ObjectAccessor::FindPlayer(entry.botGuids[i]) != nullptr, player->GetName());
                        if (PlayerbotHolder::IsBotLoading(entry.botGuids[i]))
                            PlayerbotHolder::AbandonPendingLogin(entry.botGuids[i]);
                        // Dropped bot may already be in the world: do not leave it orphaned
                        raidBrowserLogoutQueue[playerGuid].push_back(entry.botGuids[i]);
                        entry.botGuids.erase(entry.botGuids.begin() + i);
                        entry.invited.erase(entry.invited.begin() + i);
                        if (i < entry.classIds.size())
                            entry.classIds.erase(entry.classIds.begin() + i);
                        if (i < entry.roles.size())
                            entry.roles.erase(entry.roles.begin() + i);
                        ++dropped;
                    }

                    if (dropped > 0 && player->GetSession() != nullptr)
                        ChatHandler(player->GetSession()).PSendSysMessage("Raid browser: {} bot(s) could not join, continuing with the rest", dropped);

                    if (entry.botGuids.empty())
                    {
                        LOG_WARN("playerbots", "RaidBrowserAutoFill: no bots left for player {}, assembly cancelled",
                                 player->GetName());
                        toErase.push_back(playerGuid);
                        continue;
                    }
                }

                for (size_t i = 0; i < entry.botGuids.size(); ++i)
                {
                    if (entry.invited[i])
                        continue;

                    Player* bot = ObjectAccessor::FindPlayer(entry.botGuids[i]);
                    if (!bot)
                        continue;

                    if (invitesThisTick >= 5)
                        break;

                    // Apply full prep: skills, spells, gear, gems, unbind, attunements + our spec
                    if (i < entry.classIds.size() && i < entry.roles.size())
                    {
                        uint8 classId = entry.classIds[i];
                        std::string const& role = entry.roles[i];
                        int specNo = SpecNoForClassRole(classId, role);
                        if (specNo >= 0)
                        {
                            PlayerbotFactory factory(bot, bot->GetLevel());

                            // 1) Full cycle: skills, spells, gear, gems, unbind, attunements
                            factory.Randomize(false);

                            // 2) Our spec OVER the random one (Randomize sets a random spec)
                            PlayerbotFactory::InitTalentsBySpecNo(bot, specNo, true);

                            // 3) Re-gear for OUR spec (Randomize geared for the random spec)
                            factory.InitEquipment(false);
                            factory.ApplyEnchantAndGemsNew(true);

                            // 4) Consumables (food, flasks, potions)
                            factory.InitConsumables();

                            LOG_INFO("playerbots", "RaidBrowserAutoFill: applied full prep (spec {} + gear + gems + skills + unbind) to bot {} (class={}, role={})",
                                     specNo, entry.botGuids[i].ToString(), (uint32)classId, role);
                        }
                    }

                    LOG_INFO("playerbots", "RaidBrowserAutoFill: bot {} entered world for player {}",
                             entry.botGuids[i].ToString(), player->GetName());

                    auto inviteOp = std::make_unique<GroupInviteOperation>(playerGuid, entry.botGuids[i]);
                    PlayerbotWorldThreadProcessor::instance().QueueOperation(std::move(inviteOp));
                    LOG_INFO("playerbots", "RaidBrowserAutoFill: invited bot {}", entry.botGuids[i].ToString());
                    entry.invited[i] = true;
                    ++invitesThisTick;
                }

                bool allInvited = true;
                for (bool v : entry.invited)
                {
                    if (!v)
                    {
                        allInvited = false;
                        break;
                    }
                }

                if (allInvited)
                {
                    Group* group = player->GetGroup();
                    bool allMembers = (group != nullptr);
                    if (allMembers)
                    {
                        for (ObjectGuid const& botGuid : entry.botGuids)
                        {
                            if (!group->IsMember(botGuid))
                            {
                                allMembers = false;
                                break;
                            }
                        }
                    }

                    if (allMembers && group)
                    {
                        if (!group->isRaidGroup() && group->GetMembersCount() >= 5)
                            group->ConvertToRaid();

                        if (!entry.difficultySet)
                        {
                            group->SetRaidDifficulty(entry.difficulty);
                            LOG_INFO("playerbots", "RaidBrowserAutoFill: set raid difficulty {} for player {}",
                                     static_cast<uint8>(entry.difficulty), player->GetName());
                            entry.difficultySet = true;
                        }

                        auto& added = raidBrowserAddedBots[playerGuid];
                        RaidBrowserPendingTeleport teleportEntry;
                        teleportEntry.playerGuid = playerGuid;
                        teleportEntry.botGuids = entry.botGuids;
                        teleportEntry.dungeonId = entry.dungeonId;
                        raidBrowserTeleports.push_back(std::move(teleportEntry));

                        for (ObjectGuid& g : entry.botGuids)
                            added.push_back(std::move(g));
                        entry.botGuids.clear();

                        toErase.push_back(playerGuid);
                    }
                }
            }

            for (ObjectGuid const& guid : toErase)
                raidBrowserPending.erase(guid);
        }

        if (sPlayerbotAIConfig.RaidBrowserAutoFill)
        {
            std::unordered_map<ObjectGuid, std::vector<ObjectGuid>> queueCopy;
            {
                std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);
                queueCopy = std::move(raidBrowserLogoutQueue);
                raidBrowserLogoutQueue.clear();
            }

            for (auto& [playerGuid, botGuids] : queueCopy)
            {
                Player* player = ObjectAccessor::FindPlayer(playerGuid);
                if (player == nullptr)
                {
                    // Owner between maps: keep the bots queued for the next tick
                    if (ObjectAccessor::FindConnectedPlayer(playerGuid) != nullptr)
                    {
                        std::lock_guard<std::mutex> lock(raidBrowserPendingMutex);
                        auto& q = raidBrowserLogoutQueue[playerGuid];
                        q.insert(q.end(), botGuids.begin(), botGuids.end());
                    }
                    continue;
                }

                PlayerbotMgr* playerbotMgr = GET_PLAYERBOT_MGR(player);
                if (playerbotMgr == nullptr)
                    continue;

                LOG_INFO("playerbots", "RaidBrowserAutoFill: cleanup, logging out {} bots for player {}",
                         botGuids.size(), player->GetName());
                for (ObjectGuid const& botGuid : botGuids)
                    playerbotMgr->LogoutPlayerBot(botGuid);
            }
        }

        for (RaidBrowserPendingTeleport const& pendingTeleport : raidBrowserTeleports)
        {
            auto targetIt = RaidBrowserTeleportTargets.find(pendingTeleport.dungeonId);
            if (targetIt == RaidBrowserTeleportTargets.end())
                continue;

            GameTele const* tele = sObjectMgr->GetGameTele(targetIt->second, true);
            if (tele == nullptr)
            {
                LOG_WARN("playerbots", "RaidBrowserAutoFill: game_tele '{}' not found", targetIt->second);
                continue;
            }

            Player* player = ObjectAccessor::FindPlayer(pendingTeleport.playerGuid);
            char const* reason = GetRaidBrowserTeleportBlockReason(player);
            if (reason != nullptr)
            {
                if (player != nullptr && player->GetSession() != nullptr)
                    ChatHandler(player->GetSession()).PSendSysMessage("Raid browser teleport skipped: {}", reason);
                LOG_INFO("playerbots", "RaidBrowserAutoFill: teleport skipped for player {} ({}): {}",
                         player != nullptr ? player->GetName() : "?",
                         pendingTeleport.playerGuid.ToString(), reason);
                continue;
            }

            player->TeleportTo(tele->mapId, tele->position_x, tele->position_y, tele->position_z,
                              tele->orientation);

            uint32 teleportedBots = 0;
            for (ObjectGuid const& botGuid : pendingTeleport.botGuids)
            {
                Player* bot = ObjectAccessor::FindPlayer(botGuid);
                if (bot == nullptr || !bot->IsInWorld() || bot->IsBeingTeleported())
                {
                    LOG_INFO("playerbots", "RaidBrowserAutoFill: bot {} skipped for teleport", botGuid.ToString());
                    continue;
                }

                char const* botReason = GetRaidBrowserTeleportBlockReason(bot);
                if (botReason != nullptr)
                {
                    LOG_INFO("playerbots", "RaidBrowserAutoFill: bot {} skipped for teleport: {}",
                             botGuid.ToString(), botReason);
                    continue;
                }

                bot->GetMotionMaster()->Clear(true);
                bot->StopMoving();
                bot->TeleportTo(tele->mapId, tele->position_x, tele->position_y, tele->position_z,
                                tele->orientation);
                ++teleportedBots;
            }

            LOG_INFO("playerbots", "RaidBrowserAutoFill: teleported player {} ({}) and {} bots to {} (map {})",
                     player->GetName(), pendingTeleport.playerGuid.ToString(), teleportedBots,
                     targetIt->second, tele->mapId);
        }
    }

    void OnShutdown() override
    {
        LOG_INFO("playerbots", "Logging out all bots...");
        sRandomPlayerbotMgr.LogoutAllBots();
        PlayerbotHolder::ClearPendingLogins();
    }
};

class PlayerBotsBGScript : public BGScript
{
public:
    PlayerBotsBGScript() : BGScript("PlayerBotsBGScript") {}

    void OnBattlegroundStart(Battleground* bg) override
    {
        BGStrategyData data;

        BattlegroundTypeId bgType = bg->GetBgTypeID();
        if (bgType == BATTLEGROUND_RB)
            bgType = bg->GetBgTypeID(true);

        switch (bgType)
        {
            case BATTLEGROUND_WS:
                data.allianceStrategy = urand(0, WS_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, WS_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_AB:
                data.allianceStrategy = urand(0, AB_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, AB_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_AV:
                data.allianceStrategy = urand(0, AV_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, AV_STRATEGY_MAX - 1);
                break;
            case BATTLEGROUND_EY:
                data.allianceStrategy = urand(0, EY_STRATEGY_MAX - 1);
                data.hordeStrategy = urand(0, EY_STRATEGY_MAX - 1);
                break;
            default:
                break;
        }

        bgStrategies[bg->GetInstanceID()] = data;
    }

    void OnBattlegroundEnd(Battleground* bg, TeamId /*winnerTeam*/) override { bgStrategies.erase(bg->GetInstanceID()); }
};

class PlayerbotsBattlefieldScript : public BattlefieldScript
{
public:
    PlayerbotsBattlefieldScript() : BattlefieldScript("PlayerbotsBattlefieldScript") { }
};

class PlayerbotsGroupScript : public GroupScript
{
public:
    PlayerbotsGroupScript() : GroupScript("PlayerbotsGroupScript",
        { GROUPHOOK_ON_REMOVE_MEMBER, GROUPHOOK_ON_DISBAND }) {}

    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod method, ObjectGuid kicker, char const* reason) override
    {
        LOG_DEBUG("playerbots", "RaidBrowserDiag: OnRemoveMember guid {} method {} kicker {} reason '{}'",
                  guid.ToString(), static_cast<uint32>(method), kicker.ToString(), reason ? reason : "");
        MoveRaidBrowserBotsToLogoutQueue(guid, group);
    }

    void OnDisband(Group* group) override
    {
        if (group == nullptr)
            return;

        Group::MemberSlotList const& slots = group->GetMemberSlots();
        for (Group::member_citerator itr = slots.begin(); itr != slots.end(); ++itr)
            MoveRaidBrowserBotsToLogoutQueue(itr->guid, group);
    }
};

void AddPlayerbotsSecureLoginScripts();
void AddPlayerbotsSelfBotAfkScripts();
void AddSC_MagtheridonBotScripts();
void AddSC_SerpentshrineCavernBotScripts();
void AddSC_TempestKeepBotScripts();
void AddSC_HyjalBotScripts();
void AddSC_IcecrownBotScripts();
void AddSC_RubySanctumBotScripts();
void AddSC_randombot_level_mgr();

void AddPlayerbotsScripts()
{
    new PlayerbotsBattlefieldScript();
    new PlayerbotsDatabaseScript();
    new PlayerbotsPlayerScript();
    new PlayerbotsMiscScript();
    new PlayerbotsServerScript();
    new PlayerbotsSessionScript();
    new PlayerbotsAllMapScript();
    new PlayerbotsGlobalScript();
    new PlayerbotsWorldScript();
    new PlayerbotsGroupScript();
    new PlayerBotsBGScript();
    AddPlayerbotsSecureLoginScripts();
    AddPlayerbotsSelfBotAfkScripts();
    AddPlayerbotsCommandscripts();
    PlayerBotsGuildValidationScript();
    AddSC_MagtheridonBotScripts();
    AddSC_SerpentshrineCavernBotScripts();
    AddSC_TempestKeepBotScripts();
    AddSC_HyjalBotScripts();
    AddSC_IcecrownBotScripts();
    AddSC_RubySanctumBotScripts();
    AddSC_randombot_level_mgr();
}