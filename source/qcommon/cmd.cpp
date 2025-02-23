/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
#include <algorithm> // std::sort

#include "qcommon/qcommon.h"
#include "qcommon/array.h"
#include "qcommon/fs.h"
#include "qcommon/string.h"
#include "qcommon/q_trie.h"

#define MAX_ALIAS_NAME      64
#define ALIAS_LOOP_COUNT    16

struct cmd_alias_t {
	char *name;
	char *value;
	bool archive;
};

static bool cmd_preinitialized = false;
static bool cmd_initialized = false;

static trie_t *cmd_alias_trie = NULL;

static int alias_count;    // for detecting runaway loops

static int Cmd_PatternMatchesAlias( void *alias, const void *pattern ) {
	assert( alias );
	return !pattern || Com_GlobMatch( (const char *) pattern, ( (cmd_alias_t *) alias )->name, false );
}

/*
=============================================================================

COMMAND BUFFER

=============================================================================
*/

/*
* Command buffer is a cyclical dynamically allocated buffer
* It must never be totally full, since cbuf_text_head points to first free
* position in the buffer
*/

static bool cbuf_initialized = false;

#define MIN_CMD_TEXT_SIZE 1024

static size_t cbuf_text_size, cbuf_text_head, cbuf_text_tail;
static char *cbuf_text;
static mempool_t *cbuf_pool;

#define Cbuf_Malloc( size ) Mem_Alloc( cbuf_pool, size )
#define Cbuf_Free( data ) Mem_Free( data )

/*
* Cbuf_Init
*/
void Cbuf_Init() {
	assert( !cbuf_initialized );

	cbuf_pool = Mem_AllocPool( NULL, "Command buffer" );

	cbuf_text_size = MIN_CMD_TEXT_SIZE;
	cbuf_text = ( char * ) Cbuf_Malloc( cbuf_text_size );
	cbuf_text_head = 0;
	cbuf_text_tail = 0;

	cbuf_initialized = true;
}

/*
* Cbuf_Shutdown
*/
void Cbuf_Shutdown() {
	if( !cbuf_initialized ) {
		return;
	}

	Cbuf_Free( cbuf_text );
	cbuf_text = NULL;
	cbuf_text_size = 0;
	cbuf_text_head = 0;
	cbuf_text_tail = 0;

	Mem_FreePool( &cbuf_pool );

	cbuf_initialized = false;
}

/*
* Cbuf_FreeSpace
*
* Frees some space, if we have too big buffer in use
*/
static void Cbuf_FreeSpace() {
	char *old;
	size_t used, old_size;

	if( cbuf_text_head >= cbuf_text_tail ) {
		used = cbuf_text_head - cbuf_text_tail;
	} else {
		used = cbuf_text_size - cbuf_text_tail + cbuf_text_head;
	}

	if( used < cbuf_text_size / 2 && used < cbuf_text_size - MIN_CMD_TEXT_SIZE ) {
		old = cbuf_text;
		old_size = cbuf_text_size;

		cbuf_text_size = used + MIN_CMD_TEXT_SIZE;
		cbuf_text = ( char * ) Cbuf_Malloc( cbuf_text_size );

		if( cbuf_text_head >= cbuf_text_tail ) {
			memcpy( cbuf_text, old + cbuf_text_tail, used );
		} else {
			memcpy( cbuf_text, old + cbuf_text_tail, old_size - cbuf_text_tail );
			memcpy( cbuf_text + ( old_size - cbuf_text_tail ), old, cbuf_text_head );
		}
		cbuf_text_tail = 0;
		cbuf_text_head = used;

		Cbuf_Free( old );
	}
}

/*
* Cbuf_EnsureSpace
*/
static void Cbuf_EnsureSpace( size_t size ) {
	size_t free;
	size_t diff;

	if( cbuf_text_head >= cbuf_text_tail ) {
		free = cbuf_text_size - cbuf_text_head + cbuf_text_tail;
	} else {
		free = cbuf_text_tail - cbuf_text_head;
	}

	if( free >= size ) {
		return;
	}

	diff = ( size - free ) + MIN_CMD_TEXT_SIZE;
	cbuf_text_size += diff;
	cbuf_text = ( char * ) Mem_Realloc( cbuf_text, cbuf_text_size );

	if( cbuf_text_head < cbuf_text_tail ) {
		memmove( cbuf_text + cbuf_text_tail + diff, cbuf_text + cbuf_text_tail, cbuf_text_size - diff - cbuf_text_tail );
		cbuf_text_tail += diff;
	}
}

