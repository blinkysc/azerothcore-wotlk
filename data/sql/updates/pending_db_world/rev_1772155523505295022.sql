-- Cold Blood: Bind AuraScript to prevent charge consumption on Mutilate MH,
-- so the OH sub-spell also gets the guaranteed crit.
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_rog_cold_blood';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(14177, 'spell_rog_cold_blood');
