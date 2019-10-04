/*
 * websocket - WebSocket support (RFC6455)
 * (C)Copyright 2016 Bram Matthys and the UnrealIRCd team
 * License: GPLv2
 * This module was sponsored by Aberrant Software Inc.
 */
   
#include "unrealircd.h"
#include <limits.h>

#define WEBSOCKET_VERSION "1.0.0"

ModuleHeader MOD_HEADER
  = {
	"websocket",
	WEBSOCKET_VERSION,
	"WebSocket support (RFC6455)",
	"UnrealIRCd Team",
	"unrealircd-5",
    };

#if CHAR_MIN < 0
 #error "In UnrealIRCd char should always be unsigned. Check your compiler"
#endif

typedef struct WebSocketUser WebSocketUser;
struct WebSocketUser {
	char get; /**< GET initiated */
	char handshake_completed; /**< Handshake completed, use data frames */
	char *handshake_key; /**< Handshake key (used during handshake) */
	char *lefttoparse; /**< Leftover buffer to parse */
	int lefttoparselen; /**< Length of lefttoparse buffer */
};

#define WEBSOCKET_TYPE_BINARY	0x1
#define WEBSOCKET_TYPE_TEXT	0x2

#define WSU(cptr)	((WebSocketUser *)moddata_client(cptr, websocket_md).ptr)

#define WEBSOCKET_TYPE(cptr)	((cptr->local && cptr->local->listener) ? cptr->local->listener->websocket_options : 0)

#define WEBSOCKET_MAGIC_KEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" /* see RFC6455 */

#define WSOP_CONTINUATION 0x00
#define WSOP_TEXT         0x01
#define WSOP_BINARY       0x02
#define WSOP_CLOSE        0x08
#define WSOP_PING         0x09
#define WSOP_PONG         0x0a

/* Forward declarations */
int websocket_config_test(ConfigFile *cf, ConfigEntry *ce, int type, int *errs);
int websocket_config_run_ex(ConfigFile *cf, ConfigEntry *ce, int type, void *ptr);
int websocket_packet_out(Client *from, Client *to, Client *intended_to, char **msg, int *length);
int websocket_packet_in(Client *sptr, char *readbuf, int *length);
void websocket_mdata_free(ModData *m);
int websocket_handle_packet(Client *sptr, char *readbuf, int length);
int websocket_handle_handshake(Client *sptr, char *readbuf, int *length);
int websocket_complete_handshake(Client *sptr);
int websocket_handle_packet_ping(Client *sptr, char *buf, int len);
int websocket_handle_packet_pong(Client *sptr, char *buf, int len);
int websocket_create_frame(int opcode, char **buf, int *len);
int websocket_send_frame(Client *sptr, int opcode, char *buf, int len);

/* Global variables */
ModDataInfo *websocket_md;

MOD_TEST()
{
	HookAdd(modinfo->handle, HOOKTYPE_CONFIGTEST, 0, websocket_config_test);
	return MOD_SUCCESS;
}

MOD_INIT()
{
	ModDataInfo mreq;

	MARK_AS_OFFICIAL_MODULE(modinfo);

	HookAdd(modinfo->handle, HOOKTYPE_CONFIGRUN_EX, 0, websocket_config_run_ex);
	HookAdd(modinfo->handle, HOOKTYPE_PACKET, 0, websocket_packet_out);
	HookAdd(modinfo->handle, HOOKTYPE_RAWPACKET_IN, 0, websocket_packet_in);

	memset(&mreq, 0, sizeof(mreq));
	mreq.name = "websocket";
	mreq.serialize = NULL;
	mreq.unserialize = NULL;
	mreq.free = websocket_mdata_free;
	mreq.sync = 0;
	mreq.type = MODDATATYPE_CLIENT;
	websocket_md = ModDataAdd(modinfo->handle, mreq);

	return MOD_SUCCESS;
}

MOD_LOAD()
{
	if (SHOWCONNECTINFO)
	{
		config_warn("I'm disabling set::options::show-connect-info for you "
		            "as this setting is incompatible with the websocket module.");
		SHOWCONNECTINFO = 0;
	}

	return MOD_SUCCESS;
}

MOD_UNLOAD()
{
	return MOD_SUCCESS;
}

