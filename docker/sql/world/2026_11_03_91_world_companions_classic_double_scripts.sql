-- Companions: spells the forever branch's classic boss scripts handle themselves, while the database still binds them to
-- TrinityCore's own scripts of the same bosses, so both ran.
--
-- Baron Geddon's Inferno (19695): the classic boss script deals its eight blasts (500 up to 5000), and the stock aura script
-- dealt the same eight again from the aura: 30000 damage per Inferno instead of 15000, and the raid around him died in it.
-- Razorgore's Destroy Egg (19873): the classic instance counts the eggs itself, and the stock script told the instance
-- "egg event: special" under the stock data id 2, which the classic instance takes as Broodlord Lashlayer's encounter state:
-- every egg set Broodlord's encounter to SPECIAL.
DELETE FROM `spell_script_names` WHERE `spell_id` = 19695 AND `ScriptName` = 'spell_baron_geddon_inferno';
DELETE FROM `spell_script_names` WHERE `spell_id` = 19873 AND `ScriptName` = 'spell_egg_event';
