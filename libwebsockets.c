#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>

#include <poll.h>
#include <sys/mman.h>

#include "libwebsockets.h"

void md5(const unsigned char *input, int ilen, unsigned char output[16]);
static void libwebsocket_service(struct libwebsocket *wsi, int sock);

#define LWS_MAX_HEADER_NAME_LENGTH 64
#define LWS_MAX_HEADER_LEN 4096
#define LWS_INITIAL_HDR_ALLOC 256
#define LWS_ADDITIONAL_HDR_ALLOC 64


/*
 * Chrome (v0)
 * 
 * GET / HTTP/1.1
 * Upgrade: WebSocket
 * Connection: Upgrade
 * Host: 127.0.0.1:7681
 * Origin: null
 * Sec-WebSocket-Key1: +46 3      1 75 7Y 60
 * Sec-WebSocket-Key2: m^+J358s0 6N 6e0 Q1 0  ~4~
 *
 * Firefox (v76)
 *
 * GET / HTTP/1.1
 * Upgrade: WebSocket
 * Host: 127.0.0.1:7681
 * Connection: Upgrade
 * Sec-WebSocket-Key1: EC."/$14 7 687YG+gZ 44d 16
 * Origin: file://
 * Sec-WebSocket-Key2: 2  /  9 0. 4 B8 77|ov968
 *
 */


enum lws_connection_states {
	WSI_STATE_HTTP,
	WSI_STATE_HTTP_HEADERS,
	WSI_STATE_DEAD_SOCKET,
	WSI_STATE_ESTABLISHED
};

enum lws_token_indexes {
	WSI_TOKEN_GET_URI,
	WSI_TOKEN_HOST,
	WSI_TOKEN_CONNECTION,
	WSI_TOKEN_KEY1,
	WSI_TOKEN_KEY2,
	WSI_TOKEN_PROTOCOL,
	WSI_TOKEN_UPGRADE,
	WSI_TOKEN_ORIGIN,
	WSI_TOKEN_CHALLENGE,
	
	/* always last real token index*/
	WSI_TOKEN_COUNT,
	/* parser state additions */
	WSI_TOKEN_NAME_PART,
	WSI_TOKEN_SKIPPING,
	WSI_TOKEN_SKIPPING_SAW_CR,
	WSI_PARSING_COMPLETE
};

enum lws_rx_parse_state {
	LWS_RXPS_NEW,
	
	LWS_RXPS_SEEN_76_FF,
	LWS_RXPS_PULLING_76_LENGTH,
	
	LWS_RXPS_PAYLOAD_UNTIL_LENGTH_EXHAUSTED
};


struct lws_tokens {
	char * token;
	int token_len;
};


/*
 * This is totally opaque to code using the library.  It's exported as a
 * forward-reference pointer-only declaration.
 */

struct libwebsocket {
	int (*callback)(struct libwebsocket *,
		enum libwebsocket_callback_reasons reason, void *, size_t);

	enum lws_connection_states state;

	char name_buffer[LWS_MAX_HEADER_NAME_LENGTH];
	int name_buffer_pos;
	int current_alloc_len;
	enum lws_token_indexes parser_state;
	struct lws_tokens utf8_token[WSI_TOKEN_COUNT];
	int ietf_spec_revision;
	
	int sock;

	enum lws_rx_parse_state lws_rx_parse_state;
	size_t rx_packet_length;
};


const struct lws_tokens lws_tokens[WSI_TOKEN_COUNT] = {
	{ "GET ", 4 },
	{ "Host:", 5 },
	{ "Connection:", 11 },
	{ "Sec-WebSocket-Key1:", 19 },
	{ "Sec-WebSocket-Key2:", 19 },
	{ "Sec-WebSocket-Protocol:", 23 },
	{ "Upgrade:", 8 },
	{ "Origin:", 7 },
	{ "\x0d\x0a", 2 },
};


