/*
 Copyright (C) 1999-2004 IC & S  dbmail@ic-s.nl
 Copyright (c) 2004-2006 NFG Net Facilities Group BV support@nfg.nl

 This program is free software; you can redistribute it and/or 
 modify it under the terms of the GNU General Public License 
 as published by the Free Software Foundation; either 
 version 2 of the License, or (at your option) any later 
 version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/* 
 * imap4.c
 *
 * implements an IMAP 4 rev 1 server.
 */

#include "dbmail.h"

#define THIS_MODULE "imap"

/* max number of BAD/NO responses */
#define MAX_FAULTY_RESPONSES 5

const char *IMAP_COMMANDS[] = {
	"", "capability", "noop", "logout",
	"authenticate", "login",
	"select", "examine", "create", "delete", "rename", "subscribe",
	"unsubscribe",
	"list", "lsub", "status", "append",
	"check", "close", "expunge", "search", "fetch", "store", "copy",
	"uid", "sort", "getquotaroot", "getquota",
	"setacl", "deleteacl", "getacl", "listrights", "myrights",
	"namespace","thread","unselect","idle",
	"***NOMORE***"
};

extern serverConfig_t *server_conf;
extern GAsyncQueue *queue;

const char AcceptedTagChars[] =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    "!@#$%^&-=_`~\\|'\" ;:,.<>/? ";


const IMAP_COMMAND_HANDLER imap_handler_functions[] = {
	NULL,
	_ic_capability, _ic_noop, _ic_logout,
	_ic_authenticate, _ic_login,
	_ic_select, _ic_examine, _ic_create, _ic_delete, _ic_rename,
	_ic_subscribe, _ic_unsubscribe, _ic_list, _ic_lsub, _ic_status,
	_ic_append,
	_ic_check, _ic_close, _ic_expunge, _ic_search, _ic_fetch,
	_ic_store, _ic_copy, _ic_uid, _ic_sort,
	_ic_getquotaroot, _ic_getquota,
	_ic_setacl, _ic_deleteacl, _ic_getacl, _ic_listrights,
	_ic_myrights,
	_ic_namespace, _ic_thread, _ic_unselect, _ic_idle,
	NULL
};


/*
 */

static int imap4_tokenizer(ImapSession *, char *);
static int imap4(ImapSession *);

static void imap_session_bailout(ImapSession *session)
{
	// brute force:
	if (server_conf->no_daemonize == 1) _exit(0);

	assert(session && session->ci);
	TRACE(TRACE_DEBUG,"[%p] state [%d]", session, session->state);

	if ( session->command_type == IMAP_COMM_IDLE ) { // session is in a IDLE loop - need to exit the loop first
		TRACE(TRACE_DEBUG, "[%p] Session is in an idle loop, exiting loop.", session);
		session->command_state = FALSE;
		dm_thread_data *D = g_new0(dm_thread_data,1);
		D->data = (gpointer)"DONE\n\0";
		g_async_queue_push(session->ci->queue, (gpointer)D);
		usleep(25000);
	}

	ci_close(session->ci);
	dbmail_imap_session_delete(session);
}

void socket_write_cb(int fd UNUSED, short what UNUSED, void *arg)
{
	ImapSession *session = (ImapSession *)arg;

	TRACE(TRACE_DEBUG,"[%p] state [%d] command_state [%d]", session, session->state, session->command_state);

	switch(session->state) {
		case IMAPCS_LOGOUT:
			event_del(session->ci->wev);
		case IMAPCS_ERROR:
			imap_session_bailout(session);
			break;
		default:
			if (session->ci->rev) {
				if ( session->command_type == IMAP_COMM_IDLE ) {
					if ( (session->command_state == FALSE) && (session->loop++ < 1) ) {
						// make _very_ sure this is done only once during an idle command run
						// only when the idle loop has just begun: just after we pushed the
						// continuation '+' to the client
						session->command_state = IDLE;
						event_add(session->ci->rev, NULL);
					} else if (session->command_state == TRUE)  { // IDLE is done
						event_add(session->ci->rev, session->ci->timeout);
					}

				} else if ( (! session->parser_state) || session->command_state == TRUE) {
					event_add(session->ci->rev, session->ci->timeout);
				}
			}

			if (session->ci->wev)
				ci_write(session->ci,NULL);

			break;
	}
}

void socket_read_cb(int fd UNUSED, short what UNUSED, void *arg)
{
	ImapSession *session = (ImapSession *)arg;
	TRACE(TRACE_DEBUG,"[%p] state [%d] command_state [%d]", session, session->state, session->command_state);
	session->ci->cb_read(session);
}

/* 
 * only the main thread may write to the network event
 * worker threads must use an async queue
 */
static int imap_session_printf(ImapSession * self, char * message, ...)
{
        va_list ap;
        size_t l;

        assert(message);
        va_start(ap, message);
        g_string_vprintf(self->buff, message, ap);
        va_end(ap);

	ci_write(self->ci, self->buff->str);

        l = self->buff->len;
	self->buff = g_string_truncate(self->buff, 0);

        return (int)l;
}