/*
* Cbuf_AddText
*
* Adds command text at the end of the buffer
*/
void Cbuf_AddText( const char *text ) {
	size_t textlen = strlen( text );

	Cbuf_EnsureSpace( textlen );

	if( cbuf_text_size - cbuf_text_head < textlen ) {
		size_t endsize = cbuf_text_size - cbuf_text_head;

		memcpy( cbuf_text + cbuf_text_head, text, endsize );
		memcpy( cbuf_text, text + endsize, textlen - endsize );
		cbuf_text_head = textlen - endsize;
	} else {
		memcpy( cbuf_text + cbuf_text_head, text, textlen );
		cbuf_text_head += textlen;
		if( cbuf_text_head == cbuf_text_size ) {
			cbuf_text_head = 0;
		}
	}
}

/*
* Cbuf_InsertText
*
* When a command wants to issue other commands immediately, the text is
* inserted at the beginning of the buffer, before any remaining unexecuted
* commands.
* Adds a \n to the text
*/
static void Cbuf_InsertText( const char *text ) {
	size_t textlen = strlen( text );

	Cbuf_EnsureSpace( textlen );

	if( cbuf_text_tail < textlen ) {
		memcpy( cbuf_text + cbuf_text_size - ( textlen - cbuf_text_tail ), text, textlen - cbuf_text_tail );
		memcpy( cbuf_text, text + textlen - cbuf_text_tail, cbuf_text_tail );
		cbuf_text_tail = cbuf_text_size - ( textlen - cbuf_text_tail );
	} else {
		memcpy( cbuf_text + cbuf_text_tail - textlen, text, textlen );
		cbuf_text_tail -= textlen;
	}
}


/*
* Cbuf_ExecuteText
* This can be used in place of either Cbuf_AddText or Cbuf_InsertText
*/
void Cbuf_ExecuteText( int exec_when, const char *text ) {
	switch( exec_when ) {
		case EXEC_NOW:
			Cmd_ExecuteString( text );
			break;
		case EXEC_APPEND:
			Cbuf_AddText( text );
			break;
		default:
			Com_Error( ERR_FATAL, "Cbuf_ExecuteText: bad exec_when" );
	}
}

/*
* Cbuf_Execute
* Pulls off \n terminated lines of text from the command buffer and sends
* them through Cmd_ExecuteString.  Stops when the buffer is empty.
* Normally called once per frame, but may be explicitly invoked.
* Do not call inside a command function!
*/
void Cbuf_Execute() {
	size_t i;
	int c;
	char line[MAX_STRING_CHARS];
	bool quotes, quoteskip;

	alias_count = 0;    // don't allow infinite alias loops

	while( cbuf_text_tail != cbuf_text_head ) {
		// find a \n or ; line break
		i = 0;
		quotes = false;
		quoteskip = false;
		while( cbuf_text_tail != cbuf_text_head && i < sizeof( line ) - 1 ) {
			c = cbuf_text[cbuf_text_tail];

			if( !quoteskip && c == '"' ) {
				quotes = !quotes;
			}

			if( !quoteskip && c == '\\' ) {
				quoteskip = true;
			} else {
				quoteskip = false;
			}

			line[i] = c;

			cbuf_text_tail = ( cbuf_text_tail + 1 ) % cbuf_text_size;

			if( ( c == '\n' ) || ( !quotes && c == ';' ) ) {
				break;
			}

			i++;
		}
		line[i] = 0;

		// execute the command line
		Cmd_ExecuteString( line );
	}

	Cbuf_FreeSpace();
}

/*
* Cbuf_AddEarlyCommands
*
* Adds all the +set commands from the command line:
*
* Adds command line parameters as script statements
* Commands lead with a +, and continue until another +
*
* Set/exec/config commands are added early, so they are guaranteed to be set before
* the client and server initialize for the first time.
*
* This command is first run before autoexec.cfg and config.cfg to allow changing
* fs_basepath etc. The second run is after those files has been execed in order
* to allow overwriting values set in them.
*
* Other commands are added late, after all initialization is complete.
*/
void Cbuf_AddEarlyCommands( bool second_run ) {
	int i;
	const char *s;

	for( i = 1; i < COM_Argc(); ++i ) {
		s = COM_Argv( i );
		if( !Q_stricmp( s, "+set" ) ) {
			if( strlen( s ) > 4 ) {
				Cbuf_AddText( va( "\"set%s\" \"%s\" \"%s\"\n", s + 4, COM_Argv( i + 1 ), COM_Argv( i + 2 ) ) );
			} else {
				Cbuf_AddText( va( "\"set\" \"%s\" \"%s\"\n", COM_Argv( i + 1 ), COM_Argv( i + 2 ) ) );
			}
			if( second_run ) {
				COM_ClearArgv( i );
				COM_ClearArgv( i + 1 );
				COM_ClearArgv( i + 2 );
			}
			i += 2;
		} else if( second_run && !Q_stricmp( s, "+exec" ) ) {
			Cbuf_AddText( va( "exec \"%s\"\n", COM_Argv( i + 1 ) ) );
			COM_ClearArgv( i );
			COM_ClearArgv( i + 1 );
			i += 1;
		} else if( second_run && !Q_stricmp( s, "+config" ) ) {
			Cbuf_AddText( va( "config \"%s\"\n", COM_Argv( i + 1 ) ) );
			COM_ClearArgv( i );
			COM_ClearArgv( i + 1 );
			i += 1;
		}
	}
}