int libwebsocket_create_server(int port,
		int (*callback)(struct libwebsocket *,
			enum libwebsocket_callback_reasons, void *, size_t),
								   int protocol)
{
	int n;
	int sockfd, newsockfd;
	unsigned int clilen;
	struct sockaddr_in serv_addr, cli_addr;
	int pid;
	struct libwebsocket *wsi = malloc(sizeof(struct libwebsocket));
     
	if (!wsi)
		return -1;
     
	wsi->state = WSI_STATE_HTTP;
	wsi->name_buffer_pos = 0;

	for (n = 0; n < WSI_TOKEN_COUNT; n++) {
		wsi->utf8_token[n].token = NULL;
		wsi->utf8_token[n].token_len = 0;
	}
	
	wsi->callback = callback;
	switch (protocol) {
	case 0:
	case 2:
	case 76:
		fprintf(stderr, " Using protocol v%d\n", protocol);
		wsi->ietf_spec_revision = protocol;
		break;
	default:
		fprintf(stderr, "protocol %d not supported (try 0 2 or 76)\n",
								      protocol);
		return -1;
	}
 
	/* sit there listening for connects, accept and spawn session servers */
 
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd < 0) {
		fprintf(stderr, "ERROR opening socket");
	}
	bzero((char *) &serv_addr, sizeof(serv_addr));

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);
	n = bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
	if (n < 0) {
              fprintf(stderr, "ERROR on binding to port %d (%d %d)\n", port, n,
									 errno);
              return -1;
        }
 
 	/* fork off a master server for this websocket server */
 
	n = fork();
	if (n < 0) {
		fprintf(stderr, "Failed on forking server thread: %d\n", n);
		exit(1);
	}
	
	/* we are done as far as the caller is concerned */
	
	if (n)
		return 0;
 
	fprintf(stderr, " Listening on port %d\n", port);
 
	listen(sockfd, 5);
    
	while (1) {
		clilen = sizeof(cli_addr);

		 newsockfd = accept(sockfd, (struct sockaddr *) &cli_addr, &clilen);
		 if (newsockfd < 0) {
			fprintf(stderr, "ERROR on accept");
			continue;
		}
		
//		fcntl(newsockfd, F_SETFL, O_NONBLOCK);
			
		/* fork off a new server instance */
			
		 pid = fork();
		 if (pid < 0) {
			fprintf(stderr, "ERROR on fork");
			continue;
		}
		
		if (pid) {
			close(newsockfd);
			continue;
		}

		/* we are the session process */

		close(sockfd);
		
		/* sit in libwebsocket_service() until session socket closed */
		
		libwebsocket_service(wsi, newsockfd);
		exit(0);
	}
}

void libwebsocket_close(struct libwebsocket *wsi)
{
	int n;

	wsi->state = WSI_STATE_DEAD_SOCKET;

	if (wsi->callback)
		wsi->callback(wsi, LWS_CALLBACK_CLOSED, NULL, 0);

	for (n = 0; n < WSI_TOKEN_COUNT; n++)
		if (wsi->utf8_token[n].token)
			free(wsi->utf8_token[n].token);
}

const char * libwebsocket_get_uri(struct libwebsocket *wsi)
{
	if (wsi->utf8_token[WSI_TOKEN_GET_URI].token)
		return wsi->utf8_token[WSI_TOKEN_GET_URI].token;
	
	return NULL;
}