static void send_greeting(ImapSession *session)
{
	/* greet user */
	field_t banner;
	GETCONFIGVALUE("banner", "IMAP", banner);
	if (strlen(banner) > 0)
		imap_session_printf(session, "* OK %s\r\n", banner);
	else
		imap_session_printf(session, "* OK imap 4r1 server (dbmail %s)\r\n", VERSION);
	dbmail_imap_session_set_state(session,IMAPCS_NON_AUTHENTICATED);
}

/*
 * the default timeout callback */
void imap_cb_time(void *arg)
{
	ImapSession *session = (ImapSession *) arg;
	TRACE(TRACE_DEBUG,"[%p]", session);

	imap_session_printf(session, "%s", IMAP_TIMEOUT_MSG);
	dbmail_imap_session_set_state(session,IMAPCS_ERROR);
}

static int checktag(const char *s)
{
	int i;
	for (i = 0; s[i]; i++) {
		if (!strchr(AcceptedTagChars, s[i])) return 0;
	}
	return 1;
}

static size_t stridx(const char *s, char c)
{
	size_t i;
	for (i = 0; s[i] && s[i] != c; i++);
	return i;
}

static void imap_handle_exit(ImapSession *session, int status)
{
	TRACE(TRACE_DEBUG, "[%p] state [%d] command_status [%d] [%s] returned with status [%d]", 
		session, session->state, session->command_state, session->command, status);

	switch(status) {
		case -1:
			dbmail_imap_session_set_state(session,IMAPCS_ERROR);	/* fatal error occurred, kick this user */
			break;

		case 0:
			/* only do this in the main thread */
			if (session->state < IMAPCS_LOGOUT) {
				if (session->buff->len) {
					ci_write(session->ci, session->buff->str);
					dbmail_imap_session_buff_clear(session);
				}
				if (session->command_state == TRUE)
					event_add(session->ci->rev, session->ci->timeout);
			} else {
				dbmail_imap_session_buff_clear(session);
			}				

			break;
		case 1:
			dbmail_imap_session_buff_flush(session);
			session->error_count++;	/* server returned BAD or NO response */

			break;

		case 2:
			/* only do this in the main thread */
			imap_session_printf(session, "* BYE\r\n");
			imap_session_printf(session, "%s OK LOGOUT completed\r\n", session->tag);
			break;
	}
}


void imap_cb_read(void *arg)
{
	ImapSession *session = (ImapSession *) arg;
	char buffer[MAX_LINESIZE];
	int result;
	int l;

	// disable read events until we're done
	event_del(session->ci->rev);

	memset(buffer, 0, sizeof(buffer));	// have seen dirty buffers with out this

	if (session->state == IMAPCS_ERROR) {
		TRACE(TRACE_DEBUG, "session->state: ERROR. abort");
		return;
	}
	if (session->command_state==TRUE) dbmail_imap_session_reset(session);

	// Drain input buffer else return to wait for more.
	// Read in a line at a time if we don't have a string literal size defined
	// Otherwise read in sizeof(buffer) [64KB] or the remaining rbuff_size if less
	if (session->rbuff_size <= 0) {
		l = ci_readln(session->ci, buffer);
	} else {
		int needed = (session->rbuff_size < (int)sizeof(buffer)) ? session->rbuff_size : (int)sizeof(buffer);
		l = ci_read(session->ci, buffer, needed);
	}

	if (l == 0) {
		if (session->rbuff_size > 0)
			TRACE(TRACE_DEBUG,"last read [%d], still need [%d]", l, session->rbuff_size);
		return;
	}

	if (l < 0) {
		dbmail_imap_session_set_state(session,IMAPCS_ERROR);
		return;
	}

	if (session->error_count >= MAX_FAULTY_RESPONSES) {
		imap_session_printf(session, "* BYE [TRY RFC]\r\n");
		dbmail_imap_session_set_state(session,IMAPCS_ERROR);
		return;
	}

	if ( session->command_type == IMAP_COMM_IDLE ) { // session is in a IDLE loop
		TRACE(TRACE_DEBUG,"read [%s] while in IDLE loop", buffer);
		session->command_state = FALSE;
		dm_thread_data *D = g_new0(dm_thread_data,1);
		D->data = (gpointer)g_strdup(buffer);
		g_async_queue_push(session->ci->queue, (gpointer)D);
		return;
	}

	if (! imap4_tokenizer(session, buffer)) {
		event_add(session->ci->rev, session->ci->timeout);
		return;
	}

	if (! session->parser_state)
		return;

	if ((result = imap4(session))) {
		imap_handle_exit(session, result);
	}

	return;

	
}

void dbmail_imap_session_set_callbacks(ImapSession *session, void *r, void *t, int timeout)
{
	if (r) session->ci->cb_read = r;
	if (t) session->ci->cb_time = t;
	if (timeout>0) session->ci->timeout->tv_sec = (time_t)timeout;

	assert(session->ci->cb_read);
	assert(session->ci->cb_time);
	assert(session->ci->timeout->tv_sec > 0);

	TRACE(TRACE_DEBUG,"session [%p], cb_read [%p], cb_time [%p], timeout [%d]", 
		session, session->ci->cb_read, session->ci->cb_time, (int)session->ci->timeout->tv_sec);

	UNBLOCK(session->ci->rx);
	UNBLOCK(session->ci->tx);

	event_add(session->ci->rev, session->ci->timeout );
	event_add(session->ci->wev, NULL);
}

