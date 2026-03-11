-- Fingers of Frost buff (74396): change SpellPhaseMask from 3 (CAST|HIT) to 1 (CAST only).
-- With CAST procs now firing before handle_immediate(), charges are correctly
-- consumed during the CAST phase. HIT phase is no longer needed.
UPDATE `spell_proc` SET `SpellPhaseMask` = 1 WHERE `SpellId` = 74396;
