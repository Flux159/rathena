-- The tester for the sampler: a Guillotine Cross with no skills yet; every
-- step grants what it tests.
DELETE FROM `login` WHERE `account_id` = 2000000;
INSERT INTO `login` (`account_id`, `userid`, `user_pass`, `sex`, `email`, `group_id`, `character_slots`)
VALUES (2000000, 'tester', 'tester123', 'M', 'tester@example.com', 99, 9);
DELETE FROM `char` WHERE `char_id` = 150000;
INSERT INTO `char` (`char_id`, `account_id`, `char_num`, `name`, `class`, `base_level`, `job_level`,
  `str`, `agi`, `vit`, `int`, `dex`, `luk`, `max_hp`, `hp`, `max_sp`, `sp`, `status_point`, `skill_point`,
  `last_map`, `last_x`, `last_y`, `save_map`, `save_x`, `save_y`, `hair`, `hair_color`, `sex`)
VALUES (150000, 2000000, 0, 'Tester', 4059, 150, 50,
  90, 90, 60, 60, 90, 30, 20000, 20000, 2000, 2000, 0, 10,
  'prontera', 156, 191, 'prontera', 156, 191, 1, 0, 'M');
DELETE FROM `skill` WHERE `char_id` = 150000;
DELETE FROM `char_reg_num` WHERE `char_id` = 150000;
DELETE FROM `inventory` WHERE `char_id` = 150000;
DELETE FROM `sc_data` WHERE `char_id` = 150000;
