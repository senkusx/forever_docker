-- Companions: Razorgore the Untamed could not be controlled, so Blackwing Lair stopped at its first boss.
--
-- In the 1.60 client data Possess (19832, cast by the Orb of Domination's user) aims at "the nearby creature of an entry"
-- and Destroy Egg (19873, cast by the possessed Razorgore) at "the nearby game object of an entry". TrinityCore takes that
-- entry from spell target conditions, and there were none: Possess fell back to its caster ("trying to charm itself") and
-- nobody ever controlled Razorgore, so his eggs could not be broken and his phase one always ended in the raid wipe.
DELETE FROM `conditions` WHERE `SourceTypeOrReferenceId` = 13 AND `SourceEntry` IN (19832, 19873);
INSERT INTO `conditions` (`SourceTypeOrReferenceId`, `SourceGroup`, `SourceEntry`, `SourceId`, `ElseGroup`, `ConditionTypeOrReference`,
  `ConditionTarget`, `ConditionValue1`, `ConditionValue2`, `ConditionValue3`, `NegativeCondition`, `ErrorType`, `ErrorTextId`, `ScriptName`, `Comment`) VALUES
(13, 5, 19832, 0, 0, 31, 0, 3, 12435, 0, 0, 0, 0, '', 'Possess (Orb of Domination): Razorgore the Untamed'),
(13, 1, 19873, 0, 0, 31, 0, 5, 177807, 0, 0, 0, 0, '', 'Destroy Egg (Razorgore): Black Dragon Egg');