/*
* Cbuf_AddLateCommands
*
* Adds command line parameters as script statements
* Commands lead with a + and continue until another + or -
* quake +map amlev1
*
* Returns true if any late commands were added, which
* will keep the demoloop from immediately starting
*/
bool Cbuf_AddLateCommands() {
	int i;
	size_t text_size;
	char *text;

	// build the combined string to parse from
	text_size = 0;
	for( i = 1; i < COM_Argc(); i++ ) {
		if( COM_Argv( i )[0] == 0 ) {
			continue;
		}
		text_size += strlen( COM_Argv( i ) ) + 2 /* quatation marks */ + 1 /* space */;
	}
	if( text_size == 0 ) {
		return false;
	}

	text_size += 2; // '\n' and '\0' at the end
	text = ( char * ) Cbuf_Malloc( (int)text_size );
	text[0] = 0;
	for( i = 1; i < COM_Argc(); i++ ) {
		if( COM_Argv( i )[0] == 0 ) {
			continue;
		}
		if( COM_Argv( i )[0] == '+' ) {
			Q_strncatz( text, "\n", text_size );
			Q_strncatz( text, va( "\"%s\"", COM_Argv( i ) + 1 ), text_size );
		} else {
			Q_strncatz( text, va( "\"%s\"", COM_Argv( i ) ), text_size );
		}
		Q_strncatz( text, " ", text_size );
	}
	Q_strncatz( text, "\n", text_size );

	Cbuf_AddText( text );
	Cbuf_Free( text );

	return true;
}


/*
==============================================================================

SCRIPT COMMANDS

==============================================================================
*/

static void ExecConfig( const char * path ) {
	char * config = ReadFileString( sys_allocator, path );
	defer { FREE( sys_allocator, config ); };
	if( config == NULL ) {
		Com_Printf( "Couldn't execute: %s\n", path );
		return;
	}

	Cbuf_InsertText( "\n" );
	Cbuf_InsertText( config );
	Cbuf_InsertText( "\n" );
}

static void Cmd_Exec_f() {
	if( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: exec <filename>\n" );
		return;
	}

	DynamicString path( sys_allocator, "{}/base/{}", HomeDirPath(), Cmd_Argv( 1 ) );
	if( FileExtension( path.c_str() ) == "" ) {
		path += ".cfg";
	}

	ExecConfig( path.c_str() );
}

static void Cmd_Config_f() {
	if( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: config <filename>\n" );
		return;
	}

	DynamicString path( sys_allocator, "{}", Cmd_Argv( 1 ) );
	if( FileExtension( path.c_str() ) == "" ) {
		path += ".cfg";
	}

	ExecConfig( path.c_str() );
}

void ExecDefaultCfg() {
	DynamicString path( sys_allocator, "{}/base/default.cfg", RootDirPath() );
	ExecConfig( path.c_str() );
}

static const char **CL_CompleteExecBuildList( const char *partial ) {
	return Cmd_CompleteHomeDirFileList( partial, "base", ".cfg" );
}

/*
* Cmd_AliasList_f
*/
static void Cmd_AliasList_f() {
	char *pattern;
	unsigned int size;
	unsigned int i;
	trie_dump_t *dump = NULL;

	assert( cmd_alias_trie );

	Trie_GetSize( cmd_alias_trie, &size );
	if( !size ) {
		Com_Printf( "No alias commands\n" );
		return;
	}

	if( Cmd_Argc() == 1 ) {
		pattern = NULL; // no wildcard
	} else {
		pattern = Cmd_Args();
	}

	Com_Printf( "\nAlias commands:\n" );
	Trie_DumpIf( cmd_alias_trie, "", TRIE_DUMP_VALUES, Cmd_PatternMatchesAlias, pattern, &dump );
	for( i = 0; i < dump->size; ++i ) {
		cmd_alias_t *const a = (cmd_alias_t *) dump->key_value_vector[i].value;
		Com_Printf( "%s : %s\n", a->name, a->value );
	}
	Trie_FreeDump( dump );
	Com_Printf( "%i commands\n", i );
}