#ifndef CheckNull
 #define CheckNull(x) if ((!(x)->ce_vardata) || (!(*((x)->ce_vardata)))) { config_error("%s:%i: missing parameter", (x)->ce_fileptr->cf_filename, (x)->ce_varlinenum); errors++; continue; }
#endif

int websocket_config_test(ConfigFile *cf, ConfigEntry *ce, int type, int *errs)
{
	int errors = 0;
	ConfigEntry *cep;
	int has_type = 0;
	static char errored_once_nick = 0;

	if (type != CONFIG_LISTEN_OPTIONS)
		return 0;

	/* We are only interrested in listen::options::websocket.. */
	if (!ce || !ce->ce_varname || strcmp(ce->ce_varname, "websocket"))
		return 0;

	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strcmp(cep->ce_varname, "type"))
		{
			CheckNull(cep);
			has_type = 1;
			if (!strcmp(cep->ce_vardata, "text"))
			{
				if (non_utf8_nick_chars_in_use && !errored_once_nick)
				{
					/* This one is a hard error, since the consequences are grave */
					config_error("You have a websocket listener with type 'text' AND your set::allowed-nickchars contains at least one non-UTF8 character set.");
					config_error("This is a very BAD idea as this makes your websocket vulnerable to UTF8 conversion attacks. "
					             "This can cause things like unkickable users and 'ghosts' for websocket users.");
					config_error("You have 4 options: 1) Remove the websocket listener, 2) Use websocket type 'binary', "
					             "3) Remove the non-UTF8 character set from set::allowed-nickchars, 4) Replace the non-UTF8 with an UTF8 character set in set::allowed-nickchars");
					config_error("For more details see https://www.unrealircd.org/docs/WebSocket_support#websockets-and-non-utf8");
					errored_once_nick = 1;
					errors++;
				}
			}
			else if (!strcmp(cep->ce_vardata, "binary"))
			{
			}
			else
			{
				config_error("%s:%i: listen::options::websocket::type must be either 'binary' or 'text' (not '%s')",
					cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_vardata);
				errors++;
			}
		} else
		{
			config_error("%s:%i: unknown directive listen::options::websocket::%s",
				cep->ce_fileptr->cf_filename, cep->ce_varlinenum, cep->ce_varname);
			errors++;
			continue;
		}
	}

	if (!has_type)
	{
		config_error("%s:%i: websocket set, but type unspecified. Use something like: listen { ip *; port 443; websocket { type text; } }",
			ce->ce_fileptr->cf_filename, ce->ce_varlinenum);
		errors++;
	}

	*errs = errors;
	return errors ? -1 : 1;
}

int websocket_config_run_ex(ConfigFile *cf, ConfigEntry *ce, int type, void *ptr)
{
	ConfigEntry *cep, *cepp;
	ConfigItem_listen *l;
	static char warned_once_channel = 0;

	if (type != CONFIG_LISTEN_OPTIONS)
		return 0;

	/* We are only interrested in listen::options::websocket.. */
	if (!ce || !ce->ce_varname || strcmp(ce->ce_varname, "websocket"))
		return 0;

	l = (ConfigItem_listen *)ptr;

	for (cep = ce->ce_entries; cep; cep = cep->ce_next)
	{
		if (!strcmp(cep->ce_varname, "type"))
		{
			if (!strcmp(cep->ce_vardata, "binary"))
				l->websocket_options = WEBSOCKET_TYPE_BINARY;
			else if (!strcmp(cep->ce_vardata, "text"))
			{
				l->websocket_options = WEBSOCKET_TYPE_TEXT;
				if ((tempiConf.allowed_channelchars == ALLOWED_CHANNELCHARS_ANY) && !warned_once_channel)
				{
					/* This one is a warning, since the consequences are less grave than with nicks */
					config_warn("You have a websocket listener with type 'text' AND your set::allowed-channelchars is set to 'any'.");
					config_warn("This is not a recommended combination as this makes your websocket vulnerable to UTF8 conversion attacks. "
					            "This can cause things like unpartable channels for websocket users.");
					config_warn("It is highly recommended that you use set { allowed-channelchars utf8; }");
					config_warn("For more details see https://www.unrealircd.org/docs/WebSocket_support#websockets-and-non-utf8");
					warned_once_channel = 1;
				}
			}
		}
	}
	return 1;
}

