// kate: replace-tabs off; space-indent off;

/**
 * @mainpage libinotifytools
 *
 * libinotifytools is a small C library to simplify the use of Linux's inotify
 * interface.
 *
 * @link inotifytools/inotifytools.h Documentation for the library's public
 * interface.@endlink
 *
 * @link todo.html TODO list.@endlink
 */

#include "../../config.h"
#include "inotifytools/inotifytools.h"
#include "inotifytools_p.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <regex.h>
#include <setjmp.h>

#include "inotifytools/inotify.h"
static int dir_count = 0;
#define MAX_ENTRIES 10000
#define _GNU_SOURCE
#include <search.h>

static char *dir_stack[MAX_ENTRIES];
static int hcreate_return_value;
static struct hsearch_data tab = {0};


#define NIL (-1L)

void hadd(struct hsearch_data *tab, char *key, long value)
{
    ENTRY item = {key, (void *) value};
    ENTRY *pitem = &item;

    if (hsearch_r(item, ENTER, &pitem, tab)) {
        pitem->data = (void *) value;
    }
}

void hdelete(struct hsearch_data *tab, char *key)
{
    ENTRY item = {key};
    ENTRY *pitem = &item;

    if (hsearch_r(item, FIND, &pitem, tab)) {
        pitem->data = (void *) NIL;
    }
}

long hfind(struct hsearch_data *tab, char *key)
{
    ENTRY item = {key};
    ENTRY *pitem = &item;

    printf("==>searching for %s\n", key);
    
    if (hsearch_r(item, FIND, &pitem, tab)) {
	printf("==>success\n");
        return (long) pitem->data;
    } else {
	printf("==>fail\n");
    }
    return NIL;
}

/**
 * @file inotifytools/inotifytools.h
 * @brief inotifytools library public interface.
 * @author Rohan McGovern, \<rohan@mcgovern.id.au\>
 *
 * This library provides a thin layer on top of the basic inotify interface.
 * The primary use is to easily set up watches on files, potentially many files
 * at once, and read events without having to deal with low-level I/O.  There
 * are also several utility functions for inotify-related string formatting.
 *
 * To use this library, you must \c \#include the following headers accordingly:
 * \li \c \<inotifytools/inotifytools.h\> - to use any functions declared in
 *     this file.
 * \li \c \<inotifytools/inotify.h\> - to have the \c inotify_event type defined
 *     and the numeric IN_* event constants defined.   If \c \<sys/inotify.h\>
 *     was present on your system at compile time, this header simply includes
 *     that.  Otherwise it includes \c \<inotifytools/inotify-nosys.h\>.
 *
 * @section example Example
 * This very simple program recursively watches the entire directory tree
 * under its working directory for events, then prints them out with a
 * timestamp.
 * @include example.c
 *
 * @section events Events
 *
 * @note This section comes almost verbatim from the inotify(7) man page.
 *
 * @warning The information here applies to inotify in Linux 2.6.17.  Older
 *          versions of Linux may not support all the events described here.
 *
 * The following numeric events can be specified to functions in inotifytools,
 * and may be present in events returned through inotifytools:
 *
 * \li \c IN_ACCESS     -     File was accessed (read) \a *
 * \li \c IN_ATTRIB     -     Metadata changed (permissions, timestamps,
 *                            extended attributes, etc.) \a *
 * \li \c IN_CLOSE_WRITE -    File opened for writing was closed \a *
 * \li \c IN_CLOSE_NOWRITE -   File not opened for writing was closed \a *
 * \li \c IN_CREATE       -   File/directory created in watched directory \a *
 * \li \c IN_DELETE       -   File/directory deleted from watched directory \a *
 * \li \c IN_DELETE_SELF  -   Watched file/directory was itself deleted
 * \li \c IN_MODIFY       -   File was modified \a *
 * \li \c IN_MOVE_SELF    -   Watched file/directory was itself moved
 * \li \c IN_MOVED_FROM   -   File moved out of watched directory \a *
 * \li \c IN_MOVED_TO     -   File moved into watched directory \a *
 * \li \c IN_OPEN         -   File was opened \a *
 *
 * When monitoring a directory, the events marked with an asterisk \a * above
 * can  occur  for files  in the directory, in which case the name field in the
 * returned inotify_event structure identifies the name of the file within the
 * directory.
 *
 * The IN_ALL_EVENTS macro is defined as a bit mask of all of the above events.
 *
 * Two additional convenience macros are IN_MOVE, which equates to
 * IN_MOVED_FROM|IN_MOVED_TO, and IN_CLOSE which equates to
 * IN_CLOSE_WRITE|IN_CLOSE_NOWRITE.
 *
 * The following bitmasks can also be provided when creating a new watch:
 *
 * \li \c IN_DONT_FOLLOW  - Don't dereference pathname if it is a symbolic link
 * \li \c IN_MASK_ADD    -  Add (OR) events to watch mask for this pathname if
 *                          it already exists (instead of replacing mask)
 * \li \c IN_ONESHOT    -   Monitor pathname for one event, then remove from
 *                          watch list
 * \li \c IN_ONLYDIR    -   Only watch pathname if it is a directory
 *
 * The following bitmasks may occur in events generated by a watch:
 *
 * \li \c IN_IGNORED   -   Watch was removed explicitly
 *                        (inotifytools_remove_watch_*) or automatically (file
 *                        was deleted, or file system was unmounted)
 * \li \c IN_ISDIR   -     Subject of this event is a directory
 * \li \c IN_Q_OVERFLOW  - Event queue overflowed (wd is -1 for this event)
 * \li \c IN_UNMOUNT    -  File system containing watched object was unmounted
 *
 * @section TODO TODO list
 *
 * @todo Improve wd/filename mapping.  Currently there is no explicit code for
 *       handling different filenames mapping to the same inode (and hence, wd).
 *       gamin's approach sounds good: let the user watch an inode using several
 *       different filenames, and when an event occurs on the inode, generate an
 *       event for each filename.
 */

#define MAX_EVENTS 4096
#define MAX_STRLEN 4096
#define INOTIFY_PROCDIR "/proc/sys/fs/inotify/"
#define WATCHES_SIZE_PATH INOTIFY_PROCDIR "max_user_watches"
#define QUEUE_SIZE_PATH   INOTIFY_PROCDIR "max_queued_watches"
#define INSTANCES_PATH    INOTIFY_PROCDIR "max_user_instances"

static int inotify_fd;
static unsigned  num_access;
static unsigned  num_modify;
static unsigned  num_attrib;
static unsigned  num_close_nowrite;
static unsigned  num_close_write;
static unsigned  num_open;
static unsigned  num_move_self;
static unsigned  num_moved_to;
static unsigned  num_moved_from;
static unsigned  num_create;
static unsigned  num_delete;
static unsigned  num_delete_self;
static unsigned  num_unmount;
static unsigned  num_total;
static int collect_stats = 0;

struct rbtree *tree_wd = 0;
struct rbtree *tree_filename = 0;
static int error = 0;
static int init = 0;
static char* timefmt = 0;
static regex_t* regex = 0;

int isdir( char const * path );
void record_stats( struct inotify_event const * event );
int onestr_to_event(char const * event);

/**
 * @internal
 * Assert that a condition evaluates to true, and optionally output a message
 * if the assertion fails.
 *
 * @param  cond  Integer; if 0, assertion fails, otherwise assertion succeeds.
 *
 * @param  mesg  A human-readable error message shown if assertion fails.
 *
 * @section example Example
 * @code
 * int upper = 100, lower = 50;
 * int input = get_user_input();
 * niceassert( input <= upper && input >= lower,
 *             "input not in required range!");
 * @endcode
 */
#define niceassert(cond,mesg) _niceassert((long)cond, __LINE__, __FILE__, \
                                          #cond, mesg)

#define nasprintf(...) niceassert( -1 != asprintf(__VA_ARGS__), "out of memory")

/**
 * @internal
 * Assert that a condition evaluates to true, and optionally output a message
 * if the assertion fails.
 *
 * You should use the niceassert() preprocessor macro instead.
 *
 * @param  cond  If 0, assertion fails, otherwise assertion succeeds.
 *
 * @param  line  Line number of source code where assertion is made.
 *
 * @param  file  Name of source file where assertion is made.
 *
 * @param  condstr  Stringified assertion expression.
 *
 * @param  mesg  A human-readable error message shown if assertion fails.
 */
