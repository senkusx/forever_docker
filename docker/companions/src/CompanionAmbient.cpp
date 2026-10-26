#include "CompanionAmbient.h"
#include "CellImpl.h"
#include "CharacterCache.h"
#include "CompanionAI.h"
#include "CompanionConfig.h"
#include "CompanionFactory.h"
#include "CompanionMgr.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "GridDefines.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "StringFormat.h"
#include "RealmList.h"
#include "World.h"
#include "WorldSession.h"
#include <cmath>

namespace Companions
{
    namespace
    {
        // the player factions of each side (human, orc): what is hostile or friendly to that side's bots
        FactionTemplateEntry const* SideFaction(Team team)
        {
            return sFactionTemplateStore.LookupEntry(team == ALLIANCE ? 1 : 2);
        }

        std::list<Creature*> CreaturesAround(Player* player, float range)
        {
            std::list<Creature*> creatures;
            Trinity::AllWorldObjectsInRange check(player, range);
            Trinity::CreatureListSearcher<Trinity::AllWorldObjectsInRange> searcher(player, creatures, check);
            Cell::VisitGridObjects(player, searcher, range);
            return creatures;
        }

        // standable ground near a point: on the floor, dry, not much higher or lower than the point
        bool GroundNear(Player* player, Position const& around, float minDist, float maxDist, Position& out)
        {
            Map* map = player->GetMap();
            for (uint32 attempt = 0; attempt < 8; ++attempt)
            {
                float angle = frand(0.0f, 2.0f * float(M_PI));
                float dist = frand(minDist, maxDist);
                float x = around.GetPositionX() + std::cos(angle) * dist;
                float y = around.GetPositionY() + std::sin(angle) * dist;
                if (!Trinity::IsValidMapCoord(x, y))
                    continue;
                float z = map->GetHeight(player->GetPhaseShift(), x, y, around.GetPositionZ() + 5.0f, true, 20.0f);
                if (z <= INVALID_HEIGHT || std::fabs(z - around.GetPositionZ()) > 8.0f)
                    continue;
                ZLiquidStatus liquid = map->GetLiquidStatus(player->GetPhaseShift(), x, y, z, map_liquidHeaderTypeFlags::AllLiquids);
                if (liquid & MAP_LIQUID_STATUS_IN_CONTACT)
                    continue;
                out.Relocate(x, y, z + 0.5f, frand(0.0f, 2.0f * float(M_PI)));
                return true;
            }
            return false;
        }

        uint8 MaxLevel()
        {
            return uint8(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
        }
    }

    CompanionAmbient* CompanionAmbient::instance()
    {
        static CompanionAmbient instance;
        return &instance;
    }