/** UnrealIRCd internals: free WebSocketUser object. */
void websocket_mdata_free(ModData *m)
{
	WebSocketUser *wsu = (WebSocketUser *)m->ptr;
	if (wsu)
	{
		safe_free(wsu->handshake_key);
		safe_free(wsu->lefttoparse);
		safe_free(m->ptr);
	}
}

/** Outgoing packet hook.
 * This transforms the output to be Websocket-compliant, if necessary.
 */
int websocket_packet_out(Client *from, Client *to, Client *intended_to, char **msg, int *length)
{
	if (MyConnect(to) && WSU(to) && WSU(to)->handshake_completed)
	{
		if (WEBSOCKET_TYPE(to) == WEBSOCKET_TYPE_BINARY)
			websocket_create_frame(WSOP_BINARY, msg, length);
		else if (WEBSOCKET_TYPE(to) == WEBSOCKET_TYPE_TEXT)
		{
			/* Some more conversions are needed */
			char *safe_msg = unrl_utf8_make_valid(*msg);
			*msg = safe_msg;
			*length = *msg ? strlen(safe_msg) : 0;
			websocket_create_frame(WSOP_TEXT, msg, length);
		}
		return 0;
	}
	return 0;
}

int websocket_handle_websocket(Client *sptr, char *readbuf2, int length2)
{
	int n;
	char *ptr;
	int length;
	int length1 = WSU(sptr)->lefttoparselen;
	char readbuf[4096];

	length = length1 + length2;
	if (length > sizeof(readbuf)-1)
	{
		dead_socket(sptr, "Illegal buffer stacking/Excess flood");
		return 0;
	}

	if (length1 > 0)
		memcpy(readbuf, WSU(sptr)->lefttoparse, length1);
	memcpy(readbuf+length1, readbuf2, length2);

	safe_free(WSU(sptr)->lefttoparse);
	WSU(sptr)->lefttoparselen = 0;

	ptr = readbuf;
	do {
		n = websocket_handle_packet(sptr, ptr, length);
		if (n < 0)
			return -1; /* killed -- STOP processing */
		if (n == 0)
		{
			/* Short read. Stop processing for now, but save data for next time */
			safe_free(WSU(sptr)->lefttoparse);
			WSU(sptr)->lefttoparse = safe_alloc(length);
			WSU(sptr)->lefttoparselen = length;
			memcpy(WSU(sptr)->lefttoparse, ptr, length);
			return 0;
		}
		length -= n;
		ptr += n;
		if (length < 0)
			abort(); /* less than 0 is impossible */
	} while(length > 0);

	return 0;
}

/** Incoming packet hook.
 * This processes Websocket frames, if this is a websocket connection.
 * NOTE The different return values:
 * -1 means: don't touch this client anymore, it has or might have been killed!
 * 0 means: don't process this data, but you can read another packet if you want
 * >0 means: process this data (regular IRC data, non-websocket stuff)
 */
int websocket_packet_in(Client *sptr, char *readbuf, int *length)
{
	if ((sptr->local->receiveM == 0) && WEBSOCKET_TYPE(sptr) && !WSU(sptr) && (*length > 8) && !strncmp(readbuf, "GET ", 4))
	{
		/* Allocate a new WebSocketUser struct for this session */
		moddata_client(sptr, websocket_md).ptr = safe_alloc(sizeof(WebSocketUser));
		WSU(sptr)->get = 1;
	}

	if (!WSU(sptr))
		return 1; /* "normal" IRC client */

	if (WSU(sptr)->handshake_completed)
		return websocket_handle_websocket(sptr, readbuf, *length);
	/* else.. */
	return websocket_handle_handshake(sptr, readbuf, length);
}