void _niceassert( long cond, int line, char const * file, char const * condstr,
                  char const * mesg ) {
	if ( cond ) return;

	if ( mesg ) {
		fprintf(stderr, "%s:%d assertion ( %s ) failed: %s\n", file, line,
		        condstr, mesg );
	}
	else {
		fprintf(stderr, "%s:%d assertion ( %s ) failed.\n", file, line, condstr);
	}
}

/**
 * @internal
 * Construct a string from a character.
 *
 * @param ch A character.
 *
 * @return A string of length 1 consisting of the character @a ch.  The string
 *         will be overwritten in subsequent calls.
 */
char * chrtostr(char ch) {
	static char str[2] = { '\0', '\0' };
	str[0] = ch;
	return str;
}

/**
 * @internal
 */
int read_num_from_file( char * filename, int * num ) {
	FILE * file = fopen( filename, "r" );
	if ( !file ) {
		error = errno;
		return 0;
	}

	if ( EOF == fscanf( file, "%d", num ) ) {
		error = errno;
		return 0;
	}

	niceassert( 0 == fclose( file ), 0 );

	return 1;
}

int wd_compare(const void *d1, const void *d2, const void *config) {
	if (!d1 || !d2) return d1 - d2;
	return ((watch*)d1)->wd - ((watch*)d2)->wd;
}

int filename_compare(const void *d1, const void *d2, const void *config) {
	if (!d1 || !d2) return d1 - d2;
	return strcmp(((watch*)d1)->filename, ((watch*)d2)->filename);
}

/**
 * @internal
 */
watch *watch_from_wd( int wd ) {
	watch w;
	w.wd = wd;
	return (watch*)rbfind(&w, tree_wd);
}

/**
 * @internal
 */
watch *watch_from_filename( char const *filename ) {
	watch w;
	w.filename = (char*)filename;
	return (watch*)rbfind(&w, tree_filename);
}

/**
 * Initialise inotify.
 *
 * You must call this function before using any function which adds or removes
 * watches or attempts to access any information about watches.
 *
 * @return 1 on success, 0 on failure.  On failure, the error can be
 *         obtained from inotifytools_error().
 */
int inotifytools_initialize() {
	if (init) return 1;

	error = 0;
	// Try to initialise inotify
	inotify_fd = inotify_init();
	if (inotify_fd < 0)	{
		error = inotify_fd;
		return 0;
	}

	collect_stats = 0;
	init = 1;
	tree_wd = rbinit(wd_compare, 0);
	tree_filename = rbinit(filename_compare, 0);
	timefmt = 0;

	return 1;
}

/**
 * @internal
 */
void destroy_watch(watch *w) {
	if (w->filename) free(w->filename);
	free(w);
}

/**
 * @internal
 */
void cleanup_tree(const void *nodep,
                 const VISIT which,
                 const int depth, void* arg) {
	if (which != endorder && which != leaf) return;
	watch *w = (watch*)nodep;
	destroy_watch(w);
}

/**
 * Close inotify and free the memory used by inotifytools.
 *
 * If you call this function, you must call inotifytools_initialize()
 * again before any other functions can be used.
 */
void inotifytools_cleanup() {
	if (!init) return;

	init = 0;
	close(inotify_fd);
	collect_stats = 0;
	error = 0;
	timefmt = 0;

	if (regex) {
		regfree(regex);
		free(regex);
		regex = 0;
	}

	rbwalk(tree_wd, cleanup_tree, 0);
	rbdestroy(tree_wd); tree_wd = 0;
	rbdestroy(tree_filename); tree_filename = 0;
}

/**
 * @internal
 */
void empty_stats(const void *nodep,
                 const VISIT which,
                 const int depth, void *arg) {
    if (which != endorder && which != leaf) return;
	watch *w = (watch*)nodep;
	w->hit_access = 0;
	w->hit_modify = 0;
	w->hit_attrib = 0;
	w->hit_close_nowrite = 0;
	w->hit_close_write = 0;
	w->hit_open = 0;
	w->hit_move_self = 0;
	w->hit_moved_from = 0;
	w->hit_moved_to = 0;
	w->hit_create = 0;
	w->hit_delete = 0;
	w->hit_delete_self = 0;
	w->hit_unmount = 0;
	w->hit_total = 0;
}

/**
 * @internal
 */
void replace_filename(const void *nodep,
                      const VISIT which,
                      const int depth, void *arg) {
    if (which != endorder && which != leaf) return;
	watch *w = (watch*)nodep;
	char *old_name = ((char**)arg)[0];
	char *new_name = ((char**)arg)[1];
	int old_len = *((int*)&((char**)arg)[2]);
	char *name;
	if ( 0 == strncmp( old_name, w->filename, old_len ) ) {
		nasprintf( &name, "%s%s", new_name, &(w->filename[old_len]) );
		if (!strcmp( w->filename, new_name )) {
			free(name);
		} else {
			rbdelete(w, tree_filename);
			free( w->filename );
			w->filename = name;
			rbsearch(w, tree_filename);
		}
	}
}

/**
 * @internal
 */
void get_num(const void *nodep,
             const VISIT which,
             const int depth, void *arg) {
    if (which != endorder && which != leaf) return;
	++(*((int*)arg));
}


/**
 * Initialize or reset statistics.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * When this function is called, all subsequent events will be tallied.
 * Statistics can then be obtained via the @a inotifytools_get_stat_* functions.
 *
 * After the first call, subsequent calls to this function will reset the
 * event tallies to 0.
 */
void inotifytools_initialize_stats() {
	niceassert( init, "inotifytools_initialize not called yet" );

	// if already collecting stats, reset stats
	if (collect_stats) {
		rbwalk(tree_wd, empty_stats, 0);
	}

	num_access = 0;
	num_modify = 0;
	num_attrib = 0;
	num_close_nowrite = 0;
	num_close_write = 0;
	num_open = 0;
	num_move_self = 0;
	num_moved_from = 0;
	num_moved_to = 0;
	num_create = 0;
	num_delete = 0;
	num_delete_self = 0;
	num_unmount = 0;
	num_total = 0;

	collect_stats = 1;
}

/**
 * Convert character separated events from string form to integer form
 * (as in inotify.h).
 *
 * @param    event    a sequence of events in string form as defined in
 *                    inotify.h without leading IN_ prefix (e.g., MODIFY,
 *                    ATTRIB), separated by the @a sep character.  Case
 *                    insensitive.  Can be a single event.
 *                    Can be empty or NULL.  See section \ref events.
 *
 * @param    sep      Character used to separate events.  @a sep must not be
 *                    a character in a-z, A-Z, or _.
 *
 * @return            integer representing the mask specified by @a event, or 0
 *                    if any string in @a event is empty or NULL, or -1 if
 *                    any string in @a event does not match any event or
 *                    @a sep is invalid.
 *
 * @section example Example
 * @code
 * char * eventstr = "MODIFY:CLOSE:CREATE";
 * int eventnum = inotifytools_str_to_event_sep( eventstr, ':' );
 * if ( eventnum == IN_MODIFY | IN_CLOSE | IN_CREATE ) {
 *    printf( "This code always gets executed!\n" );
 * }
 * @endcode
 */
