#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include <assert.h>

#include <mos_api.h>
#include "cpu.h"
#include "rss.h"
#include "http_parsing.h"
#include "debug.h"
#include "applib.h"

#define CONFIG_FILE       "config/epwget.conf"
static struct conf_var g_conf[] = {
	{ "url",               {0} },
	{ "total_flows",       {0} },
	{ "core_limit",        {0} },
	{ "total_concurrency", {0} },
	{ "dest_port",         {0} },
};
#define NUM_CONF_VAR (sizeof(g_conf) / sizeof(struct conf_var))

#define PORT_NUM 80
//#define PORT_NUM 3333

#define MAX_URL_LEN 128
#define MAX_FILE_LEN 128
#define HTTP_HEADER_LEN 1024

#define IP_RANGE 1
#define MAX_IP_STR_LEN 16

#define BUF_SIZE (8*1024)

/*----------------------------------------------------------------------------*/
struct mtcp_conf g_mcfg;
static pthread_t mtcp_thread[MAX_CPUS];
/*----------------------------------------------------------------------------*/
static mctx_t g_mctx[MAX_CPUS];
static int done[MAX_CPUS];
/*----------------------------------------------------------------------------*/
static int num_cores;
static int core_limit;
/*----------------------------------------------------------------------------*/
static int fio = FALSE;
static char outfile[MAX_FILE_LEN + 1];
/*----------------------------------------------------------------------------*/
static char host[MAX_IP_STR_LEN + 1];
static char path[MAX_URL_LEN + 1];
static in_addr_t daddr;
static in_port_t dport;
static in_addr_t saddr;
/*----------------------------------------------------------------------------*/
static int total_flows;
static int flows[MAX_CPUS];
static int flowcnt = 0;
static int concurrency;
static int max_fds;
static uint16_t dest_port;
/*----------------------------------------------------------------------------*/
struct wget_stat
{
	uint64_t waits;
	uint64_t events;
	uint64_t connects;
	uint64_t reads;
	uint64_t writes;
	uint64_t completes;

	uint64_t errors;
	uint64_t timedout;

	uint64_t sum_resp_time;
	uint64_t max_resp_time;
};
/*----------------------------------------------------------------------------*/
struct thread_context
{
	int core;

	mctx_t mctx;
	int ep;
	struct wget_vars *wvars;

	int target;
	int started;
	int errors;
	int incompletes;
	int done;
	int pending;

	int maxevents;
	struct mtcp_epoll_event *events;

	struct wget_stat stat;
};
typedef struct thread_context* thread_context_t;
/*----------------------------------------------------------------------------*/
struct wget_vars
{
	int request_sent;

	char response[HTTP_HEADER_LEN];
	int resp_len;
	int headerset;
	uint32_t header_len;
	uint64_t file_len;
	uint64_t recv;
	uint64_t write;

	struct timeval t_start;
	struct timeval t_end;
	