/** Helper function to parse the HTTP header consisting of multiple 'Key: value' pairs */
int websocket_handshake_helper(char *buffer, int len, char **key, char **value, char **lastloc, int *end_of_request)
{
	static char buf[4096], *nextptr;
	char *p;
	char *k = NULL, *v = NULL;
	int foundlf = 0;

	if (buffer)
	{
		/* Initialize */
		if (len > sizeof(buf) - 1)
			len = sizeof(buf) - 1;

		memcpy(buf, buffer, len);
		buf[len] = '\0';
		nextptr = buf;
	}

	*end_of_request = 0;

	p = nextptr;

	if (!p)
	{
		*key = *value = NULL;
		return 0; /* done processing data */
	}

	if (!strncmp(p, "\n", 1) || !strncmp(p, "\r\n", 2))
	{
		*key = *value = NULL;
		*end_of_request = 1;
		return 0;
	}

	/* Note: p *could* point to the NUL byte ('\0') */

	/* Special handling for GET line itself. */
	if (!strncmp(p, "GET ", 4))
	{
		k = "GET";
		p += 4;
		v = p; /* SET VALUE */
		nextptr = NULL; /* set to "we are done" in case next for loop fails */
		for (; *p; p++)
		{
			if (*p == ' ')
			{
				*p = '\0'; /* terminate before "HTTP/1.X" part */
			}
			else if (*p == '\r')
			{
				*p = '\0'; /* eat silently, but don't consider EOL */
			}
			else if (*p == '\n')
			{
				*p = '\0';
				nextptr = p+1; /* safe, there is data or at least a \0 there */
				break;
			}
		}
		*key = k;
		*value = v;
		return 1;
	}

	/* Header parsing starts here.
	 * Example line "Host: www.unrealircd.org"
	 */
	k = p; /* SET KEY */

	/* First check if the line contains a terminating \n. If not, don't process it
	 * as it may have been a cut header.
	 */
	for (; *p; p++)
	{
		if (*p == '\n')
		{
			foundlf = 1;
			break;
		}
	}

	if (!foundlf)
	{
		*key = *value = NULL;
		*lastloc = k;
		return 0;
	}

	p = k;

	for (; *p; p++)
	{
		if ((*p == '\n') || (*p == '\r'))
		{
			/* Reached EOL but 'value' not found */
			*p = '\0';
			break;
		}
		if (*p == ':')
		{
			*p++ = '\0';
			if (*p++ != ' ')
				break; /* missing mandatory space after ':' */

			v = p; /* SET VALUE */
			nextptr = NULL; /* set to "we are done" in case next for loop fails */
			for (; *p; p++)
			{
				if (*p == '\r')
				{
					*p = '\0'; /* eat silently, but don't consider EOL */
				}
				else if (*p == '\n')
				{
					*p = '\0';
					nextptr = p+1; /* safe, there is data or at least a \0 there */
					break;
				}
			}
			/* A key-value pair was succesfully parsed, return it */
			*key = k;
			*value = v;
			return 1;
		}
	}

	/* Fatal parse error */
	*key = *value = NULL;
	return 0;
}

/** Handle client GET WebSocket handshake.
 * Yes, I'm going to assume that the header fits in one packet and one packet only.
 */
int websocket_handle_handshake(Client *sptr, char *readbuf, int *length)
{
	char *key, *value;
	int r, end_of_request;
	char netbuf[2048];
	char *lastloc = NULL;
	int n, maxcopy, nprefix=0;

	/** Frame re-assembling starts here **/
	*netbuf = '\0';
	if (WSU(sptr)->lefttoparse)
	{
		strlcpy(netbuf, WSU(sptr)->lefttoparse, sizeof(netbuf));
		nprefix = strlen(netbuf);
	}
	maxcopy = sizeof(netbuf) - nprefix - 1;
	/* (Need to some manual checking here as strlen() can't be safely used
	 *  on readbuf. Same is true for strlncat since it uses strlen().)
	 */
	n = *length;
	if (n > maxcopy)
		n = maxcopy;
	if (n <= 0)
	{
		dead_socket(sptr, "Oversized line");
		return -1;
	}
	memcpy(netbuf+nprefix, readbuf, n); /* SAFE: see checking above */
	netbuf[n+nprefix] = '\0';
	safe_free(WSU(sptr)->lefttoparse);

	/** Now step through the lines.. **/
	for (r = websocket_handshake_helper(netbuf, strlen(netbuf), &key, &value, &lastloc, &end_of_request);
	     r;
	     r = websocket_handshake_helper(NULL, 0, &key, &value, &lastloc, &end_of_request))
	{
		if (!strcasecmp(key, "Sec-WebSocket-Key"))
		{
			if (strchr(value, ':'))
			{
				/* This would cause unserialization issues. Should be base64 anyway */
				dead_socket(sptr, "Invalid characters in Sec-WebSocket-Key");
				return -1;
			}
			safe_strdup(WSU(sptr)->handshake_key, value);
		}
	}

	if (end_of_request)
	{
		if (!WSU(sptr)->handshake_key)
		{
			if (is_module_loaded("webredir"))
			{
				char *parx[2] = { NULL, NULL };
				do_cmd(sptr, NULL, "GET", 1, parx);
			}
			dead_socket(sptr, "Invalid WebSocket request");
			return -1;
		}
		websocket_complete_handshake(sptr);
		return 0;
	}

	if (lastloc)
	{
		/* Last line was cut somewhere, save it for next round. */
		safe_strdup(WSU(sptr)->lefttoparse, lastloc);
	}
	return 0; /* don't let UnrealIRCd process this */
}