int inotifytools_str_to_event_sep(char const * event, char sep) {
	if ( strchr( "_" "abcdefghijklmnopqrstuvwxyz"
	                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ", sep ) ) {
		return -1;
	}

	int ret, ret1, len;
	char * event1, * event2;
	char eventstr[4096];
	ret = 0;

	if ( !event || !event[0] ) return 0;

	event1 = (char *)event;
	event2 = strchr( event1, sep );
	while ( event1 && event1[0] ) {
		if ( event2 ) {
			len = event2 - event1;
			niceassert( len < 4096, "malformed event string (very long)" );
		}
		else {
			len = strlen(event1);
		}
		if ( len > 4095 ) len = 4095;
		strncpy( eventstr, event1, len );
		eventstr[len] = 0;

		ret1 = onestr_to_event( eventstr );
		if ( 0 == ret1 || -1 == ret1 ) {
			ret = ret1;
			break;
		}
		ret |= ret1;

		event1 = event2;
		if ( event1 && event1[0] ) {
			// jump over 'sep' character
			++event1;
			// if last character was 'sep'...
			if ( !event1[0] ) return 0;
			event2 = strchr( event1, sep );
		}
	}

	return ret;
}

/**
 * Convert comma-separated events from string form to integer form
 * (as in inotify.h).
 *
 * @param    event    a sequence of events in string form as defined in
 *                    inotify.h without leading IN_ prefix (e.g., MODIFY,
 *                    ATTRIB), comma-separated.  Case
 *                    insensitive.  Can be a single event.
 *                    Can be empty or NULL.  See section \ref events.
 *
 * @return            integer representing the mask specified by @a event, or 0
 *                    if any string in @a event is empty or NULL, or -1 if
 *                    any string in @a event does not match any event.
 *
 * @section example Example
 * @code
 * char * eventstr = "MODIFY,CLOSE,CREATE";
 * int eventnum = inotifytools_str_to_event( eventstr );
 * if ( eventnum == IN_MODIFY | IN_CLOSE | IN_CREATE ) {
 *    printf( "This code always gets executed!\n" );
 * }
 * @endcode
 */
int inotifytools_str_to_event(char const * event) {
	return inotifytools_str_to_event_sep( event, ',' );
}

/**
 * @internal
 * Convert a single event from string form to integer form (as in inotify.h).
 *
 * @param    event    event in string form as defined in inotify.h without
 *                    leading IN_ prefix (e.g., MODIFY, ATTRIB).  Case
 *                    insensitive.  Can be empty or NULL.
 * @return            integer representing the mask specified by 'event', or 0
 *                    if @a event is empty or NULL, or -1 if string does not
 *                    match any event.
 */
int onestr_to_event(char const * event)
{
	static int ret;
	ret = -1;

	if ( !event || !event[0] )
		ret = 0;
	else if ( 0 == strcasecmp(event, "ACCESS") )
		ret = IN_ACCESS;
	else if ( 0 == strcasecmp(event, "MODIFY") )
		ret = IN_MODIFY;
	else if ( 0 == strcasecmp(event, "ATTRIB") )
		ret = IN_ATTRIB;
	else if ( 0 == strcasecmp(event, "CLOSE_WRITE") )
		ret = IN_CLOSE_WRITE;
	else if ( 0 == strcasecmp(event, "CLOSE_NOWRITE") )
		ret = IN_CLOSE_NOWRITE;
	else if ( 0 == strcasecmp(event, "OPEN") )
		ret = IN_OPEN;
	else if ( 0 == strcasecmp(event, "MOVED_FROM") )
		ret = IN_MOVED_FROM;
	else if ( 0 == strcasecmp(event, "MOVED_TO") )
		ret = IN_MOVED_TO;
	else if ( 0 == strcasecmp(event, "CREATE") )
		ret = IN_CREATE;
	else if ( 0 == strcasecmp(event, "DELETE") )
		ret = IN_DELETE;
	else if ( 0 == strcasecmp(event, "DELETE_SELF") )
		ret = IN_DELETE_SELF;
	else if ( 0 == strcasecmp(event, "UNMOUNT") )
		ret = IN_UNMOUNT;
	else if ( 0 == strcasecmp(event, "Q_OVERFLOW") )
		ret = IN_Q_OVERFLOW;
	else if ( 0 == strcasecmp(event, "IGNORED") )
		ret = IN_IGNORED;
	else if ( 0 == strcasecmp(event, "CLOSE") )
		ret = IN_CLOSE;
	else if ( 0 == strcasecmp(event, "MOVE_SELF") )
		ret = IN_MOVE_SELF;
	else if ( 0 == strcasecmp(event, "MOVE") )
		ret = IN_MOVE;
	else if ( 0 == strcasecmp(event, "ISDIR") )
		ret = IN_ISDIR;
	else if ( 0 == strcasecmp(event, "ONESHOT") )
		ret = IN_ONESHOT;
	else if ( 0 == strcasecmp(event, "ALL_EVENTS") )
		ret = IN_ALL_EVENTS;

	return ret;
}

/**
 * Convert event from integer form to string form (as in inotify.h).
 *
 * The returned string is from static storage; subsequent calls to this function
 * or inotifytools_event_to_str_sep() will overwrite it.  Don't free() it and
 * make a copy if you want to keep it.
 *
 * @param    events   OR'd event(s) in integer form as defined in inotify.h.
 *                    See section \ref events.
 *
 * @return            comma-separated string representing the event(s), in no
 *                    particular order
 *
 * @section example Example
 * @code
 * int eventnum == IN_MODIFY | IN_CLOSE | IN_CREATE;
 * char * eventstr = inotifytools_event_to_str( eventnum );
 * printf( "%s\n", eventstr );
 * // outputs something like MODIFY,CLOSE,CREATE but order not guaranteed.
 * @endcode
 */
char * inotifytools_event_to_str(int events) {
	return inotifytools_event_to_str_sep(events, ',');
}

/**
 * Convert event from integer form to string form (as in inotify.h).
 *
 * The returned string is from static storage; subsequent calls to this function
 * or inotifytools_event_to_str() will overwrite it.  Don't free() it and
 * make a copy if you want to keep it.
 *
 * @param    events   OR'd event(s) in integer form as defined in inotify.h
 *
 * @param    sep      character used to separate events
 *
 * @return            @a sep separated string representing the event(s), in no
 *                    particular order.  If the integer is not made of OR'ed
 *                    inotify events, the string returned will be a hexadecimal
 *                    representation of the integer.
 *
 * @section example Example
 * @code
 * int eventnum == IN_MODIFY | IN_CLOSE | IN_CREATE;
 * char * eventstr = inotifytools_event_to_str_sep( eventnum, '-' );
 * printf( "%s\n", eventstr );
 * // outputs something like MODIFY-CLOSE-CREATE but order not guaranteed.
 * @endcode
 */
char * inotifytools_event_to_str_sep(int events, char sep)
{
	static char ret[1024];
	ret[0] = '\0';
	ret[1] = '\0';

	if ( IN_ACCESS & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "ACCESS" );
	}
	if ( IN_MODIFY & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "MODIFY" );
	}
	if ( IN_ATTRIB & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "ATTRIB" );
	}
	if ( IN_CLOSE_WRITE & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "CLOSE_WRITE" );
	}
	if ( IN_CLOSE_NOWRITE & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "CLOSE_NOWRITE" );
	}
	if ( IN_OPEN & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "OPEN" );
	}
	if ( IN_MOVED_FROM & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "MOVED_FROM" );
	}
	if ( IN_MOVED_TO & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "MOVED_TO" );
	}
	if ( IN_CREATE & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "CREATE" );
	}
	if ( IN_DELETE & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "DELETE" );
	}
	if ( IN_DELETE_SELF & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "DELETE_SELF" );
	}
	if ( IN_UNMOUNT & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "UNMOUNT" );
	}
	if ( IN_Q_OVERFLOW & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "Q_OVERFLOW" );
	}
	if ( IN_IGNORED & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "IGNORED" );
	}
	if ( IN_CLOSE & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "CLOSE" );
	}
	if ( IN_MOVE_SELF & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "MOVE_SELF" );
	}
	if ( IN_ISDIR & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "ISDIR" );
	}
	if ( IN_ONESHOT & events ) {
		strcat( ret, chrtostr(sep) );
		strcat( ret, "ONESHOT" );
	}

	// Maybe we didn't match any... ?
	if (ret[0] == '\0') {
		niceassert( -1 != sprintf( ret, "%c0x%08x", sep, events ), 0 );
	}

	return &ret[1];
}

/**
 * Get the filename used to establish a watch.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param wd watch descriptor.
 *
 * @return filename associated with watch descriptor @a wd, or NULL if @a wd
 *         is not associated with any filename.
 *
 * @note This always returns the filename which was used to establish a watch.
 *       This means the filename may be a relative path.  If this isn't desired,
 *       then always use absolute paths when watching files.
 *       Also, this is not necessarily the filename which might have been used
 *       to cause an event on the file, since inotify is inode based and there
 *       can be many filenames mapping to a single inode.
 *       Finally, if a file is moved or renamed while being watched, the
 *       filename returned will still be the original name.
 */
