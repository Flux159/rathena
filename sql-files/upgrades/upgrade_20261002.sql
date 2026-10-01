-- One-time login tokens, accepted in place of a password (src/login/login_token.hpp).
-- Only the SHA-256 of a token is stored.
CREATE TABLE IF NOT EXISTS `login_tokens` (
  `id` int(11) unsigned NOT NULL auto_increment,
  `account_id` int(11) unsigned NOT NULL,
  `token_hash` char(64) NOT NULL,
  `expires` datetime NOT NULL,
  `used` tinyint(1) unsigned NOT NULL default '0',
  PRIMARY KEY (`id`),
  UNIQUE KEY `token_hash` (`token_hash`),
  KEY `account_id` (`account_id`)
) ENGINE=MyISAM;
