-- Remove stale spell_script_names entry for spell_sha_t8_electrified.
-- Spell 64928 (Shaman T8 Elemental 4P Bonus) had two script registrations
-- causing Electrified to proc twice per Lightning Bolt crit.
-- The surviving script is spell_sha_t8_elemental_4p_bonus.
DELETE FROM `spell_script_names` WHERE `spell_id` = 64928 AND `ScriptName` = 'spell_sha_t8_electrified';