static int libwebsocket_parse(struct libwebsocket *wsi, unsigned char c)
{
	int n;

	switch (wsi->parser_state) {
	case WSI_TOKEN_GET_URI:
	case WSI_TOKEN_HOST:
	case WSI_TOKEN_CONNECTION:
	case WSI_TOKEN_KEY1:
	case WSI_TOKEN_KEY2:
	case WSI_TOKEN_PROTOCOL:
	case WSI_TOKEN_UPGRADE:
	case WSI_TOKEN_ORIGIN:
	case WSI_TOKEN_CHALLENGE:
	
//		fprintf(stderr, "WSI_TOKEN_(body %d) '%c'\n", wsi->parser_state, c);

		/* collect into malloc'd buffers */
		/* optional space swallow */
		if (!wsi->utf8_token[wsi->parser_state].token_len && c == ' ')
			break;
			
		/* special case space terminator for get-uri */
		if (wsi->parser_state == WSI_TOKEN_GET_URI && c == ' ') {
			wsi->utf8_token[wsi->parser_state].token[
			   wsi->utf8_token[wsi->parser_state].token_len] = '\0';
			wsi->parser_state = WSI_TOKEN_SKIPPING;
			break;
		}

		/* allocate appropriate memory */
		if (wsi->utf8_token[wsi->parser_state].token_len ==
						   wsi->current_alloc_len - 1) {
			/* need to extend */
			wsi->current_alloc_len += LWS_ADDITIONAL_HDR_ALLOC;
			if (wsi->current_alloc_len >= LWS_MAX_HEADER_LEN) {
				/* it's waaay to much payload, fail it */
				strcpy(wsi->utf8_token[wsi->parser_state].token,
				   "!!! Length exceeded maximum supported !!!");
				wsi->parser_state = WSI_TOKEN_SKIPPING;
				break;
			}
			wsi->utf8_token[wsi->parser_state].token =
			       realloc(wsi->utf8_token[wsi->parser_state].token,
							wsi->current_alloc_len);
		}

		/* bail at EOL */
		if (wsi->parser_state != WSI_TOKEN_CHALLENGE && c == '\x0d') {
			wsi->utf8_token[wsi->parser_state].token[
			   wsi->utf8_token[wsi->parser_state].token_len] = '\0';
			wsi->parser_state = WSI_TOKEN_SKIPPING_SAW_CR;
			break;
		}

		wsi->utf8_token[wsi->parser_state].token[
			    wsi->utf8_token[wsi->parser_state].token_len++] = c;

		/* special payload limiting */
		if (wsi->parser_state == WSI_TOKEN_CHALLENGE &&
			    wsi->utf8_token[wsi->parser_state].token_len == 8) {
//			fprintf(stderr, "Setting WSI_PARSING_COMPLETE\n");
			wsi->parser_state = WSI_PARSING_COMPLETE;
			break;
		}
		
		break;

		/* collecting and checking a name part */
	case WSI_TOKEN_NAME_PART:
//		fprintf(stderr, "WSI_TOKEN_NAME_PART '%c'\n", c);

		if (wsi->name_buffer_pos == sizeof(wsi->name_buffer) - 1) {
			/* name bigger than we can handle, skip until next */
			wsi->parser_state = WSI_TOKEN_SKIPPING;
			break;
		}
		wsi->name_buffer[wsi->name_buffer_pos++] = c;
		wsi->name_buffer[wsi->name_buffer_pos] = '\0';
		
		for (n = 0; n < WSI_TOKEN_COUNT; n++) {
			if (wsi->name_buffer_pos != lws_tokens[n].token_len)
				continue;
			if (strcmp(lws_tokens[n].token, wsi->name_buffer))
				continue;
			fprintf(stderr, "known header '%s'\n", wsi->name_buffer);
			wsi->parser_state = WSI_TOKEN_GET_URI + n;
			wsi->current_alloc_len = LWS_INITIAL_HDR_ALLOC;
			wsi->utf8_token[wsi->parser_state].token =
						 malloc(wsi->current_alloc_len);
			wsi->utf8_token[wsi->parser_state].token_len = 0;
			n = WSI_TOKEN_COUNT;
		}

		/* colon delimiter means we just don't know this name */

		if (wsi->parser_state == WSI_TOKEN_NAME_PART && c == ':') {
			fprintf(stderr, "skipping unknown header '%s'\n", wsi->name_buffer);
			wsi->parser_state = WSI_TOKEN_SKIPPING;
			break;
		}
		
		/* don't look for payload when it can just be http headers */
		
		if (wsi->parser_state == WSI_TOKEN_CHALLENGE &&
				!wsi->utf8_token[WSI_TOKEN_UPGRADE].token_len) {
			/* they're HTTP headers, not websocket upgrade! */
			fprintf(stderr, "Setting WSI_PARSING_COMPLETE from http headers\n");
			wsi->parser_state = WSI_PARSING_COMPLETE;
		}
		break;
			
		/* skipping arg part of a name we didn't recognize */
	case WSI_TOKEN_SKIPPING:
//		fprintf(stderr, "WSI_TOKEN_SKIPPING '%c'\n", c);
		if (c == '\x0d')
			wsi->parser_state = WSI_TOKEN_SKIPPING_SAW_CR;
		break;
	case WSI_TOKEN_SKIPPING_SAW_CR:
//		fprintf(stderr, "WSI_TOKEN_SKIPPING_SAW_CR '%c'\n", c);
		if (c == '\x0a')
			wsi->parser_state = WSI_TOKEN_NAME_PART;
		else
			wsi->parser_state = WSI_TOKEN_SKIPPING;
		wsi->name_buffer_pos = 0;
		break;
		/* we're done, ignore anything else */
	case WSI_PARSING_COMPLETE:
//		fprintf(stderr, "WSI_PARSING_COMPLETE '%c'\n", c);
		break;
		
	default:	/* keep gcc happy */
		break;
	}
	
	return 0;
}

