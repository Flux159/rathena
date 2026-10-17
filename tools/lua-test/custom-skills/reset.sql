-- The tester's account and character, as they are before the run.
DELETE FROM `login` WHERE `account_id` = 2000000;
INSERT INTO `login` (`account_id`, `userid`, `user_pass`, `sex`, `email`, `group_id`, `character_slots`)
VALUES (2000000, 'tester', 'tester123', 'M', 'tester@example.com', 99, 9);
DELETE FROM `char` WHERE `char_id` = 150000;
INSERT INTO `char` (`char_id`, `account_id`, `char_num`, `name`, `class`, `base_level`, `job_level`,
  `str`, `agi`, `vit`, `int`, `dex`, `luk`, `max_hp`, `hp`, `max_sp`, `sp`, `status_point`, `skill_point`,
  `last_map`, `last_x`, `last_y`, `save_map`, `save_x`, `save_y`, `hair`, `hair_color`, `sex`)
VALUES (150000, 2000000, 0, 'Tester', 4059, 150, 50,
  90, 90, 60, 20, 60, 30, 20000, 20000, 1000, 1000, 0, 10,
  'prontera', 156, 191, 'prontera', 156, 191, 1, 0, 'M');
DELETE FROM `skill` WHERE `char_id` = 150000;
-- Learned before this mod took them away: 5 + 3 points to refund.
-- And the prerequisites for the client's own skill-ups.
INSERT INTO `skill` (`char_id`, `id`, `lv`, `flag`) VALUES
  (150000, 2023, 5, 0),   -- GC_DARKILLUSION
  (150000, 2035, 3, 0),   -- GC_HALLUCINATIONWALK
  (150000, 2024, 3, 0),   -- GC_RESEARCHNEWPOISON 3 -> GCR_CONTACT_POISON
  (150000, 135, 10, 0),   -- AS_CLOAKING -> GC_CLOAKINGEXCEED
  (150000, 2033, 5, 0),   -- GC_CLOAKINGEXCEED 5 -> GC_PHANTOMMENACE (no Dark Illusion now)
  (150000, 50, 10, 0),    -- TF_STEAL -> TF_HIDING
  (150000, 51, 10, 0),    -- TF_HIDING -> AS_CLOAKING
  (150000, 134, 10, 0),   -- AS_KATAR -> AS_SONICBLOW
  (150000, 136, 10, 0),   -- AS_SONICBLOW -> GC_CROSSIMPACT
  (150000, 2022, 5, 0);   -- GC_CROSSIMPACT -> GCR_EXECUTIONER
DELETE FROM `char_reg_num` WHERE `char_id` = 150000;
DELETE FROM `inventory` WHERE `char_id` = 150000;