char * inotifytools_filename_from_wd( int wd ) {
	niceassert( init, "inotifytools_initialize not called yet" );
	watch *w = watch_from_wd(wd);
	if (!w)
        return NULL;

	return w->filename;
}

/**
 * Get the watch descriptor for a particular filename.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param filename file name to find watch descriptor for.
 *
 * @return watch descriptor associated with filename, or -1 if @a filename is
 *         not associated with any watch descriptor.
 *
 * @note The filename specified must always be the original name used to
 *       establish the watch.
 */
int inotifytools_wd_from_filename( char const * filename ) {
	niceassert( init, "inotifytools_initialize not called yet" );
	watch *w = watch_from_filename(filename);
	if (!w) return -1;
	return w->wd;
}

/**
 * Set the filename for a particular watch descriptor.
 *
 * This function should be used to update a filename when a file is known to
 * have been moved or renamed.  At the moment, libinotifytools does not
 * automatically handle this situation.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param wd Watch descriptor.
 *
 * @param filename New filename.
 */
void inotifytools_set_filename_by_wd( int wd, char const * filename ) {
	niceassert( init, "inotifytools_initialize not called yet" );
	watch *w = watch_from_wd(wd);
	if (!w) return;
	if (w->filename) free(w->filename);
	w->filename = strdup(filename);
}

/**
 * Set the filename for one or more watches with a particular existing filename.
 *
 * This function should be used to update a filename when a file is known to
 * have been moved or renamed.  At the moment, libinotifytools does not
 * automatically handle this situation.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param oldname Current filename.
 *
 * @param newname New filename.
 */
void inotifytools_set_filename_by_filename( char const * oldname,
                                            char const * newname ) {
	watch *w = watch_from_filename(oldname);
	if (!w) return;
	if (w->filename) free(w->filename);
	w->filename = strdup(newname);
}

/**
 * Replace a certain filename prefix on all watches.
 *
 * This function should be used to update filenames for an entire directory tree
 * when a directory is known to have been moved or renamed.  At the moment,
 * libinotifytools does not automatically handle this situation.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param oldname Current filename prefix.
 *
 * @param newname New filename prefix.
 *
 * @section example Example
 * @code
 * // if /home/user1/original_dir is moved to /home/user2/new_dir, then to
 * // update all watches:
 * inotifytools_replace_filename( "/home/user1/original_dir",
 *                                "/home/user2/new_dir" );
 * @endcode
 */
void inotifytools_replace_filename( char const * oldname,
                                    char const * newname ) {
	if ( !oldname || !newname ) return;
	char *names[2+sizeof(int)/sizeof(char*)];
	names[0] = (char*)oldname;
	names[1] = (char*)newname;
	*((int*)&names[2]) = strlen(oldname);
	rbwalk(tree_filename, replace_filename, (void*)names);
}

/**
 * @internal
 */
int remove_inotify_watch(watch *w) {
	error = 0;
	int status = inotify_rm_watch( inotify_fd, w->wd );
	if ( status < 0 ) {
		fprintf(stderr, "Failed to remove watch on %s: %s\n", w->filename,
		        strerror(status) );
		error = status;
		return 0;
	}
	return 1;
}

/**
 * @internal
 */
watch *create_watch(int wd, char *filename) {
	if ( wd <= 0 || !filename) return 0;

	watch *w = (watch*)calloc(1, sizeof(watch));
	w->wd = wd;
	w->filename = strdup(filename);
	rbsearch(w, tree_wd);
	rbsearch(w, tree_filename);
}

/**
 * Remove a watch on a file specified by watch descriptor.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param wd Watch descriptor of watch to be removed.
 *
 * @return 1 on success, 0 on failure.  If the given watch doesn't exist,
 *         returns 1.  On failure, the error can be
 *         obtained from inotifytools_error().
 */
int inotifytools_remove_watch_by_wd( int wd ) {
	niceassert( init, "inotifytools_initialize not called yet" );
	watch *w = watch_from_wd(wd);
	if (!w) return 1;

	if (!remove_inotify_watch(w)) return 0;
	rbdelete(w, tree_wd);
	rbdelete(w, tree_filename);
	destroy_watch(w);
	return 1;
}

/**
 * Remove a watch on a file specified by filename.
 *
 * @param filename Name of file on which watch should be removed.
 *
 * @return 1 on success, 0 on failure.  On failure, the error can be
 *         obtained from inotifytools_error().
 *
 * @note The filename specified must always be the original name used to
 *       establish the watch.
 */
int inotifytools_remove_watch_by_filename( char const * filename ) {
	niceassert( init, "inotifytools_initialize not called yet" );
	watch *w = watch_from_filename(filename);
	if (!w) return 1;

	if (!remove_inotify_watch(w)) return 0;
	rbdelete(w, tree_wd);
	rbdelete(w, tree_filename);
	destroy_watch(w);
	return 1;
}

/**
 * Set up a watch on a file.
 *
 * @param filename Absolute or relative path of file to watch.
 *
 * @param events bitwise ORed inotify events to watch for.  See section
 *               \ref events.
 *
 * @return 1 on success, 0 on failure.  On failure, the error can be
 *         obtained from inotifytools_error().
 */
int inotifytools_watch_file( char const * filename, int events ) {
	static char const * filenames[2];
	filenames[0] = filename;
	filenames[1] = NULL;
	return inotifytools_watch_files( filenames, events );
}

/**
 * Set up a watch on a list of files.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param filenames null-terminated array of absolute or relative paths of
 *                  files to watch.
 *
 * @param events bitwise OR'ed inotify events to watch for.  See section
 *               \ref events.
 *
 * @return 1 on success, 0 on failure.  On failure, the error can be
 *         obtained from inotifytools_error().
 */
int inotifytools_watch_files( char const * filenames[], int events ) {
	niceassert( init, "inotifytools_initialize not called yet" );
	error = 0;

	static int i;
	for ( i = 0; filenames[i]; ++i ) {
		static int wd;
		wd = inotify_add_watch( inotify_fd, filenames[i], events );
		if ( wd < 0 ) {
			if ( wd == -1 ) {
				error = errno;
				return 0;
			} // if ( wd == -1 )
			else {
				fprintf( stderr, "Failed to watch %s: returned wd was %d "
				         "(expected -1 or >0 )", filenames[i], wd );
				// no appropriate value for error
				return 0;
			} // else
		} // if ( wd < 0 )

		char *filename;
		// Always end filename with / if it is a directory
		if ( !isdir(filenames[i])
		     || filenames[i][strlen(filenames[i])-1] == '/') {
			filename = strdup(filenames[i]);
		}
		else {
			nasprintf( &filename, "%s/", filenames[i] );
		}
		create_watch(wd, filename);
		free(filename);
	} // for

	return 1;
}

/**
 * Get the next inotify event to occur.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param timeout maximum amount of time, in seconds, to wait for an event.
 *                If @a timeout is 0, the function is non-blocking.  If
 *                @a timeout is negative, the function will block until an
 *                event occurs.
 *
 * @return pointer to an inotify event, or NULL if function timed out before
 *         an event occurred.  The event is located in static storage and it
 *         may be overwritten in subsequent calls; do not call free() on it,
 *         and make a copy if you want to keep it.
 *
 * @note Your program should call this function or
 *       inotifytools_next_events() frequently; between calls to this function,
 *       inotify events will be queued in the kernel, and eventually the queue
 *       will overflow and you will miss some events.
 *
 * @note If the function inotifytools_ignore_events_by_regex() has been called
 *       with a non-NULL parameter, this function will not return on events
 *       which match the regular expression passed to that function.  However,
 *       the @a timeout period begins again each time a matching event occurs.
 */
struct inotify_event * inotifytools_next_event( int timeout ) {
	return inotifytools_next_events( timeout, 1 );
}


