-- Add Multi-Shot to Thrill of the Hunt (-34497) proc mask
-- Multi-Shot SpellFamilyFlags[0] = 0x1000, was missing from SpellFamilyMask0
-- Script limits Multi-Shot to one proc per cast to prevent multi-target mana exploit
UPDATE `spell_proc` SET `SpellFamilyMask0` = 399360 WHERE `SpellId` = -34497;
