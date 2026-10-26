-- Companions: shaman totems and Consecration.
--
-- Totems: Totem::InitSummon casts the creature's creature_template_spell Index 0, but Forever's world has no row for most
-- player totems (Windfury, Strength of Earth, Mana Spring...), so they stood there without effect, and Healing Stream,
-- Searing and Magma cast their rank 1 spell at every rank. Spells per rank come from the 1.60.1 client (SpellEffect.db2:
-- the summon spell's creature, and the area aura with the same ranks), every one of them exists in SpellName.db2.
-- The higher ranks also carried Blizzard's "zzOLD" names (zzOLDWindfury Totem II) and get their classic names back.

DELETE FROM `creature_template_spell` WHERE `CreatureID` IN (6112,7483,7484,5874,5921,5922,7403,15464,5873,5919,5920,7366,7367,7368,3573,7414,7415,7416,3527,3906,3907,3908,3909,2523,3902,3903,3904,7400,7402,5929,7464,7465,7466,3579,3911,3912,3913,7398,7399,5923,5924,5926,7412,7413,5927,7424,7425,7467,7468,7469,7486,7487,15463,9687,9688,9689,10467,11100,11101,15803) AND `Index` = 0;
INSERT INTO `creature_template_spell` (`CreatureID`, `Index`, `Spell`, `VerifiedBuild`) VALUES
-- Windfury Totem (the aura procs the Windfury attack, see CompanionSpells.cpp)
(6112, 0, 8515, 0), -- Windfury Totem
(7483, 0, 10609, 0), -- Windfury Totem II
(7484, 0, 10612, 0), -- Windfury Totem III
-- Strength of Earth Totem
(5874, 0, 8076, 0), -- Strength of Earth Totem
(5921, 0, 8162, 0), -- Strength of Earth Totem II
(5922, 0, 8163, 0), -- Strength of Earth Totem III
(7403, 0, 10441, 0), -- Strength of Earth Totem IV
(15464, 0, 25362, 0), -- Strength of Earth Totem V
-- Stoneskin Totem
(5873, 0, 8072, 0), -- Stoneskin Totem
(5919, 0, 8156, 0), -- Stoneskin Totem II
(5920, 0, 8157, 0), -- Stoneskin Totem III
(7366, 0, 10403, 0), -- Stoneskin Totem IV
(7367, 0, 10404, 0), -- Stoneskin Totem V
(7368, 0, 10405, 0), -- Stoneskin Totem VI
-- Mana Spring Totem
(3573, 0, 5677, 0), -- Mana Spring Totem
(7414, 0, 10491, 0), -- Mana Spring Totem II
(7415, 0, 10493, 0), -- Mana Spring Totem III
(7416, 0, 10494, 0), -- Mana Spring Totem IV
-- Healing Stream Totem (Forever casts rank 1 for every rank)
(3527, 0, 5672, 0), -- Healing Stream Totem
(3906, 0, 6371, 0), -- Healing Stream Totem II
(3907, 0, 6372, 0), -- Healing Stream Totem III
(3908, 0, 10460, 0), -- Healing Stream Totem IV
(3909, 0, 10461, 0), -- Healing Stream Totem V
-- Searing Totem (Forever casts rank 1 for every rank)
(2523, 0, 3606, 0), -- Searing Totem
(3902, 0, 6350, 0), -- Searing Totem II
(3903, 0, 6351, 0), -- Searing Totem III
(3904, 0, 6352, 0), -- Searing Totem IV
(7400, 0, 10435, 0), -- Searing Totem V
(7402, 0, 10436, 0), -- Searing Totem VI
-- Magma Totem (Forever casts rank 1 for every rank)
(5929, 0, 8188, 0), -- Magma Totem
(7464, 0, 10582, 0), -- Magma Totem II
(7465, 0, 10583, 0), -- Magma Totem III
(7466, 0, 10584, 0), -- Magma Totem IV
-- Stoneclaw Totem
(3579, 0, 5728, 0), -- Stoneclaw Totem
(3911, 0, 6397, 0), -- Stoneclaw Totem II
(3912, 0, 6398, 0), -- Stoneclaw Totem III
(3913, 0, 6399, 0), -- Stoneclaw Totem IV
(7398, 0, 10425, 0), -- Stoneclaw Totem V
(7399, 0, 10426, 0), -- Stoneclaw Totem VI
-- Poison and Disease Cleansing Totems
(5923, 0, 8167, 0), -- Poison Cleansing Totem
(5924, 0, 8172, 0), -- Cleansing Totem
-- Frost, Fire and Nature Resistance Totems
(5926, 0, 8182, 0), -- Frost Resistance Totem
(7412, 0, 10476, 0), -- Frost Resistance Totem II
(7413, 0, 10477, 0), -- Frost Resistance Totem III
(5927, 0, 8185, 0), -- Elemental Resistance Totem
(7424, 0, 10534, 0), -- Fire Resistance Totem II
(7425, 0, 10535, 0), -- Fire Resistance Totem III
(7467, 0, 10596, 0), -- Nature Resistance Totem
(7468, 0, 10598, 0), -- Nature Resistance Totem II
(7469, 0, 10599, 0), -- Nature Resistance Totem III
-- Grace of Air Totem
(7486, 0, 8836, 0), -- Grace of Air Totem
(7487, 0, 10626, 0), -- Grace of Air Totem II
(15463, 0, 25360, 0), -- Grace of Air Totem III
-- Windwall Totem
(9687, 0, 15108, 0), -- Windwall Totem
(9688, 0, 15109, 0), -- Windwall Totem II
(9689, 0, 15110, 0), -- Windwall Totem III
-- Mana Tide Totem
(10467, 0, 16191, 0), -- Mana Tide Totem
(11100, 0, 17355, 0), -- Mana Tide Totem II
(11101, 0, 17360, 0), -- Mana Tide Totem III
-- Tranquil Air Totem
(15803, 0, 25909, 0); -- Tranquil Air Totem

UPDATE `creature_template` SET `name` = 'Windfury Totem II' WHERE `entry` = 7483 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Windfury Totem III' WHERE `entry` = 7484 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Strength of Earth Totem II' WHERE `entry` = 5921 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Strength of Earth Totem III' WHERE `entry` = 5922 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Strength of Earth Totem IV' WHERE `entry` = 7403 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Strength of Earth Totem V' WHERE `entry` = 15464 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneskin Totem II' WHERE `entry` = 5919 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneskin Totem III' WHERE `entry` = 5920 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneskin Totem IV' WHERE `entry` = 7366 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneskin Totem V' WHERE `entry` = 7367 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneskin Totem VI' WHERE `entry` = 7368 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Mana Spring Totem II' WHERE `entry` = 7414 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Mana Spring Totem III' WHERE `entry` = 7415 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Mana Spring Totem IV' WHERE `entry` = 7416 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Healing Stream Totem II' WHERE `entry` = 3906 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Healing Stream Totem III' WHERE `entry` = 3907 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Healing Stream Totem IV' WHERE `entry` = 3908 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Healing Stream Totem V' WHERE `entry` = 3909 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Searing Totem II' WHERE `entry` = 3902 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Searing Totem III' WHERE `entry` = 3903 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Searing Totem IV' WHERE `entry` = 3904 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Searing Totem V' WHERE `entry` = 7400 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Searing Totem VI' WHERE `entry` = 7402 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Magma Totem II' WHERE `entry` = 7464 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Magma Totem III' WHERE `entry` = 7465 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Magma Totem IV' WHERE `entry` = 7466 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneclaw Totem II' WHERE `entry` = 3911 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneclaw Totem III' WHERE `entry` = 3912 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneclaw Totem IV' WHERE `entry` = 3913 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneclaw Totem V' WHERE `entry` = 7398 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Stoneclaw Totem VI' WHERE `entry` = 7399 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Frost Resistance Totem II' WHERE `entry` = 7412 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Frost Resistance Totem III' WHERE `entry` = 7413 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Fire Resistance Totem II' WHERE `entry` = 7424 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Fire Resistance Totem III' WHERE `entry` = 7425 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Nature Resistance Totem II' WHERE `entry` = 7468 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Nature Resistance Totem III' WHERE `entry` = 7469 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Grace of Air Totem' WHERE `entry` = 7486 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Grace of Air Totem II' WHERE `entry` = 7487 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Grace of Air Totem III' WHERE `entry` = 15463 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Windwall Totem' WHERE `entry` = 9687 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Windwall Totem II' WHERE `entry` = 9688 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Windwall Totem III' WHERE `entry` = 9689 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Mana Tide Totem II' WHERE `entry` = 11100 AND `name` LIKE 'zzOLD%';
UPDATE `creature_template` SET `name` = 'Mana Tide Totem III' WHERE `entry` = 11101 AND `name` LIKE 'zzOLD%';

-- Windfury Totem: the client's aura has no trigger spell (the Windfury attack is the effect's base points), the script in
-- CompanionSpells.cpp casts it. 20% on a main hand hit, as in 1.12.
DELETE FROM `spell_proc` WHERE `SpellId` IN (8515, 10609, 10612);
INSERT INTO `spell_proc` (`SpellId`, `ProcFlags`, `SpellPhaseMask`, `Chance`) VALUES
(8515, 0x14, 0x2, 20),
(10609, 0x14, 0x2, 20),
(10612, 0x14, 0x2, 20);
DELETE FROM `spell_script_names` WHERE `spell_id` IN (8515, 10609, 10612);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(8515, 'spell_companions_windfury_totem_passive'),
(10609, 'spell_companions_windfury_totem_passive'),
(10612, 'spell_companions_windfury_totem_passive');

-- Consecration (26573, 20116, 20922, 20923, 20924) creates area triggers 41311 and 41313-41316 that Forever's world does not
-- have: the damage still ticked (classic_spell_pal_consecration casts it from the paladin's aura), but the spell failed to
-- create the consecrated ground, so nothing showed. Same template and 8 yd sphere as the retail Consecration (4488),
-- without its retail script: the ground only carries the spell's visual.
DELETE FROM `areatrigger_create_properties` WHERE `Id` IN (41311, 41313, 41314, 41315, 41316) AND `IsCustom` = 0;
INSERT INTO `areatrigger_create_properties` (`Id`, `IsCustom`, `AreaTriggerId`, `IsAreatriggerCustom`, `Flags`, `Shape`, `ShapeData0`, `ShapeData1`, `ScriptName`, `VerifiedBuild`) VALUES
(41311, 0, 9228, 0, 0, 0, 8, 8, '', 0),
(41313, 0, 9228, 0, 0, 0, 8, 8, '', 0),
(41314, 0, 9228, 0, 0, 0, 8, 8, '', 0),
(41315, 0, 9228, 0, 0, 0, 8, 8, '', 0),
(41316, 0, 9228, 0, 0, 0, 8, 8, '', 0);
