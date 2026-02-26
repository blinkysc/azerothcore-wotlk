-- Darkmoon Card: Blue Dragon - add NONE DmgClass proc flags to match "on successful spellcast"
UPDATE `spell_proc` SET `ProcFlags` = 0x15400 WHERE `SpellId` = 23688;