/*
* Cmd_Alias_f
*
* Creates a new command that executes a command string (possibly ; separated)
*/
static void Cmd_Alias_f_( bool archive ) {
	cmd_alias_t *a;
	char cmd[1024];
	int i, c;
	size_t len;
	const char *s;

	if( Cmd_Argc() == 1 ) {
		Com_Printf( "usage: alias <name> <command>\n" );
		return;
	}

	s = Cmd_Argv( 1 );
	len = strlen( s );
	if( len >= MAX_ALIAS_NAME ) {
		Com_Printf( "Alias name is too long\n" );
		return;
	}

	assert( cmd_alias_trie );
	Trie_Find( cmd_alias_trie, s, TRIE_EXACT_MATCH, (void **)&a );
	if( a ) {
		if( Cmd_Argc() == 2 ) {
			if( archive ) {
				a->archive = true;
			}
			Com_Printf( "alias \"%s\" is \"%s" S_COLOR_WHITE "\"\n", a->name, a->value );
			return;
		}
		Mem_ZoneFree( a->value );
	} else {
		a = ( cmd_alias_t * ) Mem_ZoneMalloc( (int) ( sizeof( cmd_alias_t ) + len + 1 ) );
		a->name = (char *) ( (uint8_t *)a + sizeof( cmd_alias_t ) );
		strcpy( a->name, s );
		Trie_Insert( cmd_alias_trie, s, a );
	}

	if( archive ) {
		a->archive = true;
	}

	// copy the rest of the command line
	cmd[0] = 0; // start out with a null string
	c = Cmd_Argc();
	for( i = 2; i < c; i++ ) {
		Q_strncatz( cmd, Cmd_Argv( i ), sizeof( cmd ) );
		if( i != ( c - 1 ) ) {
			Q_strncatz( cmd, " ", sizeof( cmd ) );
		}
	}

	a->value = ZoneCopyString( cmd );
}

/*
* Cmd_Alias_f
*/
static void Cmd_Alias_f() {
	Cmd_Alias_f_( false );
}

/*
* Cmd_Aliasa_f
*/
static void Cmd_Aliasa_f() {
	Cmd_Alias_f_( true );
}

/*
* Cmd_Unalias_f
*
* Removes an alias command
*/
static void Cmd_Unalias_f() {
	const char *s;
	cmd_alias_t *a;

	if( Cmd_Argc() == 1 ) {
		Com_Printf( "usage: unalias <name>\n" );
		return;
	}

	s = Cmd_Argv( 1 );
	if( strlen( s ) >= MAX_ALIAS_NAME ) {
		Com_Printf( "Alias name is too long\n" );
		return;
	}

	assert( cmd_alias_trie );
	if( Trie_Remove( cmd_alias_trie, s, (void **)&a ) == TRIE_OK ) {
		Mem_ZoneFree( a->value );
		Mem_ZoneFree( a );
	} else {
		Com_Printf( "Cmd_Unalias_f: %s not added\n", s );
	}
}

/*
* Cmd_UnaliasAll_f
*
* Removes an alias command
*/
static void Cmd_UnaliasAll_f() {
	trie_dump_t *dump;
	unsigned int i;

	assert( cmd_alias_trie );
	Trie_Dump( cmd_alias_trie, "", TRIE_DUMP_VALUES, &dump );
	for( i = 0; i < dump->size; ++i ) {
		cmd_alias_t *const a = (cmd_alias_t *) dump->key_value_vector[i].value;
		Mem_ZoneFree( a->value );
		Mem_ZoneFree( a );
	}
	Trie_FreeDump( dump );
	Trie_Clear( cmd_alias_trie );
}

/*
=============================================================================

COMMAND EXECUTION

=============================================================================
*/

struct cmd_function_t {
	char *name;
	xcommand_t function;
	xcompletionf_t completion_func;
};

static int cmd_argc;
static char *cmd_argv[MAX_STRING_TOKENS];
static size_t cmd_argv_sizes[MAX_STRING_TOKENS];
static char cmd_null_string[] = "";
static char cmd_args[MAX_STRING_CHARS];

static trie_t *cmd_function_trie = NULL;

static int Cmd_PatternMatchesFunction( void *cmd, const void *pattern ) {
	assert( cmd );
	return !pattern || Com_GlobMatch( (const char *) pattern, ( (cmd_function_t *) cmd )->name, false );
}

// The functions that execute commands get their parameters with these
// functions. Cmd_Argv () will return an empty string, not a NULL
// if arg > argc, so string operations are always safe.

/*
* Cmd_Argc
*/
int Cmd_Argc() {
	return cmd_argc;
}

/*
* Cmd_Argv
*/
const char *Cmd_Argv( int arg ) {
	if( arg >= cmd_argc ) {
		return cmd_null_string;
	}
	return cmd_argv[arg];
}

