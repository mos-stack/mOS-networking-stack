#ifndef __MTCP_H_
#define __MTCP_H_

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/queue.h>
#include <pthread.h>

#include "memory_mgt.h"
#include "tcp_ring_buffer.h"
#include "tcp_send_buffer.h"
#include "tcp_stream_queue.h"
#include "socket.h"
#include "mtcp_api.h"
#include "eventpoll.h"
#include "addr_pool.h"
#include "logger.h"
#include "stat.h"
#include "io_module.h"
#include "key_value_store.h"

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#ifndef likely
#define likely(x)       __builtin_expect((x),1)
#endif
#ifndef unlikely
#define unlikely(x)     __builtin_expect((x),0)
#endif

#ifdef ENABLE_DEBUG_EVENT
#define DBG_BUF_LEN 2048
#endif

#define MAX_CPUS 16

/*
 * Ethernet frame overhead
 */
#ifndef ETHER_CRC_LEN
#define ETHER_CRC_LEN			4
#endif
#define ETHER_IFG                       12
#define ETHER_PREAMBLE                  8
#define ETHER_OVR                       (ETHER_CRC_LEN + ETHER_PREAMBLE + ETHER_IFG)

#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif
#define ETHERNET_HEADER_LEN		14	// sizeof(struct ethhdr)
#ifdef ENABLE_PCAP
#define ETHERNET_FRAME_LEN      4096
#else
#define ETHERNET_FRAME_LEN      1514	// max possible frame len
#endif
#define IP_HEADER_LEN			20	// sizeof(struct iphdr)
#define TCP_HEADER_LEN			20	// sizeof(struct tcphdr)
#define TOTAL_TCP_HEADER_LEN		54	// total header length

/* configrations */
#define BACKLOG_SIZE 			(10*1024)
#define MAX_PKT_SIZE 			(2*1024)
#define ETH_NUM 16
#define LOGFLD_NAME_LEN 1024
#define APP_NAME_LEN 40
#define MOS_APP 20

#define TCP_OPT_TIMESTAMP_ENABLED   	TRUE
#define TCP_OPT_SACK_ENABLED        	FALSE

//#define USE_TIMER_POOL

#ifdef DARWIN
#define USE_SPIN_LOCK		FALSE
#else
#define USE_SPIN_LOCK		TRUE
#endif
#define LOCK_STREAM_QUEUE	FALSE
#define INTR_SLEEPING_MTCP	TRUE
#define PROMISCUOUS_MODE	TRUE

#define MTCP_SET(a, x)		(a |= x)
#define MTCP_ISSET(a, x)	(a & x)
#define MTCP_CLR(a)		(a = 0)

#define CB_SET(a, x)		MTCP_SET(a, x)
#define CB_ISSET(a, x)		MTCP_ISSET(a, x)
#define CB_CLR(a)		MTCP_CLR(a)

#define ACTION_SET(a,x)		MTCP_SET(a, x)
#define ACTION_ISSET(a, x)	MTCP_ISSET(a, x)
#define ACTION_CLR(a)		MTCP_CLR(a)

/*----------------------------------------------------------------------------*/
/* Statistics */
#ifdef NETSTAT
#define NETSTAT_PERTHREAD	TRUE
#define NETSTAT_TOTAL		TRUE
#endif /* NETSTAT */
#define RTM_STAT			FALSE
/*----------------------------------------------------------------------------*/
/* Lock definitions for socket buffer */
#if USE_SPIN_LOCK
#define SBUF_LOCK_INIT(lock, errmsg, action);		\
	if (pthread_spin_init(lock, PTHREAD_PROCESS_PRIVATE)) {		\
		perror("pthread_spin_init" errmsg);			\
		action;										\
	}
#define SBUF_LOCK_DESTROY(lock)	pthread_spin_destroy(lock)
#define SBUF_LOCK(lock)			pthread_spin_lock(lock)
#define SBUF_UNLOCK(lock)		pthread_spin_unlock(lock)
#else
#define SBUF_LOCK_INIT(lock, errmsg, action);		\
	if (pthread_mutex_init(lock, NULL)) {			\
		perror("pthread_mutex_init" errmsg);		\
		action;										\
	}
#define SBUF_LOCK_DESTROY(lock)	pthread_mutex_destroy(lock)
#define SBUF_LOCK(lock)			pthread_mutex_lock(lock)
#define SBUF_UNLOCK(lock)		pthread_mutex_unlock(lock)
#endif /* USE_SPIN_LOCK */

/* add macro if it is not defined in /usr/include/sys/queue.h */
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, tvar)                      \
	for ((var) = TAILQ_FIRST((head));                               \
	     (var) && ((tvar) = TAILQ_NEXT((var), field), 1);		\
	     (var) = (tvar))
#endif
/*----------------------------------------------------------------------------*/
struct timer;
/*----------------------------------------------------------------------------*/
struct eth_table
{
	char dev_name[128];
	int ifindex;
	int stat_print;
	unsigned char haddr[ETH_ALEN];
	uint32_t netmask;
	uint32_t ip_addr;
	uint64_t cpu_mask;
};
/*----------------------------------------------------------------------------*/
struct route_table
{
	uint32_t daddr;
	uint32_t mask;
	uint32_t masked;
	int prefix;
	int nif;
};
/*----------------------------------------------------------------------------*/
struct arp_entry
{
	uint32_t ip;
	int8_t prefix;
	uint32_t ip_mask;
	uint32_t ip_masked;
	unsigned char haddr[ETH_ALEN];
};
/*----------------------------------------------------------------------------*/
struct arp_table
{
	struct arp_entry *entry;
	int entries;
};
/*----------------------------------------------------------------------------*/
struct mtcp_sender
{
	int ifidx;

