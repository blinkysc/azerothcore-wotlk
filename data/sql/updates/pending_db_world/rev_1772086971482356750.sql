-- Add PROC_FLAG_DONE_SPELL_NONE_DMG_CLASS_POS/NEG to Darkmoon Card: Blue Dragon (spell 23688)
-- The DBC ProcFlags (0x14000) only cover magic damage class spells, missing spells with
-- DmgClass=0 (NONE) like Prayer of Mending's initial cast, buffs, and other utility spells.
-- The tooltip says "2% chance on successful spellcast" which should include all direct casts.
-- Mirrors the approach used for Soul Preserver (60510) which also adds 0x400 to its spell_proc.
UPDATE `spell_proc` SET `ProcFlags` = 0x15400 WHERE `SpellId` = 23688;