int imap_handle_connection(client_sock *c)
{
	ImapSession *session;
	clientbase_t *ci;

	if (c)
		ci = client_init(c->sock, c->caddr);
	else
		ci = client_init(0, NULL);

	session = dbmail_imap_session_new();

	dbmail_imap_session_set_state(session, IMAPCS_NON_AUTHENTICATED);

	event_set(ci->rev, ci->rx, EV_READ, socket_read_cb, (void *)session);
	event_set(ci->wev, ci->tx, EV_WRITE, socket_write_cb, (void *)session);

	session->ci = ci;

	dbmail_imap_session_set_callbacks(session, imap_cb_read, imap_cb_time, server_conf->login_timeout);
	
	send_greeting(session);
	
	return EOF;
}

void dbmail_imap_session_reset(ImapSession *session)
{
	TRACE(TRACE_DEBUG,"[%p]", session);
	if (session->tag) {
		g_free(session->tag);
		session->tag = NULL;
	}
	if (session->command) {
		g_free(session->command);
		session->command = NULL;
	}

	session->rbuff = NULL;
	session->use_uid = 0;
	session->command_type = 0;
	session->command_state = FALSE;
	dbmail_imap_session_args_free(session, FALSE);
	
	return;
}

int imap4_tokenizer (ImapSession *session, char *buffer)
{
	char *tag = NULL, *cpy, *command;
	size_t i = 0;
		
	if (!(*buffer))
		return 0;

	/* read tag & command */
	cpy = buffer;

	/* fetch the tag and command */
	if (! session->tag) {

		if (strcmp(buffer,"\n")==0)
			return 0;

		session->parser_state = 0;
		TRACE(TRACE_INFO, "[%p] COMMAND: [%s]", session, buffer);

		// tag
		i = stridx(cpy, ' ');	/* find next space */
		if (i == strlen(cpy)) {
			if (checktag(cpy))
				imap_session_printf(session, "%s BAD No command specified\r\n", cpy);
			else
				imap_session_printf(session, "* BAD Invalid tag specified\r\n");
			session->error_count++;
			session->command_state=TRUE;
			return 0;
		}

		tag = g_strndup(cpy,i);	/* set tag */
		dbmail_imap_session_set_tag(session,tag);
		g_free(tag);

		cpy[i] = '\0';
		cpy = cpy + i + 1;	/* cpy points to command now */

		if (!checktag(session->tag)) {
			imap_session_printf(session, "* BAD Invalid tag specified\r\n");
			session->error_count++;
			session->command_state=TRUE;
			return 0;
		}

		// command
		i = stridx(cpy, ' ');	/* find next space */

		command = g_strndup(cpy,i);	/* set command */
		if (command[i-1] == '\n') command[i-1] = '\0';
		dbmail_imap_session_set_command(session,command);
		g_free(command);

		cpy = cpy + i;	/* cpy points to args now */

	}

	session->parser_state = build_args_array_ext(session, cpy);	/* build argument array */

	if (session->parser_state)
		TRACE(TRACE_DEBUG,"parser_state: [%d]", session->parser_state);

	return 1;
}
	
void _ic_cb_leave(gpointer data)
{
	dm_thread_data *D = (dm_thread_data *)data;
	ImapSession *session = D->session;
	imap_handle_exit(session, D->status);
}


int imap4(ImapSession *session)
{
	// 
	// the parser/tokenizer is satisfied we're ready reading from the client
	// so now it's time to act upon the read input
	//
	int j = 0;
	
	if (! dm_db_ping()) {
		dbmail_imap_session_set_state(session,IMAPCS_ERROR);
		return DM_EQUERY;
	}

	if (session->command_state==TRUE) // done already
		return 0;

	session->command_state=TRUE; // set command-is-done-state while doing some checks
	if (! (session->tag && session->command)) {
		TRACE(TRACE_ERR,"no tag or command");
		return 1;
	}
	if (! session->args) {
		imap_session_printf(session, "%s BAD invalid argument specified\r\n",session->tag);
		session->error_count++;
		return 1;
	}

	/* lookup the command */
	for (j = IMAP_COMM_NONE; j < IMAP_COMM_LAST && strcasecmp(session->command, IMAP_COMMANDS[j]); j++);

	if (j <= IMAP_COMM_NONE || j >= IMAP_COMM_LAST) { /* unknown command */
		imap_session_printf(session, "%s BAD no valid command\r\n", session->tag);
		return 1;
	}

	session->error_count = 0;
	session->command_type = j;
	session->command_state=FALSE; // unset command-is-done-state while command in progress

	TRACE(TRACE_INFO, "dispatch [%s]...\n", IMAP_COMMANDS[session->command_type]);
	return (*imap_handler_functions[session->command_type]) (session);
}