/*
* Cmd_Args
*
* Returns a single string containing argv(1) to argv(argc()-1)
*/
char *Cmd_Args() {
	return cmd_args;
}

/*
* Cmd_TokenizeString
*
* Parses the given string into command line tokens.
* $Cvars will be expanded unless they are in a quoted token
* Takes a null terminated string.  Does not need to be /n terminated.
*/
void Cmd_TokenizeString( const char *text ) {
	cmd_argc = 0;
	cmd_args[0] = 0;

	if( !text ) {
		return;
	}

	while( true ) {
		// skip whitespace up to a /n
		while( *text && (unsigned char)*text <= ' ' && *text != '\n' )
			text++;

		if( *text == '\n' ) {
			// a newline separates commands in the buffer
			text++;
			break;
		}

		if( !*text ) {
			return;
		}

		// set cmd_args to everything after the first arg
		if( cmd_argc == 1 ) {
			size_t l;

			strncpy( cmd_args, text, sizeof( cmd_args ) - 1 );
			cmd_args[sizeof( cmd_args ) - 1] = 0;

			// strip off any trailing whitespace
			// use > 0 and -1 instead of >= 0 since size_t can be unsigned
			l = strlen( cmd_args );
			for(; l > 0; l-- )
				if( (unsigned char)cmd_args[l - 1] <= ' ' ) {
					cmd_args[l - 1] = 0;
				} else {
					break;
				}
		}

		Span< const char > token = ParseToken( &text, Parse_StopOnNewLine );
		if( token.ptr == NULL || token == "//" )
			return;

		if( cmd_argc < MAX_STRING_TOKENS ) {
			if( cmd_argv_sizes[cmd_argc] < token.n + 1 ) {
				size_t new_size = Min2( token.n + 64, size_t( MAX_TOKEN_CHARS ) );
				cmd_argv[cmd_argc] = REALLOC_MANY( sys_allocator, char, cmd_argv[cmd_argc], cmd_argv_sizes[cmd_argc], new_size );
				cmd_argv_sizes[cmd_argc] = new_size;
			}
			memcpy( cmd_argv[cmd_argc], token.ptr, token.n );
			cmd_argv[cmd_argc][token.n] = '\0';
			cmd_argc++;
		}
	}
}


/*
* Cmd_AddCommand
* called by the init functions of other parts of the program to
* register commands and functions to call for them.
* The cmd_name is referenced later, so it should not be in temp memory
* if function is NULL, the command will be forwarded to the server
* as a clc_clientcommand instead of executed locally
*/
void Cmd_AddCommand( const char *cmd_name, xcommand_t function ) {
	cmd_function_t *cmd;

	if( !cmd_name || !cmd_name[0] ) {
		Com_DPrintf( "Cmd_AddCommand: empty name pass as an argument\n" );
		return;
	}

	// fail if the command is a variable name
	if( Cvar_String( cmd_name )[0] ) {
		Com_Printf( "Cmd_AddCommand: %s already defined as a var\n", cmd_name );
		return;
	}

	// fail if the command already exists
	assert( cmd_function_trie );
	assert( cmd_name );
	if( Trie_Find( cmd_function_trie, cmd_name, TRIE_EXACT_MATCH, (void **)&cmd ) == TRIE_OK ) {
		cmd->function = function;
		cmd->completion_func = NULL;
		Com_DPrintf( "Cmd_AddCommand: %s already defined\n", cmd_name );
		return;
	}

	cmd = ( cmd_function_t * ) Mem_ZoneMalloc( (int)( sizeof( cmd_function_t ) + strlen( cmd_name ) + 1 ) );
	cmd->name = (char *) ( (uint8_t *)cmd + sizeof( cmd_function_t ) );
	strcpy( cmd->name, cmd_name );
	cmd->function = function;
	cmd->completion_func = NULL;
	Trie_Insert( cmd_function_trie, cmd_name, cmd );
}

/*
* Cmd_RemoveCommand
*/
void Cmd_RemoveCommand( const char *cmd_name ) {
	cmd_function_t *cmd;

	if( !cmd_initialized ) {
		return;
	}

	assert( cmd_function_trie );
	assert( cmd_name );
	if( Trie_Remove( cmd_function_trie, cmd_name, (void **)&cmd ) == TRIE_OK ) {
		Mem_ZoneFree( cmd );
	} else {
		Com_Printf( "Cmd_RemoveCommand: %s not added\n", cmd_name );
	}
}

/*
* Cmd_Exists
* used by the cvar code to check for cvar / command name overlap
*/
bool Cmd_Exists( const char *cmd_name ) {
	cmd_function_t *cmd;

	assert( cmd_function_trie );
	assert( cmd_name );
	return Trie_Find( cmd_function_trie, cmd_name, TRIE_EXACT_MATCH, (void **)&cmd ) == TRIE_OK;
}

