-- Remove IMMUNE_TO_PC from creatures whose scripts never clear it (breaks threat engagement after threat system port)
-- Gortok Palehoof encounter (Utgarde Pinnacle): boss + 4 beasts, normal + heroic
UPDATE `creature_template` SET `unit_flags` = `unit_flags` & ~0x100 WHERE `entry` IN (26683, 26684, 26685, 26686, 26687, 30770, 30772, 30774, 30790, 30803);
-- Razorscale (Ulduar): normal + heroic
UPDATE `creature_template` SET `unit_flags` = `unit_flags` & ~0x100 WHERE `entry` IN (33186, 33724);
-- Scourgelord Tyrannus (Pit of Saron): normal + heroic
UPDATE `creature_template` SET `unit_flags` = `unit_flags` & ~0x100 WHERE `entry` IN (36658, 36938);