    void CompanionAmbient::Initialize()
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `companion_ambient` ("
            "`guid` BIGINT UNSIGNED NOT NULL,"
            "`created` INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Companions: characters of the ambient bots'");
        if (QueryResult result = CharacterDatabase.Query("SELECT guid FROM companion_ambient"))
        {
            do
            {
                ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(result->Fetch()[0].GetUInt64());
                CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);
                if (!cache || cache->IsDeleted || sCompanionMgr->GetRecord(guid))
                    CharacterDatabase.PExecute("DELETE FROM companion_ambient WHERE guid = {}", guid.GetCounter());
                else
                    _pool.insert(guid);
            } while (result->NextRow());
        }
        TC_LOG_INFO("server.worldserver", "Companions: {} ambient characters in the pool", _pool.size());
    }

    void CompanionAmbient::Forget(ObjectGuid guid)
    {
        _pool.erase(guid);
        _live.erase(guid);
        CharacterDatabase.PExecute("DELETE FROM companion_ambient WHERE guid = {}", guid.GetCounter());
    }

    CompanionAmbient::ZoneKind CompanionAmbient::Classify(Player* player)
    {
        ZoneKind kind;
        Map* map = player->GetMap();
        if (!map || map->Instanceable() || !player->IsAlive() || player->IsInFlight() || player->GetTransport())
            return kind;
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player->GetZoneId());
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId());
        if (!zone)
            return kind;
        kind.Allowed = true;
        kind.City = zone->GetFlags().HasFlag(AreaFlags::LinkedChat) || (area && area->GetFlags().HasFlag(AreaFlags::LinkedChat));
        // the zone's owners; contested zones (both, none, or flagged contested) get both factions
        uint8 mask = zone->FactionGroupMask;
        bool contested = zone->GetFlags().HasFlag(AreaFlags::Contested) || (mask & AREATEAM_ANY) == 0 || (mask & AREATEAM_ANY) == AREATEAM_ANY;
        kind.Alliance = contested || (mask & AREATEAM_ALLY);
        kind.Horde = contested || (mask & AREATEAM_HORDE);
        return kind;
    }

    std::string CompanionAmbient::Describe(Player* player) const
    {
        ZoneKind kind = Classify(player);
        std::string out = !kind.Allowed ? "no ambient bots here" : (kind.City ? "city" : "open world");
        if (kind.Allowed)
            out += std::string(", ") + (kind.Alliance && kind.Horde ? "both factions" : (kind.Alliance ? "Alliance" : "Horde"));
        out += ", " + std::to_string(CountNear(player)) + " around, " + std::to_string(_live.size()) + " on the server, pool "
            + std::to_string(_pool.size());
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player->GetZoneId()))
            out += Trinity::StringFormat(" | zone {} flags {:#x} {:#x} faction {}", zone->ID, uint32(zone->Flags[0]), uint32(zone->Flags[1]),
                uint32(zone->FactionGroupMask));
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId()))
            out += Trinity::StringFormat(" | area {} flags {:#x} {:#x}", area->ID, uint32(area->Flags[0]), uint32(area->Flags[1]));
        return out;
    }

    uint32 CompanionAmbient::CountNear(Player* player) const
    {
        float const radius = GetConfig().AmbientRadius;
        uint32 count = 0;
        for (auto const& [guid, live] : _live)
        {
            CompanionAI* ai = sCompanionMgr->GetAI(guid);
            Player* bot = ai ? ai->GetBot() : nullptr;
            if (bot && bot->IsInWorld())
                count += bot->GetMap() == player->GetMap() && bot->GetDistance(player) < radius ? 1 : 0;
            else
                count += live.MapId == player->GetMapId() && player->GetDistance(live.Where) < radius ? 1 : 0;
        }
        for (Request const& request : _creating)
            count += request.Where.GetMapId() == player->GetMapId() && player->GetDistance(request.Where) < radius ? 1 : 0;
        return count;
    }

    void CompanionAmbient::Update(uint32 diff)
    {
        _timer += diff;
        if (_timer < 1000)
            return;
        uint32 const elapsed = _timer;
        _timer = 0;
        _now += elapsed;

        Config const& config = GetConfig();
        bool const enabled = config.Enable && config.AmbientEnable;

        // the real players (and the test harness's stand-ins)
        std::vector<Player*> players;
        for (auto const& [id, session] : sWorld->GetAllSessions())
            if (Player* player = session->GetPlayer(); player && player->IsInWorld() && !session->IsBot())
                players.push_back(player);
        for (Player* puppet : sCompanionMgr->GetPuppets())
            if (puppet->IsInWorld())
                players.push_back(puppet);

        // the ones out there: leave when nobody is around, after dying, or when their time is up out of sight
        uint32 online = 0;
        for (auto itr = _live.begin(); itr != _live.end();)
        {
            ObjectGuid const guid = itr->first;
            Live& live = itr->second;
            CompanionAI* ai = sCompanionMgr->GetAI(guid);
            if (!ai)
            {
                if (!sCompanionMgr->IsLoggingIn(guid))
                {
                    itr = _live.erase(itr);     // logged out
                    continue;
                }
                ++itr;
                continue;
            }
            if (!ai->IsAmbient())
            {
                itr = _live.erase(itr);         // a companion now
                continue;
            }
            ++online;
            Player* bot = ai->GetBot();
            if (!bot->IsInWorld() || bot->IsBeingTeleported())
            {
                ++itr;
                continue;
            }
            float nearest = std::numeric_limits<float>::max();
            for (Player* player : players)
                if (player->GetMap() == bot->GetMap())
                    nearest = std::min(nearest, player->GetDistance(bot));

            bool leave = !enabled;
            if (!bot->IsAlive())
            {
                live.DeadMs += elapsed;
                leave = leave || live.DeadMs >= 10000;
            }
            else if (nearest > config.AmbientRadius + 60.0f)
            {
                live.AloneMs += elapsed;
                leave = leave || live.AloneMs >= 15000;
            }
            else
            {
                live.AloneMs = 0;
                leave = leave || (_now - live.Since > live.Lifetime && nearest > 90.0f && !bot->IsInCombat());
            }
            if (leave)
                sCompanionMgr->Dismiss(guid);
            ++itr;
        }

        if (!enabled)
            return;

        // fewer allowed than there are (the setting was lowered): the farthest go first
        if (online > config.AmbientMaxTotal)
        {
            std::vector<std::pair<float, ObjectGuid>> byDistance;
            for (auto const& [guid, live] : _live)
                if (CompanionAI* ai = sCompanionMgr->GetAI(guid))
                {
                    float nearest = std::numeric_limits<float>::max();
                    for (Player* player : players)
                        if (player->GetMap() == ai->GetBot()->GetMap())
                            nearest = std::min(nearest, player->GetDistance(ai->GetBot()));
                    byDistance.emplace_back(nearest, guid);
                }
            std::sort(byDistance.begin(), byDistance.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
            for (size_t i = 0; i < byDistance.size() && i < online - config.AmbientMaxTotal; ++i)
                sCompanionMgr->Dismiss(byDistance[i].second);
            return;
        }

        // around each player: as many as the setting asks for, one at a time
        for (Player* player : players)
        {
            if (_live.size() + _creating.size() >= config.AmbientMaxTotal)
                break;
            uint32& next = _nextSpawn[player->GetGUID()];
            if (_now < next)
                continue;
            ZoneKind kind = Classify(player);
            if (!kind.Allowed)
                continue;
            uint32 wanted = kind.City ? config.AmbientCityDensity : config.AmbientDensity;
            if (CountNear(player) >= wanted)
                continue;
            next = _now + 2000;
            SpawnNear(player, kind);
        }
        // forget players long gone
        for (auto itr = _nextSpawn.begin(); itr != _nextSpawn.end();)
            itr = (_now > itr->second + 600000) ? _nextSpawn.erase(itr) : std::next(itr);
    }

    void CompanionAmbient::SpawnNear(Player* player, ZoneKind const& kind)
    {
        Team team = kind.Alliance && kind.Horde ? (urand(0, 1) ? ALLIANCE : HORDE) : (kind.Alliance ? ALLIANCE : HORDE);
        Request request;
        request.BotTeam = team;
        request.City = kind.City;
        request.ZoneId = player->GetZoneId();
        if (!PickSpot(player, kind.City, team, request))
            return;
        if (_creating.size() >= 2)
            return;     // a few characters made at a time
        ObjectGuid guid = PickCharacter(team, request.Level);
        if (guid.IsEmpty())
            Create(request);
        else
            Login(guid, request);
    }

    bool CompanionAmbient::PickSpot(Player* player, bool city, Team team, Request& request) const
    {
        float const radius = GetConfig().AmbientRadius;
        FactionTemplateEntry const* side = SideFaction(team);
        if (!side)
            return false;
        uint8 const maxLevel = MaxLevel();
        Position spot;

        if (city)
        {
            // where a crowd stands: the auction house, the bank, the inn, flight masters, trainers, vendors
            NPCFlags const wanted = NPCFlags(UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_BANKER | UNIT_NPC_FLAG_INNKEEPER
                | UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_VENDOR);
            std::vector<Creature*> places;
            for (Creature* creature : CreaturesAround(player, radius))
                if (creature->IsAlive() && creature->HasNpcFlag(wanted) && creature->GetDistance(player) > 25.0f
                    && creature->GetFactionTemplateEntry() && !creature->GetFactionTemplateEntry()->IsHostileTo(side))
                    places.push_back(creature);
            bool found = !places.empty() && GroundNear(player, places[urand(0, places.size() - 1)]->GetPosition(), 3.0f, 8.0f, spot);
            if (!found && !GroundNear(player, player->GetPosition(), 30.0f, 60.0f, spot))
                return false;
            // a capital is full of characters at the level cap and of every level below it
            request.Level = urand(0, 99) < 50 ? maxLevel : uint8(urand(10, std::max<uint32>(10, maxLevel - 1)));
        }
        else
        {
            // next to the zone's mobs, at their level: far enough not to appear under the player's nose
            std::vector<Creature*> anchors, close;
            for (Creature* creature : CreaturesAround(player, radius))
            {
                if (!creature->IsAlive() || creature->IsPet() || creature->IsTotem() || creature->IsTrigger() || creature->IsCritter()
                    || creature->IsCivilian() || creature->IsElite() || creature->isWorldBoss() || !creature->GetLevel())
                    continue;
                FactionTemplateEntry const* faction = creature->GetFactionTemplateEntry();
                if (!faction || faction->IsFriendlyTo(side))
                    continue;
                (creature->GetDistance(player) > 60.0f ? anchors : close).push_back(creature);
            }
            if (anchors.empty())
                anchors = std::move(close);
            if (!anchors.empty())
            {
                Creature* anchor = anchors[urand(0, anchors.size() - 1)];
                if (!GroundNear(player, anchor->GetPosition(), 10.0f, 22.0f, spot))
                    return false;
                request.Level = uint8(std::clamp<int32>(int32(anchor->GetLevel()) + irand(0, 2), 1, maxLevel));
            }
            else
            {
                if (!GroundNear(player, player->GetPosition(), 50.0f, 100.0f, spot))
                    return false;
                request.Level = uint8(std::clamp<int32>(int32(player->GetLevel()) + irand(-2, 2), 1, maxLevel));
            }
        }
        request.Where.WorldRelocate(player->GetMapId(), spot);
        return true;
    }

    ObjectGuid CompanionAmbient::PickCharacter(Team team, uint8 level)
    {
        // the free character of that side closest below the level (it is levelled up); none close: a new one while the
        // pool has room, else the closest one anyway, else one too high is recycled for a new one
        ObjectGuid best, highest;
        uint8 bestLevel = 0, highestLevel = 0;
        for (ObjectGuid const& guid : _pool)
        {
            if (_live.count(guid) || sCompanionMgr->IsBotGuid(guid) || sCompanionMgr->IsLoggingIn(guid))
                continue;
            CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(guid);
            if (!cache || cache->IsDeleted || Player::TeamForRace(cache->Race) != team)
                continue;
            if (cache->Level <= level && (best.IsEmpty() || cache->Level > bestLevel))
            {
                best = guid;
                bestLevel = cache->Level;
            }
            if (cache->Level > level && (highest.IsEmpty() || cache->Level > highestLevel))
            {
                highest = guid;
                highestLevel = cache->Level;
            }
        }
        bool const room = _pool.size() + _creating.size() < GetConfig().AmbientPoolSize;
        if (!best.IsEmpty() && (level - bestLevel <= 5 || !room))
            return best;
        if (room)
            return ObjectGuid::Empty;
        if (!highest.IsEmpty())
        {
            CharacterCacheEntry const* cache = sCharacterCache->GetCharacterCacheByGuid(highest);
            TC_LOG_DEBUG("server.worldserver", "Companions: ambient pool full, recycling {}", highest.ToString());
            _pool.erase(highest);
            CharacterDatabase.PExecute("DELETE FROM companion_ambient WHERE guid = {}", highest.GetCounter());
            Player::DeleteFromDB(highest, cache ? cache->AccountId : 0, true, true);
        }
        return ObjectGuid::Empty;
    }

    void CompanionAmbient::Create(Request const& request)
    {
        static uint8 const Classes[] = { CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE, CLASS_PRIEST, CLASS_SHAMAN,
            CLASS_MAGE, CLASS_WARLOCK, CLASS_DRUID };
        std::vector<uint8> classes;
        for (uint8 classId : Classes)
            if (!sCompanionFactory->RacesFor(request.BotTeam, classId).empty())
                classes.push_back(classId);
        if (classes.empty())
            return;
        uint8 const classId = classes[urand(0, classes.size() - 1)];

        uint32 build = 0;
        if (std::shared_ptr<Realm const> realm = sRealmList->GetCurrentRealm())
            build = realm->Build;
        std::string error;
        _creating.push_back(request);
        ObjectGuid guid = sCompanionFactory->CreateCharacter(request.BotTeam, build, classId, 0, 2, std::string(), { }, error,
            [this, request](ObjectGuid created, bool saved)
            {
                auto itr = std::find_if(_creating.begin(), _creating.end(), [&](Request const& r)
                    { return r.Where.GetExactDist(&request.Where) < 0.1f && r.Where.GetMapId() == request.Where.GetMapId(); });
                if (itr != _creating.end())
                    _creating.erase(itr);
                if (!saved)
                    return;
                _pool.insert(created);
                CharacterDatabase.PExecute("INSERT IGNORE INTO companion_ambient (guid, created) VALUES ({}, UNIX_TIMESTAMP())", created.GetCounter());
                if (GetConfig().AmbientEnable)
                    Login(created, request);
            });
        if (guid.IsEmpty())
        {
            _creating.pop_back();
            TC_LOG_DEBUG("server.worldserver", "Companions: no ambient character: {}", error);
        }
    }

    void CompanionAmbient::Login(ObjectGuid guid, Request const& request)
    {
        if (!sCompanionMgr->LoginAmbient(guid, request.Where, request.ZoneId, request.Level, request.City))
            return;
        Live live;
        live.MapId = request.Where.GetMapId();
        live.Where = request.Where.GetPosition();
        live.Since = _now;
        live.Lifetime = urand(8, 20) * MINUTE * IN_MILLISECONDS;
        _live[guid] = live;
    }
}
