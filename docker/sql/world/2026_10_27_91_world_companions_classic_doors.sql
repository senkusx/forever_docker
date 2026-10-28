-- Companions: the doors the ported VMaNGOS scripts open and close.
--
-- VMaNGOS "open door" only opens a closed door and "close door" only closes an open one. The port turned them into
-- SMART_ACTION_ACTIVATE_GAMEOBJECT (147) with Open (8) and Close (10), which this core does differently: Open uses the
-- door (a toggle: an open door closes) and Close only resets a door something used before (a door that starts open stays
-- open). Emberseer's room door shut when he died, the gates of Gandling's six portal rooms, the Shadowforge gate in
-- Blackrock Depths, the Scarlet Monastery chapel, cathedral and armory doors, the Deadmines foundry and mast room doors,
-- Shadowfang Keep's courtyard door, Blackwing Lair's portcullis lever and the others behaved at random.
-- SMART_ACTION_GO_SET_GO_STATE (118) sets the state itself: 0 open, 1 closed.
UPDATE `smart_scripts` SET `action_type` = 118, `action_param1` = IF(`action_param1` = 8, 0, 1)
WHERE `action_type` = 147 AND `action_param1` IN (8, 10) AND `comment` LIKE 'VMaNGOS%';
