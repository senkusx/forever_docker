/*
 * Companions: spells of the 1.60.1 client that Forever's data leaves without effect, fixed for players and companions
 * alike (the matching rows are in docker/sql/world/2026_10_26_91_world_companions_totems.sql).
 */

#ifndef COMPANION_SPELLS_H
#define COMPANION_SPELLS_H

namespace Companions
{
    void AddSpellScripts();
}

#endif