/*
* Cmd_SetCompletionFunc
*/
void Cmd_SetCompletionFunc( const char *cmd_name, xcompletionf_t completion_func ) {
	cmd_function_t *cmd;

	if( !cmd_name || !cmd_name[0] ) {
		Com_DPrintf( "Cmd_SetCompletionFunc: empty name pass as an argument\n" );
		return;
	}

	if( Trie_Find( cmd_function_trie, cmd_name, TRIE_EXACT_MATCH, (void **)&cmd ) == TRIE_OK ) {
		cmd->completion_func = completion_func;
		return;
	}

	Com_DPrintf( "Cmd_SetCompletionFunc: %s already does not exist\n", cmd_name );
}

/*
* Cmd_CompleteCountPossible
*/
int Cmd_CompleteCountPossible( const char *partial ) {
	assert( partial );
	if( !partial[0] ) {
		return 0;
	} else {
		unsigned int matches;
		assert( cmd_function_trie );
		Trie_NoOfMatches( cmd_function_trie, partial, &matches );
		return matches;
	}
}

/*
* Cmd_CompleteBuildList
*/
const char **Cmd_CompleteBuildList( const char *partial ) {
	trie_dump_t *dump;
	const char **buf;
	unsigned int i;

	assert( cmd_function_trie );
	assert( partial );
	Trie_Dump( cmd_function_trie, partial, TRIE_DUMP_VALUES, &dump );
	buf = (const char **) Mem_TempMalloc( sizeof( char * ) * ( dump->size + 1 ) );
	for( i = 0; i < dump->size; ++i )
		buf[i] = ( (cmd_function_t *) ( dump->key_value_vector[i].value ) )->name;
	buf[dump->size] = NULL;
	Trie_FreeDump( dump );
	return buf;
}

/*
* Cmd_CompleteBuildArgListExt
*
* Find a possible single matching command
*/
const char **Cmd_CompleteBuildArgListExt( const char *command, const char *arguments ) {
	cmd_function_t *cmd = NULL;

	if( Trie_Find( cmd_function_trie, command, TRIE_EXACT_MATCH, (void **)&cmd ) != TRIE_OK ) {
		return NULL;
	}
	if( cmd->completion_func ) {
		return cmd->completion_func( arguments );
	}
	return NULL;
}

/*
* Cmd_CompleteBuildArgList
*
* Find a possible single matching command
*/
const char **Cmd_CompleteBuildArgList( const char *partial ) {
	const char *p;

	p = strstr( partial, " " );
	if( p ) {
		Cmd_TokenizeString( partial );
		return Cmd_CompleteBuildArgListExt( cmd_argv[0], cmd_args );
	}

	return NULL;
}

static void AddMatchingFilesRecursive( DynamicArray< char * > * files, DynamicString * path, Span< const char > prefix, size_t skip, const char * extension ) {
	ListDirHandle scan = BeginListDir( sys_allocator, path->c_str() );

	const char * name;
	bool dir;
	while( ListDirNext( &scan, &name, &dir ) ) {
		// skip ., .., .git, etc
		if( name[ 0 ] == '.' )
			continue;

		size_t old_len = path->length();
		path->append( "/{}", name );
		if( dir ) {
			AddMatchingFilesRecursive( files, path, prefix, skip, extension );
		}
		else {
			bool prefix_matches = path->length() >= prefix.n + skip && Q_strnicmp( path->c_str() + skip, prefix.ptr, prefix.n ) == 0;
			bool ext_matches = StrCaseEqual( FileExtension( path->c_str() ), extension );
			if( prefix_matches && ext_matches ) {
				files->add( ( *sys_allocator )( "{}", path->span().slice( skip, path->length() ) ) );
			}
		}
		path->truncate( old_len );
	}
}

const char ** Cmd_CompleteHomeDirFileList( const char * partial, const char * search_dir, const char * extension ) {
	DynamicString base_path( sys_allocator, "{}/{}", HomeDirPath(), search_dir );
	size_t skip = base_path.length();
	base_path += BasePath( partial );

	Span< const char > prefix = FileName( partial );

	DynamicArray< char * > files( sys_allocator );

	AddMatchingFilesRecursive( &files, &base_path, prefix, skip + 1, extension );

	std::sort( files.begin(), files.end(), SortCStringsComparator );

	size_t filenames_buffer_size = 0;
	for( const char * file : files ) {
		filenames_buffer_size += strlen( file ) + 1;
	}

	size_t pointers_buffer_size = ( files.size() + 1 ) * sizeof( char * ); // + 1 for trailing NULL
	size_t combined_buffer_size =  pointers_buffer_size + filenames_buffer_size;

	void * combined = Mem_TempMalloc( combined_buffer_size );
	char ** pointers = ( char ** ) combined;
	char * filenames = ( char * ) combined + pointers_buffer_size;

	size_t filenames_cursor = 0;
	for( size_t i = 0; i < files.size(); i++ ) {
		char * file = files[ i ];

		pointers[ i ] = filenames + filenames_cursor;
		memcpy( pointers[ i ], file, strlen( file ) + 1 );
		filenames_cursor += strlen( file ) + 1;

		FREE( sys_allocator, file );
	}

	pointers[ files.size() ] = NULL;

	return ( const char ** ) combined;
}

