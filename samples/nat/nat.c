
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
#include <linux/if_ether.h>
#include <linux/tcp.h>

#include <sys/queue.h>

#include <mos_api.h>
#include <mtcp_util.h>

#include "cpu.h"
#include "http_parsing.h"
#include "debug.h"
#include "applib.h"

/* update_curpacket()'s byte offsets */
#define OFFSET_DST_IP		16
#define OFFSET_SRC_IP		12
#define OFFSET_DST_PORT		 2
#define OFFSET_SRC_PORT		 0

/* default configure file path */
#define MOS_CONFIG_FILE		"config/mos.conf"

#define MAX_CORES 16

enum {
	SRC,
	DST,
};

struct port {
	uint16_t port;
	TAILQ_ENTRY(port) link;
};

static int g_core_limit = 1;    /* number of CPU cores used, WHY GLOBAL? */
static in_addr_t g_NATIP = 0;   /* NAT IP address */
static TAILQ_HEAD(, port) g_free_addrs;
static pthread_mutex_t g_addrlock;

static void
assign_port(mctx_t mctx, int sock)
{
	struct port *w;
	struct sockaddr_in addr[2];
	socklen_t len = sizeof(addr) * 2;

	/* remove a NAT mapping for this connection */
	if (mtcp_getpeername(mctx, sock, (struct sockaddr *)&addr, &len,
						 MOS_SIDE_CLI) < 0) {
		TRACE_ERROR("mtcp_getpeer() failed for sock=%d\n", sock);
		return;
	}
	/* assign a port number */
	pthread_mutex_lock(&g_addrlock);
	TAILQ_FOREACH(w, &g_free_addrs, link)
		if (GetRSSCPUCore(g_NATIP, addr[MOS_SIDE_SVR].sin_addr.s_addr,
						  w->port, addr[MOS_SIDE_SVR].sin_port, g_core_limit)
			 == mctx->cpu)
			break;
	if (w) {
		TAILQ_REMOVE(&g_free_addrs, w, link);
		mtcp_set_uctx(mctx, sock, w);
	} else {
		/* we're running out of available ports */
		/* FIXME: Handle this */
		TRACE_ERROR("No suitable port found! (CPU %d)\n", mctx->cpu);
	}
	pthread_mutex_unlock(&g_addrlock);
}

static int
set_addr(mctx_t mctx, int sock, uint32_t ip, uint16_t port, int part)
{
	int off_ip, off_port;
	
	if (part == SRC) {
		off_ip = OFFSET_SRC_IP;
		off_port = OFFSET_SRC_PORT;
	} else /* if (part == DST) */ {
		off_ip = OFFSET_DST_IP;
		off_port = OFFSET_DST_PORT;
	}

	if (mtcp_setlastpkt(mctx, sock, 0, off_ip,
				  (uint8_t *)&ip, sizeof(in_addr_t),
				  MOS_IP_HDR | MOS_OVERWRITE) < 0) {
		TRACE_ERROR("mtcp_setlastpkt() failed\n");
		return -1;
	}
	if (mtcp_setlastpkt(mctx, sock, 0, off_port,
				  (uint8_t *)&port, sizeof(in_port_t),
				  MOS_TCP_HDR | MOS_OVERWRITE
				  | MOS_UPDATE_IP_CHKSUM | MOS_UPDATE_TCP_CHKSUM) < 0) {
		TRACE_ERROR("mtcp_setlastpkt() failed\n");
		return -1;
	}

	return 0;
}

static void
translate_addr(mctx_t mctx, int sock, int side, uint64_t events,
		filter_arg_t *arg)
{
	struct port *w;

	if (!(w = mtcp_get_uctx(mctx, sock)))
		assign_port(mctx, sock);

	/* Translate the addresses */
	if (side == MOS_SIDE_CLI) {
		/* CLI (LAN) ==> SVR (WAN) : SNAT */
		if (!(w = mtcp_get_uctx(mctx, sock)))
			return;

		if (set_addr(mctx, sock, g_NATIP, w->port, SRC) < 0 &&
			mtcp_setlastpkt(mctx, sock, side, 0, NULL, 0, MOS_DROP) < 0)
			exit(EXIT_FAILURE);
	} else /* if (side == MOS_SIDE_SVR) */ {
		/* SVR (WAN) ==> CLI (LAN) : DNAT */
		struct sockaddr_in addr[2];
		socklen_t len = sizeof(addr) * 2;

		if (mtcp_getpeername(mctx, sock, (struct sockaddr *)&addr, &len,
					MOS_SIDE_CLI) < 0) {
			TRACE_ERROR("mtcp_getpeer() failed sock=%d side=%d\n", sock, side);
			return;
		}

		if (set_addr(mctx, sock, addr[MOS_SIDE_CLI].sin_addr.s_addr,
					 addr[MOS_SIDE_CLI].sin_port, DST) < 0 &&
			mtcp_setlastpkt(mctx, sock, side, 0, NULL, 0, MOS_DROP) < 0)
			exit(EXIT_FAILURE);
	}
}

