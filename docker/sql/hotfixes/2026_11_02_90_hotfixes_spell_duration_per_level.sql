-- WoW Forever 1.60.1.70707: SpellDuration.db2 has a DurationPerLevel field again, between Duration and MaxDuration
-- (layout 0x4FDF0951); the hotfix query of core patch 0008 reads it.
ALTER TABLE `spell_duration` ADD COLUMN `DurationPerLevel` int NOT NULL DEFAULT '0' AFTER `Duration`;
