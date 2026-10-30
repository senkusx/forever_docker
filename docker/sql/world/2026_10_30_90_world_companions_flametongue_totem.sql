-- Companions: Flametongue Totem added no Fire damage. Its creatures had no spell (Totem::InitSummon casts the
-- creature_template_spell Index 0), and its party aura has no trigger spell in the 1.60.1 client: the proc spell is the
-- effect's base points, like Windfury Totem's (see CompanionSpells.cpp). Ranks from SpellEffect.db2: summon 8227, 8249,
-- 10526, 16387 -> creatures 5950, 6012, 7423, 10557 -> auras 8230, 8250, 10521, 15036. The higher ranks also carried
-- Blizzard's "zzOLD" names.
DELETE FROM `creature_template_spell` WHERE `CreatureID` IN (5950, 6012, 7423, 10557) AND `Index` = 0;
INSERT INTO `creature_template_spell` (`CreatureID`, `Index`, `Spell`, `VerifiedBuild`) VALUES
(5950, 0, 8230, 0),
(6012, 0, 8250, 0),
(7423, 0, 10521, 0),
(10557, 0, 15036, 0);

UPDATE `creature_template` SET `name` = 'Flametongue Totem II' WHERE `entry` = 6012 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Flametongue Totem III' WHERE `entry` = 7423 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Flametongue Totem IV' WHERE `entry` = 10557 AND `name` LIKE 'zzOLD%';

-- every melee hit, main hand and off hand (deal melee swing 0x4, deal melee ability 0x10; on hit)
DELETE FROM `spell_proc` WHERE `SpellId` IN (8230, 8250, 10521, 15036);
INSERT INTO `spell_proc` (`SpellId`, `ProcFlags`, `SpellPhaseMask`, `Chance`) VALUES
(8230, 0x14, 0x2, 100),
(8250, 0x14, 0x2, 100),
(10521, 0x14, 0x2, 100),
(15036, 0x14, 0x2, 100);

DELETE FROM `spell_script_names` WHERE `spell_id` IN (8230, 8250, 10521, 15036);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(8230, 'spell_companions_flametongue_totem_passive'),
(8250, 'spell_companions_flametongue_totem_passive'),
(10521, 'spell_companions_flametongue_totem_passive'),
(15036, 'spell_companions_flametongue_totem_passive');
