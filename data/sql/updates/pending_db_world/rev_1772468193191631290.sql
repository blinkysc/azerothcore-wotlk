-- Mutilate MH sub-spells: bind spell script for extra MH poison proc chance.
-- In WotLK, Mutilate grants 2 MH poison proc chances and 1 OH proc chance.
-- The parent spell's SPELL_ATTR3_SUPPRESS_CASTER_PROCS prevents its Effect 0
-- (normalized weapon damage) from triggering weapon procs, so the MH sub-spell
-- script adds the missing second MH proc.
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_rog_mutilate_mh';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(5374, 'spell_rog_mutilate_mh'),
(34414, 'spell_rog_mutilate_mh'),
(34416, 'spell_rog_mutilate_mh'),
(34419, 'spell_rog_mutilate_mh'),
(48662, 'spell_rog_mutilate_mh'),
(48665, 'spell_rog_mutilate_mh');
