/*
 * Companions: ambient bots, to make the world feel alive. Random characters appear around real players in the open
 * world, at the level and with the gear of the zone, and live there on their own (grinding, walking up to quest givers,
 * wandering; in capital cities the auction house, the bank, the inn...). A zone of one faction gets bots of that
 * faction, contested zones both. They leave again when no player is around. Invited to a group, one becomes a companion
 * of whoever invited it. Settings Ambient.* (Density, CityDensity, MaxTotal, Radius, PoolSize, GearQuality).
 *
 * The characters are kept in a pool (characters.companion_ambient) and reused; one too high in level for where it is
 * needed is recycled once the pool is full.
 */

#ifndef COMPANION_AMBIENT_H
#define COMPANION_AMBIENT_H

#include "ObjectGuid.h"
#include "Position.h"
#include "SharedDefines.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;

namespace Companions
{
    class CompanionAmbient
    {
    public:
        static CompanionAmbient* instance();

        void Initialize();          // world startup: the pool
        void Update(uint32 diff);   // world thread
        void Forget(ObjectGuid guid);   // adopted as a companion: no longer ambient
        bool IsPooled(ObjectGuid guid) const { return _pool.count(guid) != 0; }
        size_t PoolCount() const { return _pool.size(); }
        size_t LiveCount() const { return _live.size(); }
        std::string Describe(Player* player) const;     // test harness: the zone as the ambient bots see it

        struct ZoneKind
        {
            bool Allowed = false;   // open world, not flying, not in an instance
            bool City = false;
            bool Alliance = false;  // bots of that faction may live here
            bool Horde = false;
        };
        static ZoneKind Classify(Player* player);

    private:
        struct Live
        {
            uint32 MapId = 0;
            Position Where;
            uint32 Since = 0;
            uint32 Lifetime = 0;
            uint32 AloneMs = 0;
            uint32 DeadMs = 0;
        };
        struct Request
        {
            Team BotTeam = ALLIANCE;
            WorldLocation Where;
            uint32 ZoneId = 0;
            uint8 Level = 1;
            bool City = false;
            bool Borrowed = false;      // a player's companion not out with him
        };

        void SpawnNear(Player* player, ZoneKind const& kind);
        bool PickSpot(Player* player, bool city, Team team, Request& request) const;
        ObjectGuid PickCharacter(Team team, uint8 level);
        void Create(Request const& request);
        void Login(ObjectGuid guid, Request const& request);
        uint32 CountNear(Player* player) const;

        std::unordered_set<ObjectGuid> _pool;
        std::unordered_map<ObjectGuid, Live> _live;     // logging in or in the world
        std::vector<Request> _creating;                 // characters being made for these spots
        std::unordered_map<ObjectGuid, uint32> _nextSpawn;  // per real player
        uint32 _timer = 0;
        uint32 _now = 0;
    };
}

#define sCompanionAmbient Companions::CompanionAmbient::instance()

#endif