static int interpret_key(const char *key, unsigned int *result)
{
	char digits[20];
	int digit_pos = 0;
	const char *p = key;
	int spaces = 0;
	
	while (*p) {
		if (isdigit(*p)) {
			if (digit_pos == sizeof(digits) - 1)
				return -1;
			digits[digit_pos++] = *p;
		}
		p++;
	}
	digits[digit_pos] = '\0';
	if (!digit_pos)
		return -2;
		
	while (*key) {
		if (*key == ' ')
			spaces++;
		key++;
	}
	
	if (!spaces)
		return -3;
		
	*result = atol(digits) / spaces;
	
	return 0;
}

static int libwebsocket_rx_sm(struct libwebsocket *wsi, unsigned char c)
{
	int n;
	unsigned char buf[2];

	switch (wsi->lws_rx_parse_state) {
	case LWS_RXPS_NEW:
	
		switch (wsi->ietf_spec_revision) {
		/* Firefox 4.0b6 likes this as of 30 Oct */
		case 76:
			if (c == 0xff)
				wsi->lws_rx_parse_state = LWS_RXPS_SEEN_76_FF;
			break;
		case 0:
			break;
		}
		break;
	case LWS_RXPS_SEEN_76_FF:
		if (c != 0) {
			break;
		}

		fprintf(stderr, "Seen that client is requesting a v76 close, sending ack\n");
		buf[0] = 0xff;
		buf[1] = 0;
		n = write(wsi->sock, buf, 2);
		if (n < 0) {
			fprintf(stderr, "ERROR writing to socket");
			return -1;
		}
		fprintf(stderr, "  v76 close ack sent, server closing socket\n");
		/* returning < 0 will get it closed in parent */
		return -1;

	case LWS_RXPS_PULLING_76_LENGTH:
		break;
	case LWS_RXPS_PAYLOAD_UNTIL_LENGTH_EXHAUSTED:
		break;
	}

	return 0;
}

static int libwebsocket_interpret_incoming_packet(struct libwebsocket *wsi,
						 unsigned char *buf, size_t len)
{
	int n;

	fprintf(stderr, "received %d byte packet\n", (int)len);
	for (n = 0; n < len; n++)
		fprintf(stderr, "%02X ", buf[n]);
	fprintf(stderr, "\n");

	/* let the rx protocol state machine have as much as it needs */
	
	n = 0;
	while (wsi->lws_rx_parse_state !=
			     LWS_RXPS_PAYLOAD_UNTIL_LENGTH_EXHAUSTED && n < len)
		if (libwebsocket_rx_sm(wsi, buf[n++]) < 0)
			return -1;
		
	if (n != len) {
		if (wsi->callback)
			wsi->callback(wsi, LWS_CALLBACK_RECEIVE, &buf[n], len - n);
	}
	
	return -0;
}


/*
 * We have to take care about parsing because the headers may be split
 * into multiple fragments.  They may contain unknown headers with arbitrary
 * argument lengths.  So, we parse using a single-character at a time state
 * machine that is completely independent of packet size.
 */

