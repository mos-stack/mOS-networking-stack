#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

#include <mos_api.h>
#include "cpu.h"
#include "http_parsing.h"
#include "debug.h"
#include "applib.h"

#define CONFIG_FILE       "config/epserver.conf"
static struct conf_var g_conf[] = {
	{ "core_limit", {0} },
	{ "www_main",   {0} },
};
#define NUM_CONF_VAR (sizeof(g_conf) / sizeof(struct conf_var))

#define HTTP_HEADER_LEN 1024
#define URL_LEN 128

/* shinae 10.27.2014 
 * SNDBUF_SIZE should be removed 
 */
#define SNDBUF_SIZE (8*1024)

#define MAX_FILES 100
int cores[MAX_CPUS] = {0};
/*----------------------------------------------------------------------------*/
struct mtcp_conf g_mcfg;
/*----------------------------------------------------------------------------*/
struct file_cache
{
	char name[128];
	char fullname[256];
	uint64_t size;
	char *file;
};
/*----------------------------------------------------------------------------*/
struct server_vars
{
	char request[HTTP_HEADER_LEN];
	int recv_len;
	int request_len;
	long int total_read, total_sent;
	uint8_t done;
	uint8_t rspheader_sent;
	uint8_t keep_alive;

	int fidx;						// file cache index
	char fname[128];				// file name
	long int fsize;					// file size
};
/*----------------------------------------------------------------------------*/
struct thread_context
{
	mctx_t mctx;
	int listener;
	int ep;
	struct server_vars *svars;
};
/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
/*----------------------------------------------------------------------------*/
char *www_main;
static struct file_cache fcache[MAX_FILES];
static int nfiles;
/*----------------------------------------------------------------------------*/
static int finished;
/*----------------------------------------------------------------------------*/
static char *
StatusCodeToString(int scode)
{
	switch (scode) {
		case 200:
			return "OK";
			break;

		case 404:
			return "Not Found";
			break;
	}

	return NULL;
}
/*----------------------------------------------------------------------------*/
static void
CleanServerVariable(struct server_vars *sv)
{
	sv->recv_len = 0;
	sv->request_len = 0;
	sv->total_read = 0;
	sv->total_sent = 0;
	sv->done = 0;
	sv->rspheader_sent = 0;
	sv->keep_alive = 0;
}
/*----------------------------------------------------------------------------*/
static void 
CloseConnection(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MOS_EPOLL_CTL_DEL, sockid, NULL);
	mtcp_close(ctx->mctx, sockid);
}
/*----------------------------------------------------------------------------*/
static int 
SendUntilAvailable(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	int ret;
	int sent;
	int len;

	if (sv->done || !sv->rspheader_sent) {
		return 0;
	}

	sent = 0;
	ret = 1;
	while (ret > 0) {
		len = MIN(SNDBUF_SIZE, sv->fsize - sv->total_sent);
		if (len <= 0) {
			break;
		}
		ret = mtcp_write(ctx->mctx, sockid,  
				fcache[sv->fidx].file + sv->total_sent, len);
		if (ret < 0) {
			if (errno != EAGAIN) {
				TRACE_ERROR("Socket %d: Sending HTTP response body failed. "
						"try: %d, sent: %d\n", sockid, len, ret);
			}
			break;
		}
		TRACE_APP("Socket %d: mtcp_write try: %d, ret: %d\n", sockid, len, ret);
		sent += ret;
		sv->total_sent += ret;
	}

	if (sv->total_sent >= fcache[sv->fidx].size) {
		struct mtcp_epoll_event ev;
		sv->done = TRUE;
		finished++;

		if (sv->keep_alive) {
			/* if keep-alive connection, wait for the incoming request */
			ev.events = MOS_EPOLLIN;
			ev.data.sock = sockid;
			mtcp_epoll_ctl(ctx->mctx, ctx->ep, MOS_EPOLL_CTL_MOD, sockid, &ev);

			CleanServerVariable(sv);
		} else {
			/* else, close connection */
			CloseConnection(ctx, sockid, sv);
		}
	}

	return sent;
}
/*----------------------------------------------------------------------------*/
static int 
HandleReadEvent(struct thread_context *ctx, int sockid, struct server_vars *sv)
{
	struct mtcp_epoll_event ev;
	char buf[HTTP_HEADER_LEN];
	char url[URL_LEN];
	char response[HTTP_HEADER_LEN];
	int scode;						// status code
	time_t t_now;
	char t_str[128];
	char keepalive_str[128];
	int rd;
	int i;
	int len;
	int sent;

	/* HTTP request handling */
	rd = mtcp_read(ctx->mctx, sockid, buf, HTTP_HEADER_LEN);
	if (rd <= 0) {
		return rd;
	}
	memcpy(sv->request + sv->recv_len, 
			(char *)buf, MIN(rd, HTTP_HEADER_LEN - sv->recv_len));
	sv->recv_len += rd;
	//sv->request[rd] = '\0';
	//fprintf(stderr, "HTTP Request: \n%s", request);
	sv->request_len = find_http_header(sv->request, sv->recv_len);
	if (sv->request_len <= 0) {
		TRACE_ERROR("Socket %d: Failed to parse HTTP request header.\n"
				"read bytes: %d, recv_len: %d, "
				"request_len: %d, strlen: %ld, request: \n%s\n", 
				sockid, rd, sv->recv_len, 
				sv->request_len, strlen(sv->request), sv->request);
		return rd;
	}

	http_get_url(sv->request, sv->request_len, url, URL_LEN);
	TRACE_APP("Socket %d URL: %s\n", sockid, url);
	sprintf(sv->fname, "%s%s", www_main, url);
	TRACE_APP("Socket %d File name: %s\n", sockid, sv->fname);


	sv->keep_alive = FALSE;
	if (http_header_str_val(sv->request, CONN_HDR_FLD, 
				sizeof(CONN_HDR_FLD)-1, keepalive_str, sizeof(keepalive_str))) {
		sv->keep_alive = !strcasecmp(keepalive_str, KEEP_ALIVE_STR);
	}

	/* Find file in cache */
	scode = 404;
	for (i = 0; i < nfiles; i++) {
		if (strcmp(sv->fname, fcache[i].fullname) == 0) {
			sv->fsize = fcache[i].size;
			sv->fidx = i;
			scode = 200;
			break;
		}
	}
	TRACE_APP("Socket %d File size: %ld (%ldMB)\n", 
			sockid, sv->fsize, sv->fsize / 1024 / 1024);

	/* Response header handling */
	time(&t_now);
	strftime(t_str, 128, "%a, %d %b %Y %X GMT", gmtime(&t_now));
	if (sv->keep_alive)
		sprintf(keepalive_str, "Keep-Alive");
	else
		sprintf(keepalive_str, "Close");

	sprintf(response, "HTTP/1.1 %d %s\r\n"
			"Date: %s\r\n"
			"Server: Webserver on Middlebox TCP (Ubuntu)\r\n"
			"Content-Length: %ld\r\n"
			"Connection: %s\r\n\r\n", 
			scode, StatusCodeToString(scode), t_str, sv->fsize, keepalive_str);
	len = strlen(response);
	TRACE_APP("Socket %d HTTP Response: \n%s", sockid, response);
	sent = mtcp_write(ctx->mctx, sockid, response, len);
	if (sent < len) {
		TRACE_ERROR("Socket %d: Sending HTTP response failed. "
				"try: %d, sent: %d\n", sockid, len, sent);
		CloseConnection(ctx, sockid, sv);
	}
	TRACE_APP("Socket %d Sent response header: try: %d, sent: %d\n", 
			sockid, len, sent);
	assert(sent == len);
	sv->rspheader_sent = TRUE;

	ev.events = MOS_EPOLLIN | MOS_EPOLLOUT;
	ev.data.sock = sockid;
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MOS_EPOLL_CTL_MOD, sockid, &ev);

	SendUntilAvailable(ctx, sockid, sv);

	return rd;
}
/*----------------------------------------------------------------------------*/
static int 
AcceptConnection(struct thread_context *ctx, int listener)
{
	mctx_t mctx = ctx->mctx;
	struct server_vars *sv;
	struct mtcp_epoll_event ev;
	int c;

	c = mtcp_accept(mctx, listener, NULL, NULL);

	if (c >= 0) {
		if (c >= MAX_FLOW_NUM) {
			TRACE_ERROR("Invalid socket id %d.\n", c);
			return -1;
		}

		sv = &ctx->svars[c];
		CleanServerVariable(sv);
		TRACE_APP("New connection %d accepted.\n", c);
		ev.events = MOS_EPOLLIN;
		ev.data.sock = c;
		mtcp_setsock_nonblock(ctx->mctx, c);
		mtcp_epoll_ctl(mctx, ctx->ep, MOS_EPOLL_CTL_ADD, c, &ev);
		TRACE_APP("Socket %d registered.\n", c);

	} else {
		if (errno != EAGAIN) {
			TRACE_ERROR("mtcp_accept() error %s\n", 
					strerror(errno));
		}
	}

	return c;
}
/*----------------------------------------------------------------------------*/
static int 
CreateListeningSocket(struct thread_context *ctx)
{
	int listener;
	struct mtcp_epoll_event ev;
	struct sockaddr_in saddr;
	int ret;

	/* create socket and set it as nonblocking */
	listener = mtcp_socket(ctx->mctx, AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		TRACE_ERROR("Failed to create listening socket!\n");
		return -1;
	}
	ret = mtcp_setsock_nonblock(ctx->mctx, listener);
	if (ret < 0) {
		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
		return -1;
	}

	/* bind to port 80 */
	saddr.sin_family = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port = htons(80);
	ret = mtcp_bind(ctx->mctx, listener, 
			(struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		TRACE_ERROR("Failed to bind to the listening socket!\n");
		return -1;
	}

	/* listen (backlog: 4K) */
	ret = mtcp_listen(ctx->mctx, listener, 4096);
	if (ret < 0) {
		TRACE_ERROR("mtcp_listen() failed!\n");
		return -1;
	}
	
	/* wait for incoming accept events */
	ev.events = MOS_EPOLLIN;
	ev.data.sock = listener;
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MOS_EPOLL_CTL_ADD, listener, &ev);

	return listener;
}
/*----------------------------------------------------------------------------*/
static void
GlobInitServer()
{
	DIR *dir;
	struct dirent *ent;
	int fd;
	int ret;
	uint64_t total_read;

	num_cores = GetNumCPUs();
	core_limit = num_cores;
	
	if (LoadConfig(CONFIG_FILE, g_conf, NUM_CONF_VAR))
		exit(-1);

	core_limit = atoi(g_conf[0].value);
	www_main = g_conf[1].value;

	/* open the directory to serve */
	dir = opendir(www_main);
	if (!dir) {
		TRACE_ERROR("Failed to open %s.\n", www_main);
		perror("opendir");
		exit(-1);
	}

	nfiles = 0;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0)
			continue;
		else if (strcmp(ent->d_name, "..") == 0)
			continue;

		strcpy(fcache[nfiles].name, ent->d_name);
		sprintf(fcache[nfiles].fullname, "%s/%s", www_main, ent->d_name);
		fd = open(fcache[nfiles].fullname, O_RDONLY);
		if (fd < 0) {
			perror("open");
			continue;
		} else {
			fcache[nfiles].size = lseek64(fd, 0, SEEK_END);
			lseek64(fd, 0, SEEK_SET);
		}

		fcache[nfiles].file = (char *)malloc(fcache[nfiles].size);
		if (!fcache[nfiles].file) {
			TRACE_ERROR("Failed to allocate memory for file %s\n", 
					fcache[nfiles].name);
			perror("malloc");
			continue;
		}

		TRACE_INFO("Reading %s (%lu bytes)\n", 
				fcache[nfiles].name, fcache[nfiles].size);
		total_read = 0;
		while (1) {
			ret = read(fd, fcache[nfiles].file + total_read, 
					fcache[nfiles].size - total_read);
			if (ret < 0) {
				break;
			} else if (ret == 0) {
				break;
			}
			total_read += ret;
		}
		if (total_read < fcache[nfiles].size) {
			free(fcache[nfiles].file);
			continue;
		}
		close(fd);
		nfiles++;

		if (nfiles >= MAX_FILES)
			break;
	}

	finished = 0;

	return;
}
/*----------------------------------------------------------------------------*/
static void
InitServer(mctx_t mctx, void **app_ctx)
{
	struct thread_context *ctx;

	ctx = (struct thread_context *)calloc(1, sizeof(struct thread_context));
	if (!ctx) {
		TRACE_ERROR("Failed to create thread context!\n");
		exit(-1);
	}

	ctx->mctx = mctx;

	/* create epoll descriptor */
	ctx->ep = mtcp_epoll_create(mctx, MAX_EVENTS);
	if (ctx->ep < 0) {
		TRACE_ERROR("Failed to create epoll descriptor!\n");
		exit(-1);
	}

	/* allocate memory for server variables */
	ctx->svars = (struct server_vars *)
			calloc(MAX_FLOW_NUM, sizeof(struct server_vars));
	if (!ctx->svars) {
		TRACE_ERROR("Failed to create server_vars struct!\n");
		exit(-1);
	}

	ctx->listener = CreateListeningSocket(ctx);
	if (ctx->listener < 0) {
		TRACE_ERROR("Failed to create listening socket.\n");
		exit(-1);
	}

	*app_ctx = (void *)ctx;

	return;
}
/*----------------------------------------------------------------------------*/
static void
RunServer(mctx_t mctx, void **app_ctx)
{
	struct thread_context *ctx = (*app_ctx);
	int nevents;
	int i, ret;
	int do_accept;
	struct mtcp_epoll_event *events;

	assert(ctx);
	int ep = ctx->ep;

	events = (struct mtcp_epoll_event *)
			calloc(MAX_EVENTS, sizeof(struct mtcp_epoll_event));
	if (!events) {
		TRACE_ERROR("Failed to create event struct!\n");
		exit(-1);
	}

	while (1) {
		nevents = mtcp_epoll_wait(mctx, ep, events, MAX_EVENTS, -1);
		if (nevents < 0) {
			if (errno != EINTR)
				perror("mtcp_epoll_wait");
			break;
		}

		do_accept = FALSE;
		for (i = 0; i < nevents; i++) {

			if (events[i].data.sock == ctx->listener) {
				/* if the event is for the listener, accept connection */
				do_accept = TRUE;

			} else if (events[i].events & MOS_EPOLLERR) {
				int err;
				socklen_t len = sizeof(err);

				/* error on the connection */
				TRACE_APP("[CPU %d] Error on socket %d\n", 
						core, events[i].data.sock);
				if (mtcp_getsockopt(mctx, events[i].data.sock, 
						SOL_SOCKET, SO_ERROR, (void *)&err, &len) == 0) {
					if (err != ETIMEDOUT) {
						fprintf(stderr, "Error on socket %d: %s\n", 
								events[i].data.sock, strerror(err));
					}
				} else {
					fprintf(stderr, "mtcp_getsockopt: %s (for sockid: %d)\n",
						strerror(errno), events[i].data.sock);
					exit(-1);
				}
				CloseConnection(ctx, events[i].data.sock, 
						&ctx->svars[events[i].data.sock]);

			} else if (events[i].events & MOS_EPOLLIN) {
				ret = HandleReadEvent(ctx, events[i].data.sock, 
						&ctx->svars[events[i].data.sock]);

				if (ret == 0) {
					/* connection closed by remote host */
					CloseConnection(ctx, events[i].data.sock, 
							&ctx->svars[events[i].data.sock]);
				} else if (ret < 0) {
					/* if not EAGAIN, it's an error */
					if (errno != EAGAIN) {
						CloseConnection(ctx, events[i].data.sock, 
								&ctx->svars[events[i].data.sock]);
					}
				}

			} else if (events[i].events & MOS_EPOLLOUT) {
				struct server_vars *sv = &ctx->svars[events[i].data.sock];
				if (sv->rspheader_sent) {
					SendUntilAvailable(ctx, events[i].data.sock, sv);
				} else {
					TRACE_APP("Socket %d: Response header not sent yet.\n", 
							events[i].data.sock);
				}

			} else {
				assert(0);
			}
		}

		/* if do_accept flag is set, accept connections */
		if (do_accept) {
			while (1) {
				ret = AcceptConnection(ctx, ctx->listener);
				if (ret < 0)
					break;
			}
		}

	}

	return;
}
/*----------------------------------------------------------------------------*/
void 
RunApplication(mctx_t mctx) 
{
	void *app_ctx;

	app_ctx = (void *)calloc(1, sizeof(void *));
	if (!app_ctx) {
		TRACE_ERROR("calloc failure\n");
		return;
	}

	TRACE_INFO("run application on core %d\n", mctx->cpu);
	InitServer(mctx, &(app_ctx));
	RunServer(mctx, &(app_ctx));
}
/*----------------------------------------------------------------------------*/
void * 
RunMTCP(void *arg) 
{
	int core = *(int *)arg;
	mctx_t mctx;

	mtcp_core_affinitize(core);
		
	/* mTCP Initialization */
	mctx = mtcp_create_context(core);
	if (!mctx) {
		pthread_exit(NULL);
		TRACE_ERROR("Failed to craete mtcp context.\n");
		return NULL;
	}

	/* Run application here */
	RunApplication(mctx);

	/* mTCP Tear Down */
	mtcp_destroy_context(mctx);

	return NULL;
}
/*----------------------------------------------------------------------------*/
int 
main(int argc, char **argv)
{
	int ret/*, i*/;
	int current_core = -1;
	char *fname = "config/mos.conf";	

	int opt;
	while ((opt = getopt(argc, argv, "f:c:")) != -1) {
		switch (opt) {
			case 'f':
				fname = optarg;			      
				break;
			case 'c':
				current_core = atoi(optarg);
				if (current_core >= MAX_CPUS) {
					TRACE_ERROR("Invalid core id!\n");
					exit(EXIT_FAILURE);
				}
				break;
			default:
				printf("Usage: %s [-f config_file]\n", argv[0]);
				return 0;
		}

	}

	core_limit = sysconf(_SC_NPROCESSORS_ONLN);

	ret = mtcp_init(fname);
	if (ret) {
		TRACE_ERROR("Failed to initialize mtcp.\n");
		exit(EXIT_FAILURE);
	}

	if (current_core < 0) {
		TRACE_ERROR("Invalid core id.\n");
		exit(EXIT_FAILURE);
	}

	mtcp_getconf(&g_mcfg);

	core_limit = g_mcfg.num_cores;

	GlobInitServer();

	RunMTCP(&current_core);

	mtcp_destroy();
	return 0;
}
/*----------------------------------------------------------------------------*/