	TAILQ_HEAD (control_head, tcp_stream) control_list;
	TAILQ_HEAD (send_head, tcp_stream) send_list;
	TAILQ_HEAD (ack_head, tcp_stream) ack_list;

	int control_list_cnt;
	int send_list_cnt;
	int ack_list_cnt;
};
/*----------------------------------------------------------------------------*/
struct mtcp_manager
{
	mem_pool_t bufseg_pool;
	mem_pool_t sockent_pool;    /* memory pool for msock list entry */
#ifdef USE_TIMER_POOL
	mem_pool_t timer_pool;		/* memory pool for struct timer */
#endif
	mem_pool_t evt_pool;		/* memory pool for struct ev_table */
	mem_pool_t flow_pool;		/* memory pool for tcp_stream */
	mem_pool_t rv_pool;			/* memory pool for recv variables */
	mem_pool_t sv_pool;			/* memory pool for send variables */
	mem_pool_t mv_pool;			/* memory pool for monitor variables */

	kvs_t *ev_store;

	//mem_pool_t socket_pool;
	sb_manager_t rbm_snd;

	struct hashtable *tcp_flow_table;

	uint32_t s_index;		/* stream index */
	/* ptr to smaps for end applications */
	socket_map_t smap;
	/* ptr to smaps for monitor applications */
	socket_map_t msmap;
	/* free socket map */
	TAILQ_HEAD (, socket_map) free_smap;
	/* free monitor socket map */
	TAILQ_HEAD (, socket_map) free_msmap;

	addr_pool_t ap;			/* address pool */

	uint32_t g_id;			/* id space in a thread */
	uint32_t flow_cnt;		/* number of concurrent flows */

	struct mtcp_thread_context* ctx;
	
	/* variables related to logger */
	int sp_fd;
	log_thread_context* logger;
	log_buff* w_buffer;
	FILE *log_fp;

	/* variables related to event */
	struct mtcp_epoll *ep;
	uint32_t ts_last_event;

	struct tcp_listener *listener;
	TAILQ_HEAD(, mon_listener) monitors;
	int num_msp;                    /* # of MOS_SOCK_MONITOR_STREAM */
	struct pkt_ctx *pctx;			/* current pkt context */

	stream_queue_t connectq;				/* streams need to connect */
	stream_queue_t sendq;				/* streams need to send data */
	stream_queue_t ackq;					/* streams need to send ack */

	stream_queue_t closeq;				/* streams need to close */
	stream_queue_int *closeq_int;		/* internally maintained closeq */
	stream_queue_t resetq;				/* streams need to reset */
	stream_queue_int *resetq_int;		/* internally maintained resetq */
	
	stream_queue_t destroyq;				/* streams need to be destroyed */

	struct mtcp_sender *g_sender;
	struct mtcp_sender *n_sender[ETH_NUM];

	/* lists related to timeout */
	struct rto_hashstore* rto_store;
	TAILQ_HEAD (timewait_head, tcp_stream) timewait_list;
	TAILQ_HEAD (timeout_head, tcp_stream) timeout_list;
	TAILQ_HEAD (timer_head, timer) timer_list;

	int rto_list_cnt;
	int timewait_list_cnt;
	int timeout_list_cnt;

	uint32_t cur_ts;

	int wakeup_flag;
	int is_sleeping;

	/* statistics */
	struct bcast_stat bstat;
	struct timeout_stat tstat;
#ifdef NETSTAT
	struct net_stat nstat;
	struct net_stat p_nstat;
	uint32_t p_nstat_ts;

	struct run_stat runstat;
	struct run_stat p_runstat;

	struct time_stat rtstat;
#endif /* NETSTAT */
	struct io_module_func *iom;

#ifdef ENABLE_DEBUG_EVENT
	char dbg_buf[DBG_BUF_LEN];
#endif
};
/*----------------------------------------------------------------------------*/
#ifndef __MTCP_MANAGER
#define __MTCP_MANAGER
typedef struct mtcp_manager* mtcp_manager_t;
#endif
/*----------------------------------------------------------------------------*/
mtcp_manager_t 
GetMTCPManager(mctx_t mctx);
/*----------------------------------------------------------------------------*/
struct mtcp_thread_context
{
	int cpu;
	pthread_t thread;
	uint8_t done:1, 
			exit:1, 
			interrupt:1;

	struct mtcp_manager* mtcp_manager;

	void *io_private_context;

	pthread_mutex_t flow_pool_lock;
	pthread_mutex_t socket_pool_lock;

#if LOCK_STREAM_QUEUE
#if USE_SPIN_LOCK
	pthread_spinlock_t connect_lock;
	pthread_spinlock_t close_lock;
	pthread_spinlock_t reset_lock;
	pthread_spinlock_t sendq_lock;
	pthread_spinlock_t ackq_lock;
	pthread_spinlock_t destroyq_lock;
#else
	pthread_mutex_t connect_lock;
	pthread_mutex_t close_lock;
	pthread_mutex_t reset_lock;
	pthread_mutex_t sendq_lock;
	pthread_mutex_t ackq_lock;
	pthread_mutex_t destroyq_lock;
#endif /* USE_SPIN_LOCK */
#endif /* LOCK_STREAM_QUEUE */
};
/*----------------------------------------------------------------------------*/
typedef struct mtcp_thread_context* mtcp_thread_context_t;
/*----------------------------------------------------------------------------*/
struct mtcp_manager *g_mtcp[MAX_CPUS];
struct mtcp_context *g_ctx[MAX_CPUS];
extern addr_pool_t ap[ETH_NUM];
/*----------------------------------------------------------------------------*/
void 
RunPassiveLoop(mtcp_manager_t mtcp);
/*----------------------------------------------------------------------------*/
#endif /* __MTCP_H_ */