int libwebsocket_read(struct libwebsocket *wsi, unsigned char * buf, size_t len)
{
	size_t n;
	char *p;
	unsigned int key1, key2;
	unsigned char sum[16];
	char *response;
	
	switch (wsi->state) {
	case WSI_STATE_HTTP:
		wsi->state = WSI_STATE_HTTP_HEADERS;
		wsi->parser_state = WSI_TOKEN_NAME_PART;
		/* fallthru */
	case WSI_STATE_HTTP_HEADERS:
	
		fprintf(stderr, "issuing %d bytes to parser\n", (int)len);
	
		fwrite(buf, 1, len, stderr);
		for (n = 0; n< len; n++)
			libwebsocket_parse(wsi, *buf++);
			
		if (wsi->parser_state != WSI_PARSING_COMPLETE)
			break;

		/* is this websocket protocol or normal http 1.0? */
		
		if (!wsi->utf8_token[WSI_TOKEN_UPGRADE].token_len ||
			     !wsi->utf8_token[WSI_TOKEN_CONNECTION].token_len) {
			if (wsi->callback)
				(wsi->callback)(wsi, LWS_CALLBACK_HTTP, NULL, 0);
			wsi->state = WSI_STATE_HTTP;
			return 0;
		}

			
		fprintf(stderr, "Preparing return packet\n");

		/* Websocket - confirm we have all the necessary pieces */
		
		if (!wsi->utf8_token[WSI_TOKEN_ORIGIN].token_len ||
			!wsi->utf8_token[WSI_TOKEN_HOST].token_len ||
			!wsi->utf8_token[WSI_TOKEN_CHALLENGE].token_len ||
			!wsi->utf8_token[WSI_TOKEN_KEY1].token_len ||
				     !wsi->utf8_token[WSI_TOKEN_KEY2].token_len)
			/* completed header processing, but missing some bits */
			goto bail;
		
		/* create the response packet */
		
		/* make a buffer big enough for everything */
		
		response = malloc(256 +
			wsi->utf8_token[WSI_TOKEN_UPGRADE].token_len +
			wsi->utf8_token[WSI_TOKEN_CONNECTION].token_len +
			wsi->utf8_token[WSI_TOKEN_HOST].token_len +
			wsi->utf8_token[WSI_TOKEN_ORIGIN].token_len +
			wsi->utf8_token[WSI_TOKEN_GET_URI].token_len +
			wsi->utf8_token[WSI_TOKEN_PROTOCOL].token_len);
		if (!response) {
			fprintf(stderr, "Out of memory for response buffer\n");
			goto bail;
		}
		
		p = response;
		strcpy(p,   "HTTP/1.1 101 WebSocket Protocol Handshake\x0d\x0a"
						  "Upgrade: WebSocket\x0d\x0a");
		p += strlen("HTTP/1.1 101 WebSocket Protocol Handshake\x0d\x0a"
						  "Upgrade: WebSocket\x0d\x0a");
		strcpy(p,   "Connection: Upgrade\x0d\x0aSec-WebSocket-Origin: ");
		p += strlen("Connection: Upgrade\x0d\x0aSec-WebSocket-Origin: ");
		strcpy(p, wsi->utf8_token[WSI_TOKEN_ORIGIN].token);
		p += wsi->utf8_token[WSI_TOKEN_ORIGIN].token_len;
		strcpy(p,   "\x0d\x0aSec-WebSocket-Location: ws://");
		p += strlen("\x0d\x0aSec-WebSocket-Location: ws://");
		strcpy(p, wsi->utf8_token[WSI_TOKEN_HOST].token);
		p += wsi->utf8_token[WSI_TOKEN_HOST].token_len;
		strcpy(p, wsi->utf8_token[WSI_TOKEN_GET_URI].token);
		p += wsi->utf8_token[WSI_TOKEN_GET_URI].token_len;

		if (wsi->utf8_token[WSI_TOKEN_PROTOCOL].token) {
			strcpy(p,   "\x0d\x0aSec-WebSocket-Protocol: ");
			p += strlen("\x0d\x0aSec-WebSocket-Protocol: ");
			strcpy(p, wsi->utf8_token[WSI_TOKEN_PROTOCOL].token);
			p += wsi->utf8_token[WSI_TOKEN_PROTOCOL].token_len;
		}

		strcpy(p,   "\x0d\x0a\x0d\x0a");
		p += strlen("\x0d\x0a\x0d\x0a");
		
		/* convert the two keys into 32-bit integers */
		
		if (interpret_key(wsi->utf8_token[WSI_TOKEN_KEY1].token, &key1))
			goto bail;
		if (interpret_key(wsi->utf8_token[WSI_TOKEN_KEY2].token, &key2))
			goto bail;
			
		/* lay them out in network byte order (MSB first */

		sum[0] = key1 >> 24;
		sum[1] = key1 >> 16;
		sum[2] = key1 >> 8;
		sum[3] = key1;
		sum[4] = key2 >> 24;
		sum[5] = key2 >> 16;
		sum[6] = key2 >> 8;
		sum[7] = key2;
		
		/* follow them with the challenge token we were sent */
		
		memcpy(&sum[8], wsi->utf8_token[WSI_TOKEN_CHALLENGE].token, 8);

		/* 
		 * compute the md5sum of that 16-byte series and use as our
		 * payload after our headers
		 */

		md5(sum, 16, (unsigned char *)p);
		p += 16;

		/* it's complete: go ahead and send it */
		
		fprintf(stderr, "issuing response packet %d len\n",
							   (int)(p - response));
		fwrite(response, 1,  p - response, stderr);
			
		n = write(wsi->sock, response, p - response);
		if (n < 0) {
			fprintf(stderr, "ERROR writing to socket");
			goto bail;
		}
		
		/* alright clean up and set ourselves into established state */

		free(response);
		wsi->state = WSI_STATE_ESTABLISHED;
		wsi->lws_rx_parse_state = LWS_RXPS_NEW;
		
		/* notify user code that we're ready to roll */
				
		if (wsi->callback)
			wsi->callback(wsi, LWS_CALLBACK_ESTABLISHED, NULL, 0);
		break;

	case WSI_STATE_ESTABLISHED:
		if (libwebsocket_interpret_incoming_packet(wsi, buf, len) < 0)
			goto bail;
		break;
	default:
		break;
	}
	
	return 0;
	
bail:
	libwebsocket_close(wsi);
	return -1;
}


