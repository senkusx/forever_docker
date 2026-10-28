-- Companions: SmartAI rows that act on the wrong thing (found with the worldserver's DBErrors.log and the script data).
--
-- The VMaNGOS import made "remove aura" act on the creature's victim; VMaNGOS removes it from the creature itself.
-- Syndicate rogues kept Sneak in the fight, Gyth kept Rend on his back after Rend jumped off, channelers kept
-- channeling, Mosshide gnolls their Moss Hide, and Scarlet Commander Mograine, faking his death, took every aura off
-- the player he fought instead of his own.
UPDATE `smart_scripts` SET `target_type` = 1
WHERE `source_type` = 0 AND `action_type` = 28 AND `target_type` = 2 AND `comment` LIKE 'VMaNGOS%';

-- TrinityCore's own scripts name game objects by the spawn ids of its database, and Forever's database numbered them
-- anew (30000000 + the old id), so their levers and torches pointed at nothing. Most of them also have a script of
-- their own spawn from the VMaNGOS import, which runs instead (the Shadowforge Lock, the chapel and library doors of
-- the Scarlet Monastery); the lever of Herod's door in the Armory had only this one.
UPDATE `smart_scripts` ss
JOIN `gameobject` g ON g.`guid` = ss.`target_param1` + 30000000 AND g.`id` = ss.`target_param2`
SET ss.`target_param1` = g.`guid`
WHERE ss.`target_type` = 14 AND NOT EXISTS (SELECT 1 FROM `gameobject` old WHERE old.`guid` = ss.`target_param1`);

-- numbered otherwise (the Cathedral and Armory doors outside Scarlet Monastery): the only spawn of the object named
UPDATE `smart_scripts` ss
JOIN (SELECT `id`, MIN(`guid`) AS `guid` FROM `gameobject` GROUP BY `id` HAVING COUNT(*) = 1) g ON g.`id` = ss.`target_param2`
SET ss.`target_param1` = g.`guid`
WHERE ss.`target_type` = 14 AND ss.`target_param2` <> 0 AND NOT EXISTS (SELECT 1 FROM `gameobject` old WHERE old.`guid` = ss.`target_param1`);