	int fd;
};
/*----------------------------------------------------------------------------*/
static struct thread_context *g_ctx[MAX_CPUS];
static struct wget_stat *g_stat[MAX_CPUS];
/*----------------------------------------------------------------------------*/
static thread_context_t 
CreateContext(mctx_t mctx)
{
	thread_context_t ctx;

	ctx = (thread_context_t)calloc(1, sizeof(struct thread_context));
	if (!ctx) {
		perror("malloc");
		TRACE_ERROR("Failed to allocate memory for thread context.\n");
		return NULL;
	}

	ctx->mctx = mctx;
	ctx->core = mctx->cpu;

	g_mctx[ctx->core] = mctx;

	return ctx;
}
/*----------------------------------------------------------------------------*/
static void 
DestroyContext(thread_context_t ctx) 
{
	free(ctx);
}
/*----------------------------------------------------------------------------*/
static inline int 
CreateConnection(thread_context_t ctx)
{
	mctx_t mctx = ctx->mctx;
	struct mtcp_epoll_event ev;
	struct sockaddr_in addr;
	int sockid;
	int ret;

	assert(mctx);

	errno = 0;
	sockid = mtcp_socket(mctx, AF_INET, SOCK_STREAM, 0);
	if (sockid < 0) {
		TRACE_INFO("Failed to create socket! (%s)\n",
			   strerror(errno));
		return -1;
	}
	memset(&ctx->wvars[sockid], 0, sizeof(struct wget_vars));
	ret = mtcp_setsock_nonblock(mctx, sockid);
	if (ret < 0) {
		TRACE_ERROR("Failed to set socket in nonblocking mode.\n");
		exit(-1);
	}

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = daddr;
	addr.sin_port = dport;
	
	ret = mtcp_connect(mctx, sockid, 
			(struct sockaddr *)&addr, sizeof(struct sockaddr_in));
	if (ret < 0) {
		if (errno != EINPROGRESS) {
			perror("mtcp_connect");
			mtcp_close(mctx, sockid);
			return -1;
		}
	}

	ctx->started++;
	ctx->pending++;
	ctx->stat.connects++;

	ev.events = MOS_EPOLLOUT;
	ev.data.sock = sockid;
	mtcp_epoll_ctl(mctx, ctx->ep, MOS_EPOLL_CTL_ADD, sockid, &ev);

	return sockid;
}
/*----------------------------------------------------------------------------*/
static inline void 
CloseConnection(thread_context_t ctx, int sockid)
{
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MOS_EPOLL_CTL_DEL, sockid, NULL);
	mtcp_close(ctx->mctx, sockid);
	ctx->pending--;
	ctx->done++;
	assert(ctx->pending >= 0);
	while (/*ctx->pending*/ mtcp_get_connection_cnt(ctx->mctx) < concurrency && ctx->started < ctx->target) {
		if (CreateConnection(ctx) < 0) {
			done[ctx->core] = TRUE;
			break;
		}
	}
}
/*----------------------------------------------------------------------------*/
static inline int 
SendHTTPRequest(thread_context_t ctx, int sockid, struct wget_vars *wv)
{
	char request[HTTP_HEADER_LEN];
	struct mtcp_epoll_event ev;
	int wr;
	int len;

	wv->headerset = FALSE;
	wv->recv = 0;
	wv->header_len = wv->file_len = 0;

	snprintf(request, HTTP_HEADER_LEN, "GET %s HTTP/1.0\r\n"
			"User-Agent: Wget/1.12 (linux-gnu)\r\n"
			"Accept: */*\r\n"
			"Host: %s\r\n"
//			"Connection: Keep-Alive\r\n\r\n", 
			"Connection: Close\r\n\r\n", 
			path, host);
	len = strlen(request);

	wr = mtcp_write(ctx->mctx, sockid, request, len);
	if (wr < len) {
		TRACE_ERROR("Socket %d: Sending HTTP request failed. "
				"try: %d, sent: %d\n", sockid, len, wr);
		CloseConnection(ctx, sockid);
	}
	ctx->stat.writes += wr;
	TRACE_APP("Socket %d HTTP Request of %d bytes. sent.\n", sockid, wr);
	wv->request_sent = TRUE;

	ev.events = MOS_EPOLLIN;
	ev.data.sock = sockid;
	mtcp_epoll_ctl(ctx->mctx, ctx->ep, MOS_EPOLL_CTL_MOD, sockid, &ev);

	gettimeofday(&wv->t_start, NULL);

	char fname[MAX_FILE_LEN + 1];
	if (fio) {
		snprintf(fname, MAX_FILE_LEN, "%s.%d", outfile, flowcnt++);
		wv->fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (wv->fd < 0) {
			TRACE_APP("Failed to open file descriptor for %s\n", fname);
			exit(1);
		}
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
static inline int 
DownloadComplete(thread_context_t ctx, int sockid, struct wget_vars *wv)
{
#ifdef APP
	mctx_t mctx = ctx->mctx;
#endif
	uint64_t tdiff;

	TRACE_APP("Socket %d File download complete!\n", sockid);
	gettimeofday(&wv->t_end, NULL);
	CloseConnection(ctx, sockid);
	ctx->stat.completes++;

	if (wv->recv - wv->header_len != wv->file_len) {
		fprintf(stderr, "Response size mismatch! "
						"actual recved: %ld,  expected to recved: %ld\n", 
						wv->recv-wv->header_len, wv->file_len);
	}

	tdiff = (wv->t_end.tv_sec - wv->t_start.tv_sec) * 1000000 + 
			(wv->t_end.tv_usec - wv->t_start.tv_usec);
	TRACE_APP("Socket %d Total received bytes: %lu (%luMB)\n", 
			sockid, wv->recv, wv->recv / 1000000);
	TRACE_APP("Socket %d Total spent time: %lu us\n", sockid, tdiff);
	if (tdiff > 0) {
		TRACE_APP("Socket %d Average bandwidth: %lf[MB/s]\n", 
				sockid, (double)wv->recv / tdiff);
	}
	ctx->stat.sum_resp_time += tdiff;
	if (tdiff > ctx->stat.max_resp_time)
		ctx->stat.max_resp_time = tdiff;

	if (fio && wv->fd > 0)
		close(wv->fd);

	return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
HandleReadEvent(thread_context_t ctx, int sockid, struct wget_vars *wv)
{
	mctx_t mctx = ctx->mctx;
	char buf[BUF_SIZE];
	char *pbuf;
	int rd, copy_len;

	rd = 1;
	while (rd > 0) {
		rd = mtcp_read(mctx, sockid, buf, BUF_SIZE);
		if (rd <= 0)
			break;
		ctx->stat.reads += rd;

		TRACE_APP("Socket %d: mtcp_read ret: %d, total_recv: %lu, "
				"header_set: %d, header_len: %u, file_len: %lu\n", 
				sockid, rd, wv->recv + rd, 
				wv->headerset, wv->header_len, wv->file_len);

		pbuf = buf;
		if (!wv->headerset) {
			copy_len = MIN(rd, HTTP_HEADER_LEN - wv->resp_len);
			memcpy(wv->response + wv->resp_len, buf, copy_len);
			wv->resp_len += copy_len;
			wv->header_len = find_http_header(wv->response, wv->resp_len);
			if (wv->header_len > 0) {
				wv->response[wv->header_len] = '\0';
				wv->file_len = http_header_long_val(wv->response, 
						CONTENT_LENGTH_HDR, sizeof(CONTENT_LENGTH_HDR) - 1);
				TRACE_APP("Socket %d Parsed response header. "
						"Header length: %u, File length: %lu (%luMB)\n", 
						sockid, wv->header_len, 
						wv->file_len, wv->file_len / 1024 / 1024);
				wv->headerset = TRUE;
				wv->recv += (rd - (wv->resp_len - wv->header_len));
				
				pbuf += (rd - (wv->resp_len - wv->header_len));
				rd = (wv->resp_len - wv->header_len);

			} else {
				/* failed to parse response header */
				wv->recv += rd;
				rd = 0;
				ctx->stat.errors++;
				ctx->errors++;
				CloseConnection(ctx, sockid);
				return 0;
			}
		}
		wv->recv += rd;
		
		if (fio && wv->fd > 0) {
			int wr = 0;
			while (wr < rd) {
				int _wr = write(wv->fd, pbuf + wr, rd - wr);
				assert (_wr == rd - wr);
				 if (_wr < 0) {
					 perror("write");
					 TRACE_ERROR("Failed to write.\n");
					 assert(0);
					 break;
				 }
				 wr += _wr;	
				 wv->write += _wr;
			}
		}
		
		if (wv->header_len && (wv->recv >= wv->header_len + wv->file_len)) {
			break;
		}
	}

	if (rd > 0) {
		if (wv->header_len && (wv->recv >= wv->header_len + wv->file_len)) {
			TRACE_APP("Socket %d Done Write: "
					"header: %u file: %lu recv: %lu write: %lu\n", 
					sockid, wv->header_len, wv->file_len, 
					wv->recv - wv->header_len, wv->write);
			DownloadComplete(ctx, sockid, wv);

			return 0;
		}

	} else if (rd == 0) {
		/* connection closed by remote host */
		TRACE_DBG("Socket %d connection closed with server.\n", sockid);

		if (wv->header_len && (wv->recv >= wv->header_len + wv->file_len)) {
			DownloadComplete(ctx, sockid, wv);
		} else {
			ctx->stat.errors++;
			ctx->incompletes++;
			CloseConnection(ctx, sockid);
		}

	} else if (rd < 0) {
		if (errno != EAGAIN) {
			TRACE_DBG("Socket %d: mtcp_read() error %s\n", 
					sockid, strerror(errno));
			ctx->stat.errors++;
			ctx->errors++;
			CloseConnection(ctx, sockid);
		}
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
static void 
PrintStats()
{
	struct wget_stat total = {0};
	struct wget_stat *st;
	uint64_t avg_resp_time;
	uint64_t total_resp_time = 0;
	int i;

	for (i = 0; i < core_limit; i++) {
		st = g_stat[i];
		if (!st)
			continue;
		avg_resp_time = st->completes? st->sum_resp_time / st->completes : 0;

		total.waits += st->waits;
		total.events += st->events;
		total.connects += st->connects;
		total.reads += st->reads;
		total.writes += st->writes;
		total.completes += st->completes;
		total_resp_time += avg_resp_time;
		if (st->max_resp_time > total.max_resp_time)
			total.max_resp_time = st->max_resp_time;
		total.errors += st->errors;
		total.timedout += st->timedout;

		memset(st, 0, sizeof(struct wget_stat));
	}
	fprintf(stderr, "[ ALL ] connect: %7lu, read: %4lu MB, write: %4lu MB, "
			"completes: %7lu (resp_time avg: %4lu, max: %6lu us)\n", 
			total.connects, 
			total.reads / 1024 / 1024, total.writes / 1024 / 1024, 
			total.completes, total_resp_time / core_limit, total.max_resp_time);
}
/*----------------------------------------------------------------------------*/
static void
GlbInitWget()
{
	struct mtcp_conf mcfg;
	char *url;
	int total_concurrency = 0;
	int flow_per_thread;
	int flow_remainder_cnt;
	int i;

	num_cores = GetNumCPUs();
	core_limit = num_cores;
	concurrency = 100;
	total_flows = -1;

	LoadConfig(CONFIG_FILE, g_conf, NUM_CONF_VAR);

	url = g_conf[0].value;
	total_flows = atoi(g_conf[1].value);
	core_limit = atoi(g_conf[2].value);
	total_concurrency = atoi(g_conf[3].value);
	dest_port = atoi(g_conf[4].value);

	if ((strlen(url) == 0)
			|| (strlen(url) > MAX_URL_LEN)
			|| (total_flows <= 0)) {
		TRACE_INFO("Invalid configuration\n");
		exit(0);
	}

	char* slash_p = strchr(url, '/');
	if (slash_p) {
		strncpy(host, url, slash_p - url);
		strncpy(path, strchr(url, '/'), MAX_URL_LEN);
	} else {
		strncpy(host, url, MAX_IP_STR_LEN);
		strncpy(path, "/", 1);
	}

	daddr = inet_addr(host);
	dport = (dest_port == 0) ? htons(PORT_NUM) : htons(dest_port);
	saddr = INADDR_ANY;

	if (total_flows < core_limit) {
		core_limit = total_flows;
	}

	/* per-core concurrency = total_concurrency / # cores */
	if (total_concurrency > 0)
		concurrency = total_concurrency / core_limit;

	/* set the max number of fds 3x larger than concurrency */
	max_fds = concurrency * 3;

	TRACE_CONFIG("Application configuration:\n");
	TRACE_CONFIG("URL: %s\n", path);
	TRACE_CONFIG("# of total_flows: %d\n", total_flows);
	TRACE_CONFIG("# of cores: %d\n", core_limit);
	TRACE_CONFIG("Concurrency: %d\n", total_concurrency);

	mtcp_getconf(&mcfg);
	mcfg.max_concurrency = max_fds;
	mcfg.max_num_buffers = max_fds;
	mtcp_setconf(&mcfg);

	flow_per_thread = total_flows / core_limit;
	flow_remainder_cnt = total_flows % core_limit;

	for (i = 0; i < MAX_CPUS; i++) {
		done[i] = FALSE;
		flows[i] = flow_per_thread;

		if (flow_remainder_cnt-- > 0)
			flows[i]++;
	}

	return;
}
/*----------------------------------------------------------------------------*/
static void
InitWget(mctx_t mctx, void **app_ctx)
{
	thread_context_t ctx;
	int ep;

	assert(mctx);

	int core = mctx->cpu;

	ctx = CreateContext(mctx);
	if (!ctx)
		exit(-1);
	g_ctx[core] = ctx;
	g_stat[core] = &ctx->stat;
	srand(time(NULL));

	mtcp_init_rss(mctx, saddr, IP_RANGE, daddr, dport);

	if (flows[core] == 0) {
		TRACE_DBG("Application thread %d finished.\n", core);
		exit(-1);
	}
	ctx->target = flows[core];

	/* Initialization */
	ctx->maxevents = max_fds * 3;
	ep = mtcp_epoll_create(mctx, ctx->maxevents);
	if (ep < 0) {
		TRACE_ERROR("Failed to create epoll struct!n");
		exit(EXIT_FAILURE);
	}
	ctx->events = (struct mtcp_epoll_event *)
			calloc(ctx->maxevents, sizeof(struct mtcp_epoll_event));
	if (!ctx->events) {
		TRACE_ERROR("Failed to allocate events!\n");
		exit(EXIT_FAILURE);
	}
	ctx->ep = ep;

	ctx->wvars = (struct wget_vars *)calloc(max_fds, sizeof(struct wget_vars));
	if (!ctx->wvars) {
		TRACE_ERROR("Failed to create wget variables!\n");
		exit(EXIT_FAILURE);
	}

	ctx->started = ctx->done = ctx->pending = 0;
	ctx->errors = ctx->incompletes = 0;

	*app_ctx = ctx;

	return;
}
/*----------------------------------------------------------------------------*/
static void
RunWget(mctx_t mctx, void **app_ctx)
{
	struct in_addr daddr_in;
	struct timeval cur_tv, prev_tv;
	int nevents;
	int i;

	assert(mctx);
	assert(*app_ctx);

	thread_context_t ctx = *app_ctx;
	int core = ctx->core;

	daddr_in.s_addr = daddr;
	fprintf(stderr, "Thread %d handles %d flows. connecting to %s:%u\n", 
			core, flows[core], inet_ntoa(daddr_in), ntohs(dport));

	gettimeofday(&cur_tv, NULL);
	prev_tv = cur_tv;

	while (!done[core]) {
		gettimeofday(&cur_tv, NULL);

		/* print statistics every second */
		if (core == 0 && cur_tv.tv_sec > prev_tv.tv_sec) {
			PrintStats();
			prev_tv = cur_tv;
		}

		while (/*ctx->pending*/ mtcp_get_connection_cnt(ctx->mctx) < concurrency && ctx->started < ctx->target) {
			if (CreateConnection(ctx) < 0) {
				done[core] = TRUE;
				break;
			}
		}

		nevents = mtcp_epoll_wait(mctx, ctx->ep,
				ctx->events, ctx->maxevents, ctx->pending ? -1 : 10);
		ctx->stat.waits++;
	
		if (nevents < 0) {
			if (errno != EINTR) {
				TRACE_ERROR("mtcp_epoll_wait failed! ret: %d\n", nevents);
			}
			done[core] = TRUE;
			break;
		} else {
			ctx->stat.events += nevents;
		}

		for (i = 0; i < nevents; i++) {

			if (ctx->events[i].events & MOS_EPOLLERR) {
				int err;
				socklen_t len = sizeof(err);

				TRACE_APP("[CPU %d] Error on socket %d\n", 
						core, ctx->events[i].data.sockid);
				ctx->stat.errors++;
				ctx->errors++;
				if (mtcp_getsockopt(mctx, ctx->events[i].data.sock, 
							SOL_SOCKET, SO_ERROR, (void *)&err, &len) == 0) {
					if (err == ETIMEDOUT)
						ctx->stat.timedout++;
				}
				CloseConnection(ctx, ctx->events[i].data.sock);

			} else if (ctx->events[i].events & MOS_EPOLLIN) {
				HandleReadEvent(ctx, 
						ctx->events[i].data.sock,
						&ctx->wvars[ctx->events[i].data.sock]);

			} else if (ctx->events[i].events == MOS_EPOLLOUT) {
				struct wget_vars *wv = &ctx->wvars[ctx->events[i].data.sock];

				if (!wv->request_sent) {
					SendHTTPRequest(ctx, ctx->events[i].data.sock, wv);
				} else {
					//TRACE_DBG("Request already sent.\n");
				}

			} else {
				TRACE_ERROR("Socket %d: event: %s\n", 
						ctx->events[i].data.sock,
						EventToString(ctx->events[i].events));
				assert(0);
			}
		}

		if (ctx->done >= ctx->target) {
			fprintf(stdout, "Completed %d connections, "
					"errors: %d incompletes: %d\n", 
					ctx->done, ctx->errors, ctx->incompletes);
			break;
		}
	}

	TRACE_INFO("Wget thread %d waiting for mtcp to be destroyed.\n", core);

	g_stat[core] = NULL;
	g_ctx[core] = NULL;
	DestroyContext(ctx);

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
	InitWget(mctx, &(app_ctx));
	RunWget(mctx, &(app_ctx));
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
	pthread_exit(NULL);

	return NULL;
}
/*----------------------------------------------------------------------------*/
int 
main(int argc, char **argv)
{
	int ret, i;
	int cores[MAX_CPUS];
	char *fname = "config/mos.conf";

	int opt;
	while ((opt = getopt(argc, argv, "f:")) != -1) {
		switch (opt) {
			case 'f':
				fname = optarg;
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

	mtcp_getconf(&g_mcfg);

	core_limit = g_mcfg.num_cores;

	GlbInitWget();

	for (i = 0; i < core_limit; i++) {
		cores[i] = i;

		/* Run mtcp thread */
		if ((g_mcfg.cpu_mask & (1L << i)) &&
			pthread_create(&mtcp_thread[i], NULL, RunMTCP, (void *)&cores[i])) {
			perror("pthread_create");
			TRACE_ERROR("Failed to create msg_test thread.\n");
			exit(-1);
		}
	}
	
	for (i = 0; i < core_limit; i++) {
		if (g_mcfg.cpu_mask & (1L << i))
			pthread_join(mtcp_thread[i], NULL);
		TRACE_INFO("Message test thread %d joined.\n", i);
	}

	mtcp_destroy();
	return 0;
}
/*----------------------------------------------------------------------------*/
