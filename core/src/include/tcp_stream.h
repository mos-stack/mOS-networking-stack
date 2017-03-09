#ifndef __TCP_STREAM_H_
#define __TCP_STREAM_H_

#ifdef DARWIN
#include <netinet/tcp.h>
#else
#include <linux/tcp.h>
#endif
#include <netinet/ip.h>
#include <sys/queue.h>

#include "mtcp.h"
#include "socket.h"
#include "memory_mgt.h"
#include "tcp_rb.h"

#define TCP_MAX_SEQ 		4294967295

#define HAS_STREAM_TYPE(str, type)	(str->stream_type & (1 << type))
#define IS_STREAM_TYPE(str, type)	(str->stream_type == (1 << type))
#define STREAM_TYPE(type)		(1 << type)

/*----------------------------------------------------------------------------*/
/**
 * routines for traversing stream + raw sockets 
 */
struct sockent {
	struct socket_map *sock;
	TAILQ_ENTRY(sockent) link;
};
#define SOCKQ_HEAD(name) TAILQ_HEAD(name, sockent)
#define SOCKQ_INIT(head) TAILQ_INIT(head)
#define SOCKQ_INSERT_TAIL(head, socket) \
do { \
	struct sockent *__s = \
			(struct sockent *)MPAllocateChunk(mtcp->sockent_pool); \
	if (__s) { \
		__s->sock = (socket); \
		TAILQ_INSERT_TAIL(head, __s, link); \
	} \
} while (0)
#define SOCKQ_REMOVE(head, socket) \
do { \
	struct sockent *__walk, *__temp; \
	for (__walk = TAILQ_FIRST(head); __walk != NULL; __walk = __temp) { \
		__temp = TAILQ_NEXT(__walk, link); \
		if ((socket) == __walk->sock) break; \
	} \
	if (__walk) { \
		TAILQ_REMOVE(head, __walk, link); \
		MPFreeChunk(mtcp->sockent_pool, __walk); \
	} \
} while (0)
#define SOCKQ_FOREACH_START(var, head) \
do { \
	struct sockent *__walk, *__temp; \
	for (__walk = ((head)->tqh_first); \
		((var) = __walk ? __walk->sock : NULL, __walk); \
		__walk = __temp) { \
		__temp = ((__walk)->link.tqe_next);
#define SOCKQ_FOREACH_REVERSE(var, head) \
	do { \
        struct sockent *__walk, *__temp; \
        for (__walk = (*(((struct mlist *)((head)->tqh_last))->tqh_last));	\
	((var) = __walk ? __walk->sock : NULL, __walk); \
	     __walk = __temp) { \
        __temp = (*(((struct mlist *)((__walk)->link.tqe_prev))->tqh_last));
#define SOCKQ_FOREACH_END }} while (0)
/*----------------------------------------------------------------------------*/

struct rtm_stat
{
	uint32_t tdp_ack_cnt;
	uint32_t tdp_ack_bytes;
	uint32_t ack_upd_cnt;
	uint32_t ack_upd_bytes;
#if TCP_OPT_SACK_ENABLED
	uint32_t sack_cnt;
	uint32_t sack_bytes;
	uint32_t tdp_sack_cnt;
	uint32_t tdp_sack_bytes;
#endif /* TCP_OPT_SACK_ENABLED */
	uint32_t rto_cnt;
	uint32_t rto_bytes;
};

typedef struct seq_remap_entry {
	uint32_t seq_base;
	uint32_t seq_off;
} seq_remap_entry;

#define SRE_MAX			8

#if TCP_OPT_SACK_ENABLED
struct sack_entry
{
	uint32_t left_edge;
	uint32_t right_edge;
	uint32_t expire;
};
#endif /* TCP_OPT_SACK_ENABLED */

struct tcp_recv_vars
{
	/* receiver variables */
	uint32_t rcv_wnd;		/* receive window (unscaled) */
	//uint32_t rcv_up;		/* receive urgent pointer */
	uint32_t irs;			/* initial receiving sequence */
	uint32_t snd_wl1;		/* segment seq number for last window update */
	uint32_t snd_wl2;		/* segment ack number for last window update */

	/* variables for fast retransmission */
	uint32_t last_ack_seq;	/* highest ackd seq */
	uint8_t dup_acks;		/* number of duplicated acks */
	
	/* timestamps */
	uint32_t ts_recent;			/* recent peer timestamp */
	uint32_t ts_lastack_rcvd;	/* last ack rcvd time */
	uint32_t ts_last_ts_upd;	/* last peer ts update time */
	uint32_t ts_tw_expire;	// timestamp for timewait expire

	/* RTT estimation variables */
	uint32_t srtt;			/* smoothed round trip time << 3 (scaled) */
	uint32_t mdev;			/* medium deviation */
	uint32_t mdev_max;		/* maximal mdev ffor the last rtt period */
	uint32_t rttvar;		/* smoothed mdev_max */
	uint32_t rtt_seq;		/* sequence number to update rttvar */

#if TCP_OPT_SACK_ENABLED		/* currently not used */
#define MAX_SACK_ENTRY 8
	struct sack_entry sack_table[MAX_SACK_ENTRY];
	uint8_t sacks:3;
#endif /* TCP_OPT_SACK_ENABLED */

	tcprb_t *rcvbuf;

#if USE_SPIN_LOCK
	pthread_spinlock_t read_lock;
#else
	pthread_mutex_t read_lock;
#endif
	struct hash_bucket_head *he_mybucket;
	TAILQ_ENTRY(tcp_stream) he_link;	/* hash table entry link */
};

struct tcp_send_vars
{
	/* IP-level information */
	uint16_t ip_id;

	uint16_t mss;			/* maximum segment size */
	uint16_t eff_mss;		/* effective segment size (excluding tcp option) */

	uint8_t wscale_mine;		/* my window scale (advertising window) */
	uint8_t wscale_peer;		/* peer's window scale (advertised window) */
	int8_t nif_out;			/* cached output network interface */
	unsigned char *d_haddr;	/* cached destination MAC address */

	/* send sequence variables */
	uint32_t snd_una;		/* send unacknoledged */
	uint32_t snd_wnd;		/* send window (unscaled) */
	uint32_t peer_wnd;		/* client window size */
	//uint32_t snd_up;		/* send urgent pointer (not used) */
	uint32_t iss;			/* initial sending sequence */
	uint32_t fss;			/* final sending sequence */

	/* retransmission timeout variables */
	uint8_t nrtx;			/* number of retransmission */
	uint8_t max_nrtx;		/* max number of retransmission */
	uint32_t rto;			/* retransmission timeout */
	uint32_t ts_rto;		/* timestamp for retransmission timeout */

	/* congestion control variables */
	uint32_t cwnd;				/* congestion window */
	uint32_t ssthresh;			/* slow start threshold */

	/* timestamp */
	uint32_t ts_lastack_sent;	/* last ack sent time */

	uint8_t is_wack:1, 			/* is ack for window adertisement? */
			ack_cnt:6;			/* number of acks to send. max 64 */

	uint8_t on_control_list;
	uint8_t on_send_list;
	uint8_t on_ack_list;
	uint8_t on_sendq;
	uint8_t on_ackq;
	uint8_t on_closeq;
	uint8_t on_resetq;

	uint8_t on_closeq_int:1, 
			on_resetq_int:1, 
			is_fin_sent:1, 
			is_fin_ackd:1;

	TAILQ_ENTRY(tcp_stream) control_link;
	TAILQ_ENTRY(tcp_stream) send_link;
	TAILQ_ENTRY(tcp_stream) ack_link;

	TAILQ_ENTRY(tcp_stream) timer_link;		/* timer link (rto list, tw list) */
	TAILQ_ENTRY(tcp_stream) timeout_link;	/* connection timeout link */

	struct tcp_send_buffer *sndbuf;
	struct seq_remap_entry sre[SRE_MAX];	/* seq # translation table */
	uint8_t sre_index;			/* seq # translation index */

#if USE_SPIN_LOCK
	pthread_spinlock_t write_lock;
#else
	pthread_mutex_t write_lock;
#endif

#if RTM_STAT
	struct rtm_stat rstat;			/* retransmission statistics */
#endif
};

typedef struct tcp_stream
{
	/* 
	 * This is a direct replacement for fctx...
	 * However this could be replaced by some
	 * more elaborate data structure that supports
	 * multiple monitors in the future...
	 *
	 * In case no monitor is attached, msock will be
	 * NULL.
	 *
	 * Support for standalone monitors will be patched
	 * in future revisions...
	 */

	SOCKQ_HEAD(mlist) msocks;        /* in case monitoring is enabled */
	socket_map_t socket;		/* relating to MOS_SOCK_STREAM */

	uint32_t id;
	uint32_t stream_type;		/* to identify sock_stream/mon_stream */

	uint32_t saddr;			/* in network order */
	uint32_t daddr;			/* in network order */
	uint16_t sport;			/* in network order */
	uint16_t dport;			/* in network order */
	
	uint32_t actions;
	uint64_t cb_events;

	uint8_t state;			/* tcp state */
	uint8_t close_reason;	/* close reason */
	uint8_t on_hash_table;
	uint8_t on_timewait_list;
	uint8_t ht_idx;
	uint8_t closed;
	uint8_t is_bound_addr;
	uint8_t need_wnd_adv;
	int16_t on_rto_idx;

	uint16_t on_timeout_list:1, 
		on_rcv_br_list:1, 
		on_snd_br_list:1, 
		saw_timestamp:1,	/* whether peer sends timestamp */
		sack_permit:1,		/* whether peer permits SACK */
		control_list_waiting:1, 
		have_reset:1,
		side:2,
		buffer_mgmt:2,
		status_mgmt:1,
		allow_pkt_modification:1;
	
	uint32_t snd_nxt;		/* send next */
	uint32_t rcv_nxt;		/* receive next */

	struct tcp_recv_vars *rcvvar;
	struct tcp_send_vars *sndvar;
	
	uint32_t last_active_ts;		/* ts_last_ack_sent or ts_last_ts_upd */

	struct tcp_stream *pair_stream; /* pair stream in case of monitor / proxy socket */
#ifdef RECORDPKT_PER_STREAM
	struct pkt_ctx last_pctx;
	unsigned char  last_pkt_data[ETHERNET_FRAME_LEN];
#endif

} tcp_stream;

extern inline char *
TCPStateToString(const tcp_stream *cur_stream);

extern inline int 
AddEpollEvent(struct mtcp_epoll *ep, 
		int queue_type, socket_map_t socket, uint32_t event);

extern inline void 
RaiseReadEvent(mtcp_manager_t mtcp, tcp_stream *stream);

extern inline void 
RaiseWriteEvent(mtcp_manager_t mtcp, tcp_stream *stream);

extern inline void 
RaiseCloseEvent(mtcp_manager_t mtcp, tcp_stream *stream);

extern inline int
RaiseErrorEvent(mtcp_manager_t mtcp, tcp_stream *stream);

tcp_stream *
CreateTCPStream(mtcp_manager_t mtcp, socket_map_t socket, int type, 
		uint32_t saddr, uint16_t sport, uint32_t daddr, uint16_t dport,
		unsigned int *hash);

extern inline tcp_stream *
CreateDualTCPStream(mtcp_manager_t mtcp, socket_map_t socket, int type, uint32_t saddr, 
		    uint16_t sport, uint32_t daddr, uint16_t dport, unsigned int *hash);

extern inline tcp_stream *
CreateClientTCPStream(mtcp_manager_t mtcp, socket_map_t socket, int type,
			uint32_t saddr, uint16_t sport, uint32_t daddr, uint16_t dport, unsigned int *hash);

extern inline tcp_stream *
AttachServerTCPStream(mtcp_manager_t mtcp, tcp_stream *cs, int type,
			uint32_t saddr, uint16_t sport, uint32_t daddr, uint16_t dport);

void
DestroyTCPStream(mtcp_manager_t mtcp, tcp_stream *stream);

void 
DumpStream(mtcp_manager_t mtcp, tcp_stream *stream);

int
GetFragInfo(socket_map_t sock, int side, void *optval, socklen_t *optlen);

int
GetBufInfo(socket_map_t sock, int side, void *optval, socklen_t *optlen);

int
GetTCPState(struct tcp_stream *stream, int side,
			void *optval, socklen_t *optlen);

int
DisableBuf(socket_map_t sock, int side);

int
GetLastTimestamp(struct tcp_stream *stream, uint32_t *usecs, socklen_t *sz);

void
posix_seq_srand(unsigned seed);

#endif /* __TCP_STREAM_H_ */