/*
 * notice, we will use up to LWS_SEND_BUFFER_PRE_PADDING bytes BEFORE the
 * buffer pointer given and LWS_SEND_BUFFER_POST_PADDING bytes AFTER
 * buf + len !!!  Caller must allocate and offset pointer accordingly!
 * 
 * This lets us send packets in one write() action including the protocol
 * pre- and post- data without copying the payload around.
 */

int libwebsocket_write(struct libwebsocket * wsi, unsigned char *buf,
			  size_t len, enum libwebsocket_write_protocol protocol)
{
	int n;
	int pre = 0;
	int post = 0;
	unsigned int shift = 7;
	
	if (protocol == LWS_WRITE_HTTP)
		goto send_raw;
	
	/* websocket protocol, either binary or text */
	
	if (wsi->state != WSI_STATE_ESTABLISHED)
		return -1;

	switch (wsi->ietf_spec_revision) {
	/* Firefox 4.0b6 likes this as of 30 Oct */
	case 76:
		if (protocol == LWS_WRITE_BINARY) {
			/* in binary mode we send 7-bit used length blocks */
			pre = 1;
			while (len & (127 << shift)) {
				pre++;
				shift += 7;
			}
			n = 0;
			shift -= 7;
			while (shift >= 0) {
				if (shift)
					buf[0 - pre + n] =
						  ((len >> shift) & 127) | 0x80;
				else
					buf[0 - pre + n] =
						  ((len >> shift) & 127);
				n++;
				shift -= 7;
			}
			break;
		}

		/* frame type = text, length-free spam mode */

		buf[-1] = 0;
		buf[len] = 0xff; /* EOT marker */
		pre = 1;
		post = 1;
		break;

	/* chrome likes this as of 30 Oct */
	case 0:
		buf[-9] = 0xff;
#if defined __LP64__
			buf[-8] = len >> 56;
			buf[-7] = len >> 48;
			buf[-6] = len >> 40;
			buf[-5] = len >> 32;
#else
			buf[-8] = 0;
			buf[-7] = 0;
			buf[-6] = 0;
			buf[-5] = 0;
#endif
		buf[-4] = len >> 24;
		buf[-3] = len >> 16;
		buf[-2] = len >> 8;
		buf[-1] = len;
		pre = 9;
		break;
		
	/* just an unimplemented spec right now apparently */
	case 2:
		n = 4; /* text */
		if (protocol == LWS_WRITE_BINARY)
			n = 5; /* binary */
		if (len < 126) {
			buf[-2] = n;
			buf[-1] = len;
			pre = 2;
		} else {
			if (len < 65536) {
				buf[-4] = n;
				buf[-3] = 126;
				buf[-2] = len >> 8;
				buf[-1] = len;
				pre = 4;
			} else {
				buf[-10] = n;
				buf[-9] = 127;
#if defined __LP64__
					buf[-8] = (len >> 56) & 0x7f;
					buf[-7] = len >> 48;
					buf[-6] = len >> 40;
					buf[-5] = len >> 32;
#else
					buf[-8] = 0;
					buf[-7] = 0;
					buf[-6] = 0;
					buf[-5] = 0;
#endif
				buf[-4] = len >> 24;
				buf[-3] = len >> 16;
				buf[-2] = len >> 8;
				buf[-1] = len;
				pre = 10;
			}
		}
		break;
	}
	
//	for (n = 0; n < (len + pre + post); n++)
//		fprintf(stderr, "%02X ", buf[n - pre]);
//		
//	fprintf(stderr, "\n");

send_raw:

	n = send(wsi->sock, buf - pre, len + pre + post, 0);
	if (n < 0) {
		fprintf(stderr, "ERROR writing to socket");
		return -1;
	}

//	fprintf(stderr, "written %d bytes to client\n", (int)len);
	
	return 0;
}