/*
Cmd_CompleteAlias
*/
char *Cmd_CompleteAlias( const char *partial ) {
	size_t len;
	cmd_alias_t *a;

	assert( partial );
	len = strlen( partial );
	assert( cmd_alias_trie );
	if( len && ( Trie_Find( cmd_alias_trie, partial, TRIE_PREFIX_MATCH, (void **)&a ) == TRIE_OK ) ) {
		return a->name;
	} else {
		return NULL;
	}
}

/*
Cmd_CompleteAliasCountPossible
*/
int Cmd_CompleteAliasCountPossible( const char *partial ) {
	assert( partial );
	if( !strlen( partial ) ) {
		return 0;
	} else {
		unsigned int matches;
		assert( cmd_alias_trie );
		Trie_NoOfMatches( cmd_alias_trie, partial, &matches );
		return matches;
	}
}

/*
Cmd_CompleteAliasBuildList
*/
const char **Cmd_CompleteAliasBuildList( const char *partial ) {
	trie_dump_t *dump;
	const char **buf;
	unsigned int i;

	assert( cmd_alias_trie );
	assert( partial );
	Trie_Dump( cmd_alias_trie, partial, TRIE_DUMP_VALUES, &dump );
	buf = (const char **) Mem_TempMalloc( sizeof( char * ) * ( dump->size + 1 ) );
	for( i = 0; i < dump->size; ++i )
		buf[i] = ( (cmd_alias_t *) ( dump->key_value_vector[i].value ) )->name;
	buf[dump->size] = NULL;
	Trie_FreeDump( dump );
	return buf;
}

/*
* Cmd_CheckForCommand
*
* Used by console code to check if text typed is a command/cvar/alias or chat
*/
bool Cmd_CheckForCommand( char *text ) {
	char cmd[MAX_STRING_CHARS];
	cmd_alias_t *a;
	int i;

	// this is not exactly what cbuf does when extracting lines
	// for execution, but it works unless you do weird things like
	// putting the command in quotes
	for( i = 0; i < MAX_STRING_CHARS - 1; i++ )
		if( (unsigned char)text[i] <= ' ' || text[i] == ';' ) {
			break;
		} else {
			cmd[i] = text[i];
		}
	cmd[i] = 0;

	if( Cmd_Exists( cmd ) ) {
		return true;
	}
	if( Cvar_Find( cmd ) ) {
		return true;
	}
	if( Trie_Find( cmd_alias_trie, cmd, TRIE_EXACT_MATCH, (void **)&a ) == TRIE_OK ) {
		return true;
	}

	return false;
}

/*
* Cmd_ExecuteString
* Parses a single line of text into arguments and tries to execute it
* as if it was typed at the console
* FIXME: lookupnoadd the token to speed search?
*/
void Cmd_ExecuteString( const char *text ) {
	char *str;
	cmd_function_t *cmd;
	cmd_alias_t *a;

	Cmd_TokenizeString( text );

	// execute the command line
	if( !Cmd_Argc() ) {
		return; // no tokens

	}
	str = cmd_argv[0];

	// FIXME: This routine defines the order in which identifiers are looked-up, but
	// there are no checks for name-clashes. If a user sets a cvar with the name of
	// an existing command or alias, that cvar becomes shadowed!
	// We need a global namespace data-structure and a way to check for name-clashes
	// that does not break seperation of concerns.
	// Aiwa, 07-14-2006

	assert( cmd_function_trie );
	assert( cmd_alias_trie );
	if( Trie_Find( cmd_function_trie, str, TRIE_EXACT_MATCH, (void **)&cmd ) == TRIE_OK ) {
		// check functions
		if( !cmd->function ) {
			// forward to server command
			Cmd_ExecuteString( va( "cmd %s", text ) );
		} else {
			cmd->function();
		}
	} else if( Trie_Find( cmd_alias_trie, str, TRIE_EXACT_MATCH, (void **)&a ) == TRIE_OK ) {
		// check alias
		if( ++alias_count == ALIAS_LOOP_COUNT ) {
			Com_Printf( "ALIAS_LOOP_COUNT\n" );
			alias_count = 0;
			return;
		}
		Cbuf_InsertText( "\n" );
		Cbuf_InsertText( a->value );
	} else if( Cvar_Command() ) {
		// check cvars
	} else {
		Com_Printf( "Unknown command \"%s" S_COLOR_WHITE "\"\n", str );
	}
}

