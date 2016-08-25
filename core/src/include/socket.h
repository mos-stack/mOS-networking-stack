#ifndef __SOCKET_H_
#define __SOCKET_H_

#include <sys/queue.h>
#include "tcp_stream_queue.h"
#include "mtcp_epoll.h"
#include "mos_api.h"
#include "bpf/sfbpf.h"
#include "bpf/sfbpf_dlt.h"
#include "event_callback.h"
#include "tcp_rb.h"
#include "scalable_event.h"

/*----------------------------------------------------------------------------*/
#ifndef __SOCKET_MAP
#define __SOCKET_MAP
typedef struct socket_map * socket_map_t;
#endif
/*----------------------------------------------------------------------------*/
enum socket_opts
{
	MTCP_NONBLOCK		= 0x01,
	MTCP_ADDR_BIND		= 0x02, 
};
/*----------------------------------------------------------------------------*/
struct tcp_listener
{
	int sockid;
	socket_map_t socket;

	int backlog;
	stream_queue_t acceptq;
	
	pthread_mutex_t accept_lock;
	pthread_cond_t accept_cond;
};
/*----------------------------------------------------------------------------*/
struct mon_listener
{
	socket_map_t socket;
	void *uctx;

	uint8_t ude_id;
	uint8_t client_buf_mgmt: 2,	/* controls different buf mgmt modes (client-side) */
		server_buf_mgmt: 2,	/* controls different buf mgmt modes (server-side) */
		client_mon: 1,		/* controls client-side monitoring */
		server_mon: 1,		/* controls server-side monitoring */
		is_stream_syn_filter_hit: 1;
	
	//	struct sfbpf_program fcode;
	
	union {
		/** For MOS_SOCK_MONITOR_RAW type socket **/
		struct sfbpf_program raw_pkt_fcode;
		/** For MOS_SOCK_MONITOR_STREAM type socket **/
		struct {
			struct sfbpf_program stream_syn_fcode;
			struct sfbpf_program stream_orphan_fcode;
		};
	};

	struct event_queue *eq;

#ifdef NEWEV
	stree_t *stree_dontcare;
	stree_t *stree_pre_rcv;
	stree_t *stree_post_snd;
#else
	struct ev_base dontcare_evb;
	struct ev_base pre_tcp_evb;
	struct ev_base post_tcp_evb;
#endif

	TAILQ_ENTRY(mon_listener) link;
};
/*----------------------------------------------------------------------------*/
struct mon_stream
{
	socket_map_t socket;
	struct tcp_stream *stream;
	void *uctx;

	/* 
	 * offset that points to the monitoring stream offset.
	 * This variable will eventually be moved to socket.
	 */
	loff_t peek_offset[MOS_SIDE_BOTH];
	struct mon_listener *monitor_listener;

#ifdef NEWEV
	stree_t *stree_dontcare;
	stree_t *stree_pre_rcv;
	stree_t *stree_post_snd;
#else
	struct ev_pointer dontcare_evp;
	struct ev_pointer pre_tcp_evp;
	struct ev_pointer post_tcp_evp;
#endif

	uint8_t client_buf_mgmt: 2,
		server_buf_mgmt: 2,
		client_mon: 1,
		server_mon: 1;
};
/*----------------------------------------------------------------------------*/
struct socket_map
{
	int id;
	int socktype;
	uint32_t opts;
	uint8_t forward;

	struct sockaddr_in saddr;

	union {
		struct tcp_stream *stream;
		struct tcp_listener *listener;
		struct mon_listener *monitor_listener;
		struct mon_stream *monitor_stream;
		struct mtcp_epoll *ep;
		struct pipe *pp;
	};

	uint64_t epoll;			/* registered events */
	uint64_t events;		/* available events */
	mtcp_epoll_data_t ep_data;

	TAILQ_ENTRY (socket_map) link;
};
/*----------------------------------------------------------------------------*/
socket_map_t 
AllocateSocket(mctx_t mctx, int socktype);
/*----------------------------------------------------------------------------*/
void 
FreeSocket(mctx_t mctx, int sockid, int socktype); 
/*----------------------------------------------------------------------------*/
socket_map_t 
GetSocket(mctx_t mctx, int sockid);
/*----------------------------------------------------------------------------*/

#endif /* __SOCKET_H_ */
