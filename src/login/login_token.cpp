// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "login_token.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

#include <common/cbasetypes.hpp>
#include <common/showmsg.hpp>
#include <common/sql.hpp>
#include <common/strlib.hpp>

#include "account.hpp"
#include "login.hpp"

/// The token table (sql-files/main.sql).
static const char* login_token_table = "login_tokens";

/// Its own connection to the account database, opened on first use with the
/// account engine's settings, so the account engine itself is untouched.
static Sql* sql_handle = nullptr;
static bool sql_failed = false;

static bool login_token_connect( AccountDB* accounts ){
	if( sql_handle != nullptr )
		return true;
	// One failed connection is reported once; every token after it is simply
	// refused, and passwords keep working.
	if( sql_failed || accounts == nullptr )
		return false;

	char host[256], port[8], user[256], pass[256], db[256], codepage[32];
	if( !accounts->get_property( accounts, "login_server_ip", host, sizeof( host ) )
		|| !accounts->get_property( accounts, "login_server_port", port, sizeof( port ) )
		|| !accounts->get_property( accounts, "login_server_id", user, sizeof( user ) )
		|| !accounts->get_property( accounts, "login_server_pw", pass, sizeof( pass ) )
		|| !accounts->get_property( accounts, "login_server_db", db, sizeof( db ) ) ){
		sql_failed = true;
		return false;
	}

	sql_handle = Sql_Malloc();
	if( SQL_ERROR == Sql_Connect( sql_handle, user, pass, host, (uint16)strtoul( port, nullptr, 10 ), db ) ){
		ShowError( "login_token: could not connect to the account database; one-time login tokens are refused.\n" );
		Sql_ShowDebug( sql_handle );
		Sql_Free( sql_handle );
		sql_handle = nullptr;
		sql_failed = true;
		return false;
	}
	if( accounts->get_property( accounts, "login_codepage", codepage, sizeof( codepage ) ) && codepage[0] != '\0'
		&& SQL_ERROR == Sql_SetEncoding( sql_handle, codepage ) )
		Sql_ShowDebug( sql_handle );
	return true;
}

bool login_token_shape( const char* passwd ){
	if( passwd == nullptr || passwd[0] != '~' )
		return false;
	size_t length = strnlen( passwd, PASSWD_LENGTH );
	if( length != 23 )
		return false;
	for( size_t i = 1; i < length; i++ ){
		char c = passwd[i];
		if( !( ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '-' || c == '_' ) )
			return false;
	}
	return true;
}

/// Equal, in time that depends only on the length. Both are 64 hex digits.
static bool login_token_equal( const char* a, const char* b, size_t length ){
	uint8 difference = 0;
	for( size_t i = 0; i < length; i++ )
		difference |= (uint8)( a[i] ^ b[i] );
	return difference == 0;
}

bool login_token_accept( AccountDB* accounts, struct login_session_data& sd, struct mmo_account& acc ){
	// <passwordencrypt> sends an MD5 of what was typed, never the token itself.
	if( sd.passwdenc != 0 || !login_token_shape( sd.passwd ) || !login_token_connect( accounts ) )
		return false;

	// The token is base64url and '~' only (checked above), so it needs no
	// escaping. The database hashes it; only this account's live tokens are
	// read, and each stored hash is compared here in constant time.
	if( SQL_ERROR == Sql_Query( sql_handle,
		"SELECT `id`, `token_hash`, SHA2('%s', 256) FROM `%s` WHERE `account_id` = '%u' AND `used` = '0' AND `expires` > NOW()",
		sd.passwd, login_token_table, acc.account_id ) ){
		Sql_ShowDebug( sql_handle );
		return false;
	}

	uint32 match = 0;
	char* data;
	size_t length;
	while( SQL_SUCCESS == Sql_NextRow( sql_handle ) ){
		char stored[65] = {}, presented[65] = {};
		uint32 id;

		Sql_GetData( sql_handle, 0, &data, nullptr );
		id = (uint32)strtoul( data, nullptr, 10 );
		Sql_GetData( sql_handle, 1, &data, &length );
		if( data == nullptr || length != 64 )
			continue;
		memcpy( stored, data, 64 );
		Sql_GetData( sql_handle, 2, &data, &length );
		if( data == nullptr || length != 64 )
			continue;
		memcpy( presented, data, 64 );

		if( login_token_equal( stored, presented, 64 ) && match == 0 )
			match = id;
	}
	Sql_FreeResult( sql_handle );

	if( match == 0 )
		return false;

	// Single use: whichever login marks it first gets in, any other gets nothing.
	if( SQL_ERROR == Sql_Query( sql_handle, "UPDATE `%s` SET `used` = '1' WHERE `id` = '%u' AND `used` = '0' AND `expires` > NOW()", login_token_table, match ) ){
		Sql_ShowDebug( sql_handle );
		return false;
	}
	if( Sql_NumRowsAffected( sql_handle ) != 1 )
		return false;

	ShowInfo( "login_token: account '%s' logged in with a one-time login token.\n", acc.userid );
	return true;
}

void login_token_final(){
	if( sql_handle != nullptr ){
		Sql_Free( sql_handle );
		sql_handle = nullptr;
	}
	sql_failed = false;
}