/*
* Cmd_List_f
*/
static void Cmd_List_f() {
	trie_dump_t *dump = NULL;
	unsigned int i;
	char *pattern;

	if( Cmd_Argc() == 1 ) {
		pattern = NULL; // no wildcard
	} else {
		pattern = Cmd_Args();
	}

	Com_Printf( "\nCommands:\n" );
	assert( cmd_function_trie );
	Trie_DumpIf( cmd_function_trie, "", TRIE_DUMP_VALUES, Cmd_PatternMatchesFunction, pattern, &dump );
	for( i = 0; i < dump->size; ++i ) {
		cmd_function_t *const cmd = (cmd_function_t *) dump->key_value_vector[i].value;
		Com_Printf( "%s\n", cmd->name );
	}
	Trie_FreeDump( dump );
	Com_Printf( "%i commands\n", i );
}

/*
* Cmd_PreInit
*/
void Cmd_PreInit() {
	assert( !cmd_preinitialized );
	assert( !cmd_initialized );

	assert( !cmd_alias_trie );
	assert( !cmd_function_trie );

	Trie_Create( TRIE_CASE_INSENSITIVE, &cmd_alias_trie );
	Trie_Create( TRIE_CASE_INSENSITIVE, &cmd_function_trie );

	cmd_preinitialized = true;
}

/*
* Cmd_Init
*/
void Cmd_Init() {
	assert( !cmd_initialized );
	assert( cmd_preinitialized );

	assert( cmd_alias_trie );
	assert( cmd_function_trie );

	//
	// register our commands
	//
	Cmd_AddCommand( "cmdlist", Cmd_List_f );
	Cmd_AddCommand( "exec", Cmd_Exec_f );
	Cmd_AddCommand( "config", Cmd_Config_f );
	Cmd_AddCommand( "aliaslist", Cmd_AliasList_f );
	Cmd_AddCommand( "aliasa", Cmd_Aliasa_f );
	Cmd_AddCommand( "unalias", Cmd_Unalias_f );
	Cmd_AddCommand( "unaliasall", Cmd_UnaliasAll_f );
	Cmd_AddCommand( "alias", Cmd_Alias_f );

	Cmd_SetCompletionFunc( "alias", Cmd_CompleteAliasBuildList );
	Cmd_SetCompletionFunc( "aliasa", Cmd_CompleteAliasBuildList );
	Cmd_SetCompletionFunc( "unalias", Cmd_CompleteAliasBuildList );
	Cmd_SetCompletionFunc( "exec", CL_CompleteExecBuildList );

	cmd_initialized = true;
}

void Cmd_Shutdown() {
	if( cmd_initialized ) {
		unsigned int i;
		trie_dump_t *dump;

		assert( cmd_alias_trie );
		assert( cmd_function_trie );

		Cmd_RemoveCommand( "cmdlist" );
		Cmd_RemoveCommand( "exec" );
		Cmd_RemoveCommand( "config" );
		Cmd_RemoveCommand( "aliaslist" );
		Cmd_RemoveCommand( "aliasa" );
		Cmd_RemoveCommand( "unalias" );
		Cmd_RemoveCommand( "unaliasall" );
		Cmd_RemoveCommand( "alias" );

		// this is somewhat ugly IMO
		for( i = 0; i < MAX_STRING_TOKENS && cmd_argv_sizes[i]; i++ ) {
			FREE( sys_allocator, cmd_argv[i] );
			cmd_argv_sizes[i] = 0;
		}

		Trie_Dump( cmd_function_trie, "", TRIE_DUMP_VALUES, &dump );
		for( i = 0; i < dump->size; ++i ) {
#ifndef PUBLIC_BUILD
			Com_Printf( "Warning: Command %s was never removed\n",
						( (cmd_function_t *)dump->key_value_vector[i].value )->name );
#endif
			Cmd_RemoveCommand( ( (cmd_function_t *)dump->key_value_vector[i].value )->name );
		}
		Trie_FreeDump( dump );

		cmd_initialized = false;
	}

	if( cmd_preinitialized ) {
		assert( cmd_alias_trie );
		assert( cmd_function_trie );

		Trie_Destroy( cmd_alias_trie );
		cmd_alias_trie = NULL;
		Trie_Destroy( cmd_function_trie );
		cmd_function_trie = NULL;

		cmd_preinitialized = false;
	}
}