/**
 * Get the next inotify events to occur.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param timeout maximum amount of time, in seconds, to wait for an event.
 *                If @a timeout is 0, the function is non-blocking.  If
 *                @a timeout is negative, the function will block until an
 *                event occurs.
 *
 * @param num_events approximate number of inotify events to wait for until
 *                   this function returns.  Use this for buffering reads to
 *                   inotify if you expect to receive large amounts of events.
 *                   You are NOT guaranteed that this number of events will
 *                   actually be read; instead, you are guaranteed that the
 *                   number of bytes read from inotify is
 *                   @a num_events * sizeof(struct inotify_event).  Obviously
 *                   the larger this number is, the greater the latency between
 *                   when an event occurs and when you'll know about it.
 *                   May not be larger than 4096.
 *
 * @return pointer to an inotify event, or NULL if function timed out before
 *         an event occurred or @a num_events < 1.  The event is located in
 *         static storage and it may be overwritten in subsequent calls; do not
 *         call free() on it, and make a copy if you want to keep it.
 *         When @a num_events is greater than 1, this will return a pointer to
 *         the first event only, and you MUST call this function again to
 *         get pointers to subsequent events; don't try to add to the pointer
 *         to find the next events or you will run into trouble.
 *
 * @note You may actually get different events with different values of
 *       @a num_events.  This is because inotify does some in-kernel filtering
 *       of duplicate events, meaning some duplicate events will not be
 *       reported if @a num_events > 1.  For some purposes this is fine, but
 *       for others (such as gathering accurate statistics on numbers of event
 *       occurrences) you must call this function with @a num_events = 1, or
 *       simply use inotifytools_next_event().
 *
 * @note Your program should call this function or
 *       inotifytools_next_events() frequently; between calls to this function,
 *       inotify events will be queued in the kernel, and eventually the queue
 *       will overflow and you will miss some events.
 *
 * @note If the function inotifytools_ignore_events_by_regex() has been called
 *       with a non-NULL parameter, this function will not return on events
 *       which match the regular expression passed to that function.  However,
 *       the @a timeout period begins again each time a matching event occurs.
 */
struct inotify_event * inotifytools_next_events( int timeout, int num_events ) {
	niceassert( init, "inotifytools_initialize not called yet" );
	niceassert( num_events <= MAX_EVENTS, "too many events requested" );

	if ( num_events < 1 ) return NULL;

	static struct inotify_event event[MAX_EVENTS];
	static struct inotify_event * ret;
	static int first_byte = 0;
	static ssize_t bytes;
	static jmp_buf jmp;
	static char match_name[MAX_STRLEN];

#define RETURN(A) {\
	if (regex) {\
		inotifytools_snprintf(match_name, MAX_STRLEN, A, "%w%f");\
		if (0 == regexec(regex, match_name, 0, 0, 0)) {\
			longjmp(jmp,0);\
		}\
	}\
	if ( collect_stats ) {\
		record_stats( A );\
	}\
	return A;\
}

	setjmp(jmp);

	error = 0;

	// first_byte is index into event buffer
	if ( first_byte != 0
	  && first_byte <= (int)(bytes - sizeof(struct inotify_event)) ) {

		ret = (struct inotify_event *)((char *)&event[0] + first_byte);
		first_byte += sizeof(struct inotify_event) + ret->len;

		// if the pointer to the next event exactly hits end of bytes read,
		// that's good.  next time we're called, we'll read.
		if ( first_byte == bytes ) {
			first_byte = 0;
		}
		else if ( first_byte > bytes ) {
			// oh... no.  this can't be happening.  An incomplete event.
			// Copy what we currently have into first element, call self to
			// read remainder.
			// oh, and they BETTER NOT overlap.
			// Boy I hope this code works.
			// But I think this can never happen due to how inotify is written.
			niceassert( (long)((char *)&event[0] +
			            sizeof(struct inotify_event) +
			            event[0].len) <= (long)ret,
			            "extremely unlucky user, death imminent" );
			// how much of the event do we have?
			bytes = (char *)&event[0] + bytes - (char *)ret;
			memcpy( &event[0], ret, bytes );
			return inotifytools_next_events( timeout, num_events );
		}
		RETURN(ret);

	}

	else if ( first_byte == 0 ) {
		bytes = 0;
	}


	static ssize_t this_bytes;
	static unsigned int bytes_to_read;
	static int rc;
	static fd_set read_fds;

	static struct timeval read_timeout;
	read_timeout.tv_sec = timeout;
	read_timeout.tv_usec = 0;
	static struct timeval * read_timeout_ptr;
	read_timeout_ptr = ( timeout <= 0 ? NULL : &read_timeout );

	FD_ZERO(&read_fds);
	FD_SET(inotify_fd, &read_fds);
	rc = select(inotify_fd + 1, &read_fds,
	            NULL, NULL, read_timeout_ptr);
	if ( rc < 0 ) {
		// error
		error = errno;
		return NULL;
	}
	else if ( rc == 0 ) {
		// timeout
		return NULL;
	}

	// wait until we have enough bytes to read
	do {
		rc = ioctl( inotify_fd, FIONREAD, &bytes_to_read );
	} while ( !rc &&
	          bytes_to_read < sizeof(struct inotify_event)*num_events );

	if ( rc == -1 ) {
		error = errno;
		return NULL;
	}

	this_bytes = read(inotify_fd, &event[0] + bytes,
	                  sizeof(struct inotify_event)*MAX_EVENTS - bytes);
	if ( this_bytes < 0 ) {
		error = errno;
		return NULL;
	}
	if ( this_bytes == 0 ) {
		fprintf(stderr, "Inotify reported end-of-file.  Possibly too many "
		                "events occurred at once.\n");
		return NULL;
	}
	bytes += this_bytes;

	ret = &event[0];
	first_byte = sizeof(struct inotify_event) + ret->len;
	niceassert( first_byte <= bytes, "ridiculously long filename, things will "
	                                 "almost certainly screw up." );
	if ( first_byte == bytes ) {
		first_byte = 0;
	}

	RETURN(ret);

#undef RETURN
}

/**
 * Set up recursive watches on an entire directory tree.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @param path path of directory or file to watch.  If the path is a directory,
 *             every subdirectory will also be watched for the same events up
 *             to the maximum readable depth.  If the path is a file, the file
 *             is watched exactly as if inotifytools_watch_file() were used.
 *
 * @param events Inotify events to watch for.  See section \ref events.
 *
 * @return 1 on success, 0 on failure.  On failure, the error can be
 *         obtained from inotifytools_error().  Note that some errors on
 *         subdirectories will be ignored; for example, if you watch a directory
 *         tree which contains some directories which you do not have access to,
 *         those directories will not be watched, but this function will still
 *         return 1 if no other errors occur.
 *
 * @note This function does not attempt to work atomically.  If you use this
 *       function to watch a directory tree and files or directories are being
 *       created or removed within that directory tree, there are no guarantees
 *       as to whether or not those files will be watched.
 */
int inotifytools_watch_recursively( char const * path, int events ) {
	return inotifytools_watch_recursively_with_exclude( path, events, 0 );
}

/**
 * Set up recursive watches on an entire directory tree, optionally excluding
 * some directories.
 *
 * inotifytools_initialize() must be called before this function can
 * be used.
 *
 * @author UH
 *
 * @param path path of directory or file to watch.  If the path is a directory,
 *             every subdirectory will also be watched for the same events up
 *             to the maximum readable depth.  If the path is a file, the file
 *             is watched exactly as if inotifytools_watch_file() were used.
 *
 * @param exclude_list NULL terminated path list of directories not to watch.
 *                     Can be NULL if no paths are to be excluded.
 *                     Directories may or may not include a trailing '/'.
 *
 * @param events Inotify events to watch for.  See section \ref events.
 *
 * @return 1 on success, 0 on failure.  On failure, the error can be
 *         obtained from inotifytools_error().  Note that some errors on
 *         subdirectories will be ignored; for example, if you watch a directory
 *         tree which contains some directories which you do not have access to,
 *         those directories will not be watched, but this function will still
 *         return 1 if no other errors occur.
 *
 * @note This function does not attempt to work atomically.  If you use this
 *       function to watch a directory tree and files or directories are being
 *       created or removed within that directory tree, there are no guarantees
 *       as to whether or not those files will be watched.
 */