static void libwebsocket_service(struct libwebsocket *wsi, int sock)
{
	int n;
	unsigned char buf[256];
	struct pollfd fds;
	
	wsi->sock = sock;
	    
	while (1) {
		fds.fd = sock;
		fds.events = POLLIN;
		fds.revents = 0;
 		n = poll(&fds, 1, 50);

		/* 
		 * we seem to get a ton of POLLINs coming when there's nothing
		 * to read (at least, 0 bytes read)... don't understand why yet
		 * but I stuck a usleep() in the 0 byte read case to regulate
		 * CPU
		 */


		if (n < 0) {
			fprintf(stderr, "Socket dead (poll = %d)\n", n);
			return;
		}
		if (n == 0)
			goto pout;

		if (fds.revents & (POLLERR | POLLHUP)) {
			fprintf(stderr, "Socket dead\n");
			return;
		}
		
		if (wsi->state == WSI_STATE_DEAD_SOCKET) {
			fprintf(stderr, "Seen socket dead, returning from service\n");
			return;
		}
		
		if (fds.revents & POLLIN) {
			
//			fprintf(stderr, "POLLIN\n");
			
			n = recv(sock, buf, sizeof(buf), 0);
			if (n < 0) {
				fprintf(stderr, "Socket read returned %d\n", n);
				continue;
			}
			if (n)
				libwebsocket_read(wsi, buf, n);
			else {
//				fprintf(stderr, "POLLIN with zero length waiting\n");
				usleep(50000);
			}
		}
pout:
		if (wsi->state != WSI_STATE_ESTABLISHED)
			continue;

//		fprintf(stderr, "POLLOUT\n");
					
		if (wsi->callback)
			wsi->callback(wsi, LWS_CALLBACK_SEND, NULL, 0);
	}
}

int libwebsockets_serve_http_file(struct libwebsocket *wsi, const char * file,
						      const char * content_type)
{
	int fd;
	struct stat stat;
	char buf[512];
	char *p = buf;
	int n;

	fd = open(file, O_RDONLY);
	if (fd < 1) {
		p += sprintf(p, "HTTP/1.0 400 Bad\x0d\x0a"
			"Server: libwebsockets\x0d\x0a"
			"\x0d\x0a"
		);
		libwebsocket_write(wsi, (unsigned char *)buf, p - buf, LWS_WRITE_HTTP);
		
		return -1;
	}

	fstat(fd, &stat);
	p += sprintf(p, "HTTP/1.0 200 OK\x0d\x0a"
			"Server: libwebsockets\x0d\x0a"
			"Content-Type: %s\x0d\x0a"
			"Content-Length: %u\x0d\x0a"
			"\x0d\x0a", content_type, (unsigned int)stat.st_size
			);
			
	libwebsocket_write(wsi, (unsigned char *)buf, p - buf, LWS_WRITE_HTTP);

	n = 1;
	while (n > 0) {
		n = read(fd, buf, 512);
		libwebsocket_write(wsi, (unsigned char *)buf, n, LWS_WRITE_HTTP);
	}
	
	close(fd);
		
	return 0;
}