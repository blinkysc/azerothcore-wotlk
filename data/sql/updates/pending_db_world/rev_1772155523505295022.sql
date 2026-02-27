-- Mutilate sub-spells: skip independent hit rolls, parent spell is the single gate.
-- Negative spell_id applies to all ranks in the spell chain.
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_rog_mutilate_strike';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(-5374,  'spell_rog_mutilate_strike'),
(-27576, 'spell_rog_mutilate_strike');

-- Cold Blood: Bind AuraScript to prevent charge consumption on Mutilate MH,
-- so the OH sub-spell also gets the guaranteed crit.
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_rog_cold_blood';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(14177, 'spell_rog_cold_blood');
