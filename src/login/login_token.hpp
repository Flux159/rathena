// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef LOGIN_TOKEN_HPP
#define LOGIN_TOKEN_HPP

#include <common/cbasetypes.hpp>

struct AccountDB;
struct login_session_data;
struct mmo_account;

/**
 * One-time login tokens, accepted in place of a password.
 *
 * Something outside the login server -- a web sign-in, for instance -- has
 * already decided who the player is, and wants the ordinary client to log in
 * without a password. It makes a random token, stores only its SHA-256 in
 * `login_tokens` (sql-files/main.sql) with a short expiry, and hands the token
 * to the client, which sends it in the password field of its normal login.
 *
 * A token:
 *   - has exactly the shape `~` followed by 22 base64url characters (23
 *     characters, the most CA_LOGIN's password field holds), so no ordinary
 *     password is ever looked up as one;
 *   - belongs to one account, and only logs that account in;
 *   - works once: it is marked used by the same statement that checks it is
 *     still unused;
 *   - expires (the row's `expires`, compared with the database's clock).
 *
 * The stored hash is compared in constant time. The token is never logged.
 * A password that is not a token, or a token that does not match, falls
 * through to the ordinary password check unchanged.
 */

/// True if `passwd` has a token's shape (see above).
bool login_token_shape( const char* passwd );

/// Accept `sd.passwd` as a one-time token for `acc`, consuming it. False for
/// anything else, including a well-formed token that is unknown, expired,
/// used, or another account's.
bool login_token_accept( AccountDB* accounts, struct login_session_data& sd, struct mmo_account& acc );

/// Close the token check's database connection, at shutdown.
void login_token_final();

#endif /* LOGIN_TOKEN_HPP */
