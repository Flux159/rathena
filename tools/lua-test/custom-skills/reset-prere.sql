-- Pre-renewal has no Guillotine Cross: the tester is an Assassin Cross at the
-- era's highest base level, and prere_tree.py puts the example's tree there.
UPDATE `char` SET `class` = 4013, `base_level` = 99 WHERE `char_id` = 150000;