/** Complete the handshake by sending the appropriate HTTP 101 response etc. */
int websocket_complete_handshake(Client *sptr)
{
	char buf[512], hashbuf[64];
	SHA_CTX hash;
	char sha1out[20]; /* 160 bits */

	WSU(sptr)->handshake_completed = 1;

	snprintf(buf, sizeof(buf), "%s%s", WSU(sptr)->handshake_key, WEBSOCKET_MAGIC_KEY);
	SHA1_Init(&hash);
	SHA1_Update(&hash, buf, strlen(buf));
	SHA1_Final(sha1out, &hash);

	b64_encode(sha1out, sizeof(sha1out), hashbuf, sizeof(hashbuf));

	snprintf(buf, sizeof(buf),
	         "HTTP/1.1 101 Switching Protocols\r\n"
	         "Upgrade: websocket\r\n"
	         "Connection: Upgrade\r\n"
	         "Sec-WebSocket-Accept: %s\r\n"
	         "\r\n",
	         hashbuf);

	/* Caution: we bypass sendQ flood checking by doing it this way.
	 * Risk is minimal, though, as we only permit limited text only
	 * once per session.
	 */
	dbuf_put(&sptr->local->sendQ, buf, strlen(buf));
	send_queued(sptr);

	return 0;
}

/* Add LF (if needed) to a buffer. Max 4K. */
void add_lf_if_needed(char **buf, int *len)
{
	static char newbuf[4096];
	char *b = *buf;
	int l = *len;

	if (l <= 0)
		return; /* too short */

	if (b[l - 1] == '\n')
		return; /* already contains \n */

	if (l >= sizeof(newbuf)-2)
		l = sizeof(newbuf)-2; /* cut-off if necessary */

	memcpy(newbuf, b, l);
	newbuf[l] = '\n';
	newbuf[l + 1] = '\0'; /* not necessary, but I like zero termination */
	l++;
	*buf = newbuf; /* new buffer */
	*len = l; /* new length */
}

/** WebSocket packet handler.
 * For more information on the format, check out page 28 of RFC6455.
 * @returns The number of bytes processed (the size of the frame)
 *          OR 0 to indicate a possible short read (want more data)
 *          OR -1 in case of an error.
 */
