UPDATE `spell_proc` SET `SpellFamilyMask0` = 4096, `ProcFlags` = 0, `Charges` = 0 WHERE `SpellId` = 36032;

-- Missile Barrage (44401): use PROC_ATTR_REQ_SPELLMOD to consume charges
-- only when the cast spell is actually modified by the aura (Arcane Missiles),
-- instead of relying on SpellFamilyMask filtering
UPDATE `spell_proc` SET `SpellFamilyMask0` = 0, `AttributesMask` = 8 WHERE `SpellId` = 44401;
