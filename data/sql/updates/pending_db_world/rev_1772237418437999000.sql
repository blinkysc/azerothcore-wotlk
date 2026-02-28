-- Arcane Potency buffs (57529/57531): add PROC_ATTR_DISALLOW_PROC_ON_APPLY
-- so the buff cannot be consumed by the same spell cast that created it
-- (fixes Arcane Potency + Clearcasting interaction with AoE spells like Arcane Explosion)
UPDATE `spell_proc` SET `AttributesMask` = `AttributesMask` | 0x20 WHERE `SpellId` IN (57529, 57531);