int inotifytools_watch_recursively_with_exclude( char const * path, int events,
                                                 char const ** exclude_list ) {
	niceassert( init, "inotifytools_initialize not called yet" );

	DIR * dir;
	char * my_path;
	error = 0;
	dir = opendir( path );
	if ( !dir ) {
		// If not a directory, don't need to do anything special
		if ( errno == ENOTDIR ) {
			return inotifytools_watch_file( path, events );
		}
		else {
			error = errno;
			return 0;
		}
	}

	hcreate_r(MAX_ENTRIES, &tab);
	if ( path[strlen(path)-1] != '/' ) {
		nasprintf( &my_path, "%s/", path );
	}
	else {
		my_path = (char *)path;
	}

	static struct dirent * ent;
	char * next_file;
	static struct stat64 my_stat;
	ent = readdir( dir );
	// Watch each directory within this directory
	while ( ent ) {
		if ( (0 != strcmp( ent->d_name, "." )) &&
		     (0 != strcmp( ent->d_name, ".." )) ) {
			nasprintf(&next_file,"%s%s", my_path, ent->d_name);
			if ( -1 == lstat64( next_file, &my_stat ) ) {
				error = errno;
				free( next_file );
				if ( errno != EACCES ) {
					error = errno;
					if ( my_path != path ) free( my_path );
					closedir( dir );
					return 0;
				}
			}
			else if ( S_ISDIR( my_stat.st_mode ) &&
			          !S_ISLNK( my_stat.st_mode )) {
				free( next_file );
				nasprintf(&next_file,"%s%s/", my_path, ent->d_name);
				static unsigned int no_watch;
				static char const ** exclude_entry;

				no_watch = 0;
				for (exclude_entry = exclude_list;
					 exclude_entry && *exclude_entry && !no_watch;
					 ++exclude_entry) {
					static int exclude_length;

					exclude_length = strlen(*exclude_entry);
					if ((*exclude_entry)[exclude_length-1] == '/') {
						--exclude_length;
					}
					if ( strlen(next_file) == (unsigned)(exclude_length + 1) &&
					    !strncmp(*exclude_entry, next_file, exclude_length)) {
						// directory found in exclude list
						no_watch = 1;
					}
				}
				if (!no_watch) {
					static int status;
					status = inotifytools_watch_recursively_with_exclude(
					              next_file,
					              events,
					              exclude_list );
					// For some errors, we will continue.
					if ( !status && (EACCES != error) && (ENOENT != error) &&
					     (ELOOP != error) ) {
						free( next_file );
						if ( my_path != path ) free( my_path );
						closedir( dir );
						return 0;
					}
				} // if !no_watch
				hadd(&tab, next_file, dir_count);
				hfind(&tab, next_file);
				dir_stack[dir_count] = next_file;
				printf("veraoks_debug: watching %s(%d)\n", next_file, dir_count);
				dir_count++;
				free( next_file );
			} // if isdir and not islnk
			else {
				free( next_file );
			}
		}
		ent = readdir( dir );
		error = 0;
	}

	closedir( dir );

	int ret = inotifytools_watch_file( my_path, events );
	if ( my_path != path ) free( my_path );
        return ret;
}

/**
 * @internal
 */
void record_stats( struct inotify_event const * event ) {
	if (!event) return;
	watch *w = watch_from_wd(event->wd);
	if (!w) return;
	if ( IN_ACCESS & event->mask ) {
		++w->hit_access;
		++num_access;
	}
	if ( IN_MODIFY & event->mask ) {
		++w->hit_modify;
		++num_modify;
	}
	if ( IN_ATTRIB & event->mask ) {
		++w->hit_attrib;
		++num_attrib;
	}
	if ( IN_CLOSE_WRITE & event->mask ) {
		++w->hit_close_write;
		++num_close_write;
	}
	if ( IN_CLOSE_NOWRITE & event->mask ) {
		++w->hit_close_nowrite;
		++num_close_nowrite;
	}
	if ( IN_OPEN & event->mask ) {
		++w->hit_open;
		++num_open;
	}
	if ( IN_MOVED_FROM & event->mask ) {
		++w->hit_moved_from;
		++num_moved_from;
	}
	if ( IN_MOVED_TO & event->mask ) {
		++w->hit_moved_to;
		++num_moved_to;
	}
	if ( IN_CREATE & event->mask ) {
		++w->hit_create;
		++num_create;
	}
	if ( IN_DELETE & event->mask ) {
		++w->hit_delete;
		++num_delete;
	}
	if ( IN_DELETE_SELF & event->mask ) {
		++w->hit_delete_self;
		++num_delete_self;
	}
	if ( IN_UNMOUNT & event->mask ) {
		++w->hit_unmount;
		++num_unmount;
	}
	if ( IN_MOVE_SELF & event->mask ) {
		++w->hit_move_self;
		++num_move_self;
	}

	++w->hit_total;
	++num_total;

}

int *stat_ptr(watch *w, int event)
{
	if ( IN_ACCESS == event )
		return &w->hit_access;
	if ( IN_MODIFY == event )
		return &w->hit_modify;
	if ( IN_ATTRIB == event )
		return &w->hit_attrib;
	if ( IN_CLOSE_WRITE == event )
		return &w->hit_close_write;
	if ( IN_CLOSE_NOWRITE == event )
		return &w->hit_close_nowrite;
	if ( IN_OPEN == event )
		return &w->hit_open;
	if ( IN_MOVED_FROM == event )
		return &w->hit_moved_from;
	if ( IN_MOVED_TO == event )
		return &w->hit_moved_to;
	if ( IN_CREATE == event )
		return &w->hit_create;
	if ( IN_DELETE == event )
		return &w->hit_delete;
	if ( IN_DELETE_SELF == event )
		return &w->hit_delete_self;
	if ( IN_UNMOUNT == event )
		return &w->hit_unmount;
	if ( IN_MOVE_SELF == event )
		return &w->hit_move_self;
	if ( 0 == event )
		return &w->hit_total;
	return 0;
}

/**
 * Get statistics by a particular watch descriptor.
 *
 * inotifytools_initialize_stats() must be called before this function can
 * be used.
 *
 * @param wd watch descriptor to get stats for.
 *
 * @param event a single inotify event to get statistics for, or 0 for event
 *              total.  See section \ref events.
 *
 * @return the number of times the event specified by @a event has occurred on
 *         the watch descriptor specified by @a wd since stats collection was
 *         enabled, or -1 if @a event or @a wd are invalid.
 */
int inotifytools_get_stat_by_wd( int wd, int event ) {
	if (!collect_stats) return -1;

	watch *w = watch_from_wd(wd);
	if (!w) return -1;
	int *i = stat_ptr(w, event);
	if (!i) return -1;
	return *i;
}

/**
 * Get statistics aggregated across all watches.
 *
 * inotifytools_initialize_stats() must be called before this function can
 * be used.
 *
 * @param event a single inotify event to get statistics for, or 0 for event
 *              total.  See section \ref events.
 *
 * @return the number of times the event specified by @a event has occurred over
 *         all watches since stats collection was enabled, or -1 if @a event
 *         is not a valid event.
 */
int inotifytools_get_stat_total( int event ) {
	if (!collect_stats) return -1;
	if ( IN_ACCESS == event )
		return num_access;
	if ( IN_MODIFY == event )
		return num_modify;
	if ( IN_ATTRIB == event )
		return num_attrib;
	if ( IN_CLOSE_WRITE == event )
		return num_close_write;
	if ( IN_CLOSE_NOWRITE == event )
		return num_close_nowrite;
	if ( IN_OPEN == event )
		return num_open;
	if ( IN_MOVED_FROM == event )
		return num_moved_from;
	if ( IN_MOVED_TO == event )
		return num_moved_to;
	if ( IN_CREATE == event )
		return num_create;
	if ( IN_DELETE == event )
		return num_delete;
	if ( IN_DELETE_SELF == event )
		return num_delete_self;
	if ( IN_UNMOUNT == event )
		return num_unmount;
	if ( IN_MOVE_SELF == event )
		return num_move_self;

	if ( 0 == event )
		return num_total;

	return -1;
}