static void
release_port(mctx_t mctx, int sock, int side, uint64_t events,
		filter_arg_t *arg)
{
	/* release the port number */
	struct port *w;

	if (!(w = mtcp_get_uctx(mctx, sock)))
		return;

	/* assign a port number */
	pthread_mutex_lock(&g_addrlock);
	TAILQ_INSERT_TAIL(&g_free_addrs, w, link);
	mtcp_set_uctx(mctx, sock, NULL);
	pthread_mutex_unlock(&g_addrlock);
}
/*----------------------------------------------------------------------------*/
static void
init_monitor(mctx_t mctx)
{
	int lsock = mtcp_socket(mctx, AF_INET, MOS_SOCK_MONITOR_STREAM, 0);
	if (lsock < 0) {
		TRACE_ERROR("Failed to create monitor raw socket!\n");
		return;
	}

	if (mtcp_register_callback(mctx, lsock, MOS_ON_PKT_IN, MOS_HK_SND,
							   translate_addr))
		exit(EXIT_FAILURE);
	if (mtcp_register_callback(mctx, lsock, MOS_ON_CONN_END, MOS_HK_SND,
							   release_port))
		exit(EXIT_FAILURE);
}
/*----------------------------------------------------------------------------*/
int 
main(int argc, char **argv)
{
	int i, opt;
	char *fname = MOS_CONFIG_FILE;  /* path to the default mos config file */
	struct mtcp_conf mcfg;          /* mOS configuration */
	mctx_t mctx_list[MAX_CORES];                     /* mOS context */

	/* get the total # of cpu cores */
	g_core_limit = GetNumCPUs();       

	/* Parse command line arguments */
	while ((opt = getopt(argc, argv, "c:f:i:")) != -1) {
		switch (opt) {
		case 'f':
			fname = optarg;
			break;
		case 'c':
			if (atoi(optarg) > g_core_limit) {
				printf("Available number of CPU cores is %d\n", g_core_limit);
				return -1;
			}
			g_core_limit = atoi(optarg);
			break;
		case 'i':
			g_NATIP = inet_addr(optarg);
			break;
		default:
			printf("Usage: %s [-f mos_config_file] [-c #_of_cpu] [-i ip_address]\n", argv[0]);
			return 0;
		}
	}

	/* NAT IP address checking */
	if (!g_NATIP) {
		fprintf(stderr, "You have to specify IP address of NAT with '-i' option\n");
		exit(EXIT_FAILURE);
	}

	/* parse mos configuration file */
	if (mtcp_init(fname)) {
		fprintf(stderr, "Failed to initialize mtcp.\n");
		exit(EXIT_FAILURE);
	}

	/* set the core limit */
	mtcp_getconf(&mcfg);
	mcfg.num_cores = g_core_limit;
	mtcp_setconf(&mcfg);

	/* Initialize global data structure */
	pthread_mutex_init(&g_addrlock, NULL);
	TAILQ_INIT(&g_free_addrs);
	for (i = 1025; i < 65535; i++) {
		struct port *p = malloc(sizeof(struct port));
		if (!p)
			exit(EXIT_FAILURE);
		p->port = htons(i);
		TAILQ_INSERT_TAIL(&g_free_addrs, p, link);
	}

	for (i = 0; i < g_core_limit; i++) {
		/* Run mOS for each CPU core */
		if (!(mctx_list[i] = mtcp_create_context(i))) {
			fprintf(stderr, "Failed to craete mtcp context.\n");
			return -1;
		}

		/* init monitor */
		init_monitor(mctx_list[i]);
	}

	/* wait until mOS finishes */
	for (i = 0; i < g_core_limit; i++)
		mtcp_app_join(mctx_list[i]);

	mtcp_destroy();
	return 0;
}
/*----------------------------------------------------------------------------*/