int websocket_handle_packet(Client *sptr, char *readbuf, int length)
{
	char opcode; /**< Opcode */
	char masked; /**< Masked */
	int len; /**< Length of the packet */
	char maskkey[4]; /**< Key used for masking */
	char *p, *payload;
	int total_packet_size;

	if (length < 4)
	{
		/* WebSocket packet too short */
		return 0;
	}

	/* fin    = readbuf[0] & 0x80; -- unused */
	opcode = readbuf[0] & 0x7F;
	masked = readbuf[1] & 0x80;
	len    = readbuf[1] & 0x7F;
	p = &readbuf[2]; /* point to next element */

	/* actually 'fin' is unused.. we don't care. */

	if (!masked)
	{
		dead_socket(sptr, "WebSocket packet not masked");
		return -1; /* Having the masked bit set is required (RFC6455 p29) */
	}

	if (len == 127)
	{
		dead_socket(sptr, "WebSocket packet with insane size");
		return -1; /* Packets requiring 64bit lengths are not supported. Would be insane. */
	}

	total_packet_size = len + 2 + 4; /* 2 for header, 4 for mask key, rest for payload */

	/* Early (minimal) length check */
	if (length < total_packet_size)
	{
		/* WebSocket frame too short */
		return 0;
	}

	/* Len=126 is special. It indicates the data length is actually "126 or more" */
	if (len == 126)
	{
		/* Extended payload length (16 bit). For packets of >=126 bytes */
		len = (readbuf[2] << 8) + readbuf[3];
		if (len < 126)
		{
			dead_socket(sptr, "WebSocket protocol violation (extended payload length too short)");
			return -1; /* This is a violation (not a short read), see page 29 */
		}
		p += 2; /* advance pointer 2 bytes */

		/* Need to check the length again, now it has changed: */
		if (length < len + 4 + 4)
		{
			/* WebSocket frame too short */
			return 0;
		}
		/* And update the packet size */
		total_packet_size = len + 4 + 4; /* 4 for header, 4 for mask key, rest for payload */
	}

	memcpy(maskkey, p, 4);
	p+= 4;
	payload = (len > 0) ? p : NULL;

	if (len > 0)
	{
		/* Unmask this thing (page 33, section 5.3) */
		int n;
		char v;
		for (n = 0; n < len; n++)
		{
			v = *p;
			*p++ = v ^ maskkey[n % 4];
		}
	}

	switch(opcode)
	{
		case WSOP_CONTINUATION:
		case WSOP_TEXT:
		case WSOP_BINARY:
			if (len > 0)
			{
				add_lf_if_needed(&payload, &len);
				if (!process_packet(sptr, payload, len, 1)) /* let UnrealIRCd process this data */
					return -1; /* fatal error occured (such as flood kill) */
			}
			return total_packet_size;

		case WSOP_CLOSE:
			dead_socket(sptr, "Connection closed"); /* TODO: Improve I guess */
			return -1;

		case WSOP_PING:
			if (websocket_handle_packet_ping(sptr, payload, len) < 0)
				return -1;
			return total_packet_size;

		case WSOP_PONG:
			if (websocket_handle_packet_pong(sptr, payload, len) < 0)
				return -1;
			return total_packet_size;

		default:
			dead_socket(sptr, "WebSocket: Unknown opcode");
			return -1;
	}

	return -1; /* NOTREACHED */
}

int websocket_handle_packet_ping(Client *sptr, char *buf, int len)
{
	if (len > 500)
	{
		dead_socket(sptr, "WebSocket: oversized PING request");
		return -1;
	}
	websocket_send_frame(sptr, WSOP_PONG, buf, len);
	sptr->local->since++; /* lag penalty of 1 second */
	return 0;
}

int websocket_handle_packet_pong(Client *sptr, char *buf, int len)
{
	/* We don't care */
	return 0;
}

/** Create a frame. Used for OUTGOING data. */
int websocket_create_frame(int opcode, char **buf, int *len)
{
	static char sendbuf[8192];

	sendbuf[0] = opcode | 0x80; /* opcode & final */

	if (*len > sizeof(sendbuf) - 8)
		abort(); /* should never happen (safety) */

	/* strip LF */
	if (*len > 0)
	{
		if (*(*buf + *len - 1) == '\n')
			*len = *len - 1;
	}
	/* strip CR */
	if (*len > 0)
	{
		if (*(*buf + *len - 1) == '\r')
			*len = *len - 1;
	}

	if (*len < 126)
	{
		/* Short payload */
		sendbuf[1] = (char)*len;
		memcpy(&sendbuf[2], *buf, *len);
		*buf = sendbuf;
		*len += 2;
	} else {
		/* Long payload */
		sendbuf[1] = 126;
		sendbuf[2] = (char)((*len >> 8) & 0xFF);
		sendbuf[3] = (char)(*len & 0xFF);
		memcpy(&sendbuf[4], *buf, *len);
		*buf = sendbuf;
		*len += 4;
	}
	return 0;
}

/** Create and send a frame */
int websocket_send_frame(Client *sptr, int opcode, char *buf, int len)
{
	char *b = buf;
	int l = len;

	if (websocket_create_frame(opcode, &b, &l) < 0)
		return -1;

	if (DBufLength(&sptr->local->sendQ) > get_sendq(sptr))
	{
		dead_socket(sptr, "Max SendQ exceeded");
		return -1;
	}

	dbuf_put(&sptr->local->sendQ, b, l);
	send_queued(sptr);
	return 0;
}