/**
 * Get statistics by a particular filename.
 *
 * inotifytools_initialize_stats() must be called before this function can
 * be used.
 *
 * @param filename name of file to get stats for.
 *
 * @param event a single inotify event to get statistics for, or 0 for event
 *              total.  See section \ref events.
 *
 * @return the number of times the event specified by @a event has occurred on
 *         the file specified by @a filename since stats collection was
 *         enabled, or -1 if the file is not being watched or @a event is
 *         invalid.
 *
 * @note The filename specified must always be the original name used to
 *       establish the watch.
 */
int inotifytools_get_stat_by_filename( char const * filename,
                                                int event ) {
	return inotifytools_get_stat_by_wd( inotifytools_wd_from_filename(
	       filename ), event );
}

/**
 * Get the last error which occurred.
 *
 * When a function fails, call this to find out why.  The returned value is
 * a typical @a errno value, the meaning of which depends on context.  For
 * example, if inotifytools_watch_file() fails because you attempt to watch
 * a file which doesn't exist, this function will return @a ENOENT.
 *
 * @return an error code.
 */
int inotifytools_error() {
	return error;
}

/**
 * @internal
 */
int isdir( char const * path ) {
	static struct stat64 my_stat;

	if ( -1 == lstat64( path, &my_stat ) ) {
		if (errno == ENOENT) return 0;
		fprintf(stderr, "Stat failed on %s: %s\n", path, strerror(errno));
		return 0;
	}

	return S_ISDIR( my_stat.st_mode ) && !S_ISLNK( my_stat.st_mode );
}


/**
 * Get the number of watches set up through libinotifytools.
 *
 * @return number of watches set up by inotifytools_watch_file(),
 *         inotifytools_watch_files() and inotifytools_watch_recursively().
 */
int inotifytools_get_num_watches() {
	int ret = 0;
	rbwalk(tree_filename, get_num, (void*)&ret);
	return ret;
}

/**
 * Print a string to standard out using an inotify_event and a printf-like
 * syntax.
 * The string written will only ever be up to 4096 characters in length.
 *
 * @param event the event to use to construct a string.
 *
 * @param fmt the format string used to construct a string.
 *
 * @return number of characters written, or -1 if an error occurs.
 *
 * @section syntax Format string syntax
 * The following tokens will be replaced with the specified string:
 *  \li \c \%w - This will be replaced with the name of the Watched file on
 *               which an event occurred.
 *  \li \c \%f - When an event occurs within a directory, this will be replaced
 *               with the name of the File which caused the event to occur.
 *               Otherwise, this will be replaced with an empty string.
 *  \li \c \%e - Replaced with the Event(s) which occurred, comma-separated.
 *  \li \c \%Xe - Replaced with the Event(s) which occurred, separated by
 *                whichever character is in the place of `X'.
 *  \li \c \%T - Replaced by the current Time in the format specified by the
 *               string previously passed to inotifytools_set_printf_timefmt(),
 *               or replaced with an empty string if that function has never
 *               been called.
 *
 * @section example Example
 * @code
 * // suppose this is the only file watched.
 * inotifytools_watch_file( "mydir/", IN_CLOSE );
 *
 * // wait until an event occurs
 * struct inotify_event * event = inotifytools_next_event( -1 );
 *
 * inotifytools_printf(stderr, event, "in %w, file %f had event(s): %.e\n");
 * // suppose the file 'myfile' in mydir was read from and closed.  Then,
 * // this prints to standard out something like:
 * // "in mydir/, file myfile had event(s): CLOSE_NOWRITE.CLOSE.ISDIR\n"
 * @endcode
 */
int inotifytools_printf( struct inotify_event* event, char* fmt ) {
	return inotifytools_fprintf( stdout, event, fmt );
}

/**
 * Print a string to a file using an inotify_event and a printf-like syntax.
 * The string written will only ever be up to 4096 characters in length.
 *
 * @param file file to print to
 *
 * @param event the event to use to construct a string.
 *
 * @param fmt the format string used to construct a string.
 *
 * @return number of characters written, or -1 if an error occurs.
 *
 * @section syntax Format string syntax
 * The following tokens will be replaced with the specified string:
 *  \li \c \%w - This will be replaced with the name of the Watched file on
 *               which an event occurred.
 *  \li \c \%f - When an event occurs within a directory, this will be replaced
 *               with the name of the File which caused the event to occur.
 *               Otherwise, this will be replaced with an empty string.
 *  \li \c \%e - Replaced with the Event(s) which occurred, comma-separated.
 *  \li \c \%Xe - Replaced with the Event(s) which occurred, separated by
 *                whichever character is in the place of `X'.
 *  \li \c \%T - Replaced by the current Time in the format specified by the
 *               string previously passed to inotifytools_set_printf_timefmt(),
 *               or replaced with an empty string if that function has never
 *               been called.
 *
 * @section example Example
 * @code
 * // suppose this is the only file watched.
 * inotifytools_watch_file( "mydir/", IN_CLOSE );
 *
 * // wait until an event occurs
 * struct inotify_event * event = inotifytools_next_event( -1 );
 *
 * inotifytools_fprintf(stderr, event, "in %w, file %f had event(s): %.e\n");
 * // suppose the file 'myfile' in mydir was read from and closed.  Then,
 * // this prints to standard error something like:
 * // "in mydir/, file myfile had event(s): CLOSE_NOWRITE.CLOSE.ISDIR\n"
 * @endcode
 */
int inotifytools_fprintf( FILE* file, struct inotify_event* event, char* fmt ) {
	static char out[MAX_STRLEN+1];
	static int ret;
	ret = inotifytools_sprintf( out, event, fmt );
	if ( -1 != ret ) fprintf( file, "%s", out );
	return ret;
}

/**
 * Construct a string using an inotify_event and a printf-like syntax.
 * The string can only ever be up to 4096 characters in length.
 *
 * This function will keep writing until it reaches 4096 characters.  If your
 * allocated array is not large enough to hold the entire string, your program
 * may crash.
 * inotifytools_snprintf() is safer and you should use it where possible.
 *
 * @param out location in which to store string.
 *
 * @param event the event to use to construct a string.
 *
 * @param fmt the format string used to construct a string.
 *
 * @return number of characters written, or -1 if an error occurs.
 *
 * @section syntax Format string syntax
 * The following tokens will be replaced with the specified string:
 *  \li \c \%w - This will be replaced with the name of the Watched file on
 *               which an event occurred.
 *  \li \c \%f - When an event occurs within a directory, this will be replaced
 *               with the name of the File which caused the event to occur.
 *               Otherwise, this will be replaced with an empty string.
 *  \li \c \%e - Replaced with the Event(s) which occurred, comma-separated.
 *  \li \c \%Xe - Replaced with the Event(s) which occurred, separated by
 *                whichever character is in the place of `X'.
 *  \li \c \%T - Replaced by the current Time in the format specified by the
 *               string previously passed to inotifytools_set_printf_timefmt(),
 *               or replaced with an empty string if that function has never
 *               been called.
 *
 * @section example Example
 * @code
 * // suppose this is the only file watched.
 * inotifytools_watch_file( "mydir/", IN_CLOSE );
 *
 * // wait until an event occurs
 * struct inotify_event * event = inotifytools_next_event( -1 );
 *
 * char mystring[1024];
 * // hope this doesn't crash - if filename is really long, might not fit into
 * // mystring!
 * inotifytools_sprintf(mystring, event, "in %w, file %f had event(s): %.e\n");
 * printf( mystring );
 * // suppose the file 'myfile' in mydir was written to and closed.  Then,
 * // this prints something like:
 * // "in mydir/, file myfile had event(s): CLOSE_WRITE.CLOSE.ISDIR\n"
 * @endcode
 */
int inotifytools_sprintf( char * out, struct inotify_event* event, char* fmt ) {
	return inotifytools_snprintf( out, MAX_STRLEN, event, fmt );
}


/**
 * Construct a string using an inotify_event and a printf-like syntax.
 * The string can only ever be up to 4096 characters in length.
 *
 * @param out location in which to store string.
 *
 * @param size maximum amount of characters to write.
 *
 * @param event the event to use to construct a string.
 *
 * @param fmt the format string used to construct a string.
 *
 * @return number of characters written, or -1 if an error occurs.
 *
 * @section syntax Format string syntax
 * The following tokens will be replaced with the specified string:
 *  \li \c \%w - This will be replaced with the name of the Watched file on
 *               which an event occurred.
 *  \li \c \%f - When an event occurs within a directory, this will be replaced
 *               with the name of the File which caused the event to occur.
 *               Otherwise, this will be replaced with an empty string.
 *  \li \c \%e - Replaced with the Event(s) which occurred, comma-separated.
 *  \li \c \%Xe - Replaced with the Event(s) which occurred, separated by
 *                whichever character is in the place of `X'.
 *  \li \c \%T - Replaced by the current Time in the format specified by the
 *               string previously passed to inotifytools_set_printf_timefmt(),
 *               or replaced with an empty string if that function has never
 *               been called.
 *
 * @section example Example
 * @code
 * // suppose this is the only file watched.
 * inotifytools_watch_file( "mydir/", IN_CLOSE );
 *
 * // wait until an event occurs
 * struct inotify_event * event = inotifytools_next_event( -1 );
 *
 * char mystring[1024];
 * inotifytools_snprintf( mystring, 1024, event,
 *                        "in %w, file %f had event(s): %.e\n" );
 * printf( mystring );
 * // suppose the file 'myfile' in mydir was written to and closed.  Then,
 * // this prints something like:
 * // "in mydir/, file myfile had event(s): CLOSE_WRITE.CLOSE.ISDIR\n"
 * @endcode
 */
int inotifytools_snprintf( char * out, int size,
                           struct inotify_event* event, char* fmt ) {
	static char * filename, * eventname, * eventstr;
	static unsigned int i, ind;
	static char ch1;
	static char timestr[MAX_STRLEN];
	static time_t now;


	if ( event->len > 0 ) {
		eventname = event->name;
	}
	else {
		eventname = NULL;
	}


	filename = inotifytools_filename_from_wd( event->wd );
	printf("filename %s\n",filename);
	hfind(&tab, filename);

	if ( !fmt || 0 == strlen(fmt) ) {
		error = EINVAL;
		return -1;
	}
	if ( strlen(fmt) > MAX_STRLEN || size > MAX_STRLEN) {
		error = EMSGSIZE;
		return -1;
	}

	ind = 0;
	for ( i = 0; i < strlen(fmt) &&
	             (int)ind < size - 1; ++i ) {
		if ( fmt[i] != '%' ) {
			out[ind++] = fmt[i];
			continue;
		}

		if ( i == strlen(fmt) - 1 ) {
			// last character is %, invalid
			error = EINVAL;
			return ind;
		}

		ch1 = fmt[i+1];

		if ( ch1 == '%' ) {
			out[ind++] = '%';
			++i;
			continue;
		}

		if ( ch1 == 'w' ) {
			if ( filename ) {
				strncpy( &out[ind], filename, size - ind );
				ind += strlen(filename);
			}
			++i;
			continue;
		}

		if ( ch1 == 'f' ) {
			if ( eventname ) {
				strncpy( &out[ind], eventname, size - ind );
				ind += strlen(eventname);
			}
			++i;
			continue;
		}

		if ( ch1 == 'e' ) {
			eventstr = inotifytools_event_to_str( event->mask );
			strncpy( &out[ind], eventstr, size - ind );
			ind += strlen(eventstr);
			++i;
			continue;
		}

		if ( ch1 == 'T' ) {

			if ( timefmt ) {

				now = time(0);
				if ( 0 >= strftime( timestr, MAX_STRLEN-1, timefmt,
				                    localtime( &now ) ) ) {

					// time format probably invalid
					error = EINVAL;
					return ind;
				}
			}
			else {
				timestr[0] = 0;
			}

			strncpy( &out[ind], timestr, size - ind );
			ind += strlen(timestr);
			++i;
			continue;
		}

		// Check if next char in fmt is e
		if ( i < strlen(fmt) - 2 && fmt[i+2] == 'e' ) {
			eventstr = inotifytools_event_to_str_sep( event->mask, ch1 );
			strncpy( &out[ind], eventstr, size - ind );
			ind += strlen(eventstr);
			i += 2;
			continue;
		}

		// OK, this wasn't a special format character, just output it as normal
		if ( ind < MAX_STRLEN ) out[ind++] = '%';
		if ( ind < MAX_STRLEN ) out[ind++] = ch1;
		++i;
	}
	out[ind] = 0;

	return ind - 1;
}

/**
 * Set time format for printf functions.
 *
 * @param fmt A format string valid for use with strftime, or NULL.  If NULL,
 *            time substitutions will no longer be made in printf functions.
 *            Note that this format string is not validated at all; using an
 *            incorrect format string will cause the printf functions to give
 *            incorrect results.
 */
void inotifytools_set_printf_timefmt( char * fmt ) {
	timefmt = fmt;
}

/**
 * Get the event queue size.
 *
 * This setting can also be read or modified by accessing the file
 * \a /proc/sys/fs/inotify/max_queued_events.
 *
 * @return the maximum number of events which will be queued in the kernel.
 */
int inotifytools_get_max_queued_events() {
	int ret;
	if ( !read_num_from_file( QUEUE_SIZE_PATH, &ret ) ) return -1;
	return ret;
}

/**
 * Get the maximum number of user instances of inotify.
 *
 * This setting can also be read or modified by accessing the file
 * \a /proc/sys/fs/inotify/max_user_instances.
 *
 * @return the maximum number of inotify file descriptors a single user can
 *         obtain.
 */
int inotifytools_get_max_user_instances() {
	int ret;
	if ( !read_num_from_file( INSTANCES_PATH, &ret ) ) return -1;
	return ret;
}

/**
 * Get the maximum number of user watches.
 *
 * This setting can also be read or modified by accessing the file
 * \a /proc/sys/fs/inotify/max_user_watches.
 *
 * @return the maximum number of inotify watches a single user can obtain per
 *         inotify instance.
 */
int inotifytools_get_max_user_watches() {
	int ret;
	if ( !read_num_from_file( WATCHES_SIZE_PATH, &ret ) ) return -1;
	return ret;
}

/**
 * Ignore inotify events matching a particular regular expression.
 *
 * @a pattern is a regular expression and @a flags is a bitwise combination of
 * POSIX regular expression flags.
 *
 * On future calls to inotifytools_next_events() or inotifytools_next_event(),
 * the regular expression is executed on the filename of files on which
 * events occur.  If the regular expression matches, the matched event will be
 * ignored.
 */
int inotifytools_ignore_events_by_regex( char const *pattern, int flags ) {
	if (!pattern) {
		if (regex) {
			regfree(regex);
			free(regex);
			regex = 0;
		}
		return 1;
	}

	if (regex) { regfree(regex); }
	else       { regex = (regex_t *)malloc(sizeof(regex_t)); }

	int ret = regcomp(regex, pattern, flags | REG_NOSUB);
	if (0 == ret) return 1;

	regfree(regex);
	free(regex);
	regex = 0;
	error = EINVAL;
	return 0;
}

int event_compare(const void *p1, const void *p2, const void *config)
{
	if (!p1 || !p2) return p1 - p2;
	char asc = 1;
	int sort_event = (int)config;
	if (sort_event == -1) {
		sort_event = 0;
		asc = 0;
	} else if (sort_event < 0) {
		sort_event = -sort_event;
		asc = 0;
	}
	int *i1 = stat_ptr((watch*)p1, sort_event);
	int *i2 = stat_ptr((watch*)p2, sort_event);
	if (0 == *i1 - *i2) {
		return ((watch*)p1)->wd - ((watch*)p2)->wd;
	}
	if (asc)
		return *i1 - *i2;
	else
		return *i2 - *i1;
}

struct rbtree *inotifytools_wd_sorted_by_event(int sort_event)
{
	struct rbtree *ret = rbinit(event_compare, (void*)sort_event);
	RBLIST *all = rbopenlist(tree_wd);
	void const *p = rbreadlist(all);
	while (p) {
		void const *r = rbsearch(p, ret);
		niceassert((int)(r == p), "Couldn't insert watch into new tree");
		p = rbreadlist(all);
	}
	rbcloselist(all);
	return ret;
}

