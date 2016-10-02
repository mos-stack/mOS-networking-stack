#include <string.h>

#include "mtcp.h"
#include "socket.h"
#include "debug.h"
#include "eventpoll.h"
#include "timer.h"
#include "config.h"

/*---------------------------------------------------------------------------*/
void
FreeMonListener(mtcp_manager_t mtcp, socket_map_t socket)
{
	struct mon_listener *monitor = socket->monitor_listener;

#ifdef NEWEV
	stree_dec_ref(mtcp->ev_store, monitor->stree_dontcare);
	stree_dec_ref(mtcp->ev_store, monitor->stree_pre_rcv);
	stree_dec_ref(mtcp->ev_store, monitor->stree_post_snd);

	monitor->stree_dontcare = NULL;
	monitor->stree_pre_rcv = NULL;
	monitor->stree_post_snd = NULL;
#else
	CleanupEvB(mtcp, &monitor->dontcare_evb);
	CleanupEvB(mtcp, &monitor->pre_tcp_evb);
	CleanupEvB(mtcp, &monitor->post_tcp_evb);
#endif

	DestroyEventQueue(monitor->eq);

	/* Clean up monitor filter */
	if (ISSET_BPFFILTER(&monitor->stream_syn_fcode))
		sfbpf_freecode(&monitor->stream_syn_fcode);
	if (ISSET_BPFFILTER(&monitor->stream_orphan_fcode))
		sfbpf_freecode(&monitor->stream_orphan_fcode);
	
	memset(monitor, 0, sizeof(struct mon_listener));
}
/*---------------------------------------------------------------------------*/
static inline void
FreeMonStream(mtcp_manager_t mtcp, socket_map_t socket)
{
	struct mon_stream *mstream = socket->monitor_stream;

#ifdef NEWEV
	stree_dec_ref(mtcp->ev_store, mstream->stree_dontcare);
	stree_dec_ref(mtcp->ev_store, mstream->stree_pre_rcv);
	stree_dec_ref(mtcp->ev_store, mstream->stree_post_snd);

	mstream->stree_dontcare = NULL;
	mstream->stree_pre_rcv = NULL;
	mstream->stree_post_snd = NULL;
#else
	CleanupEvP(&mstream->dontcare_evp);
	CleanupEvP(&mstream->pre_tcp_evp);
	CleanupEvP(&mstream->post_tcp_evp);
#endif

	mstream->socket = NULL;
	mstream->stream = NULL;
	mstream->uctx = NULL;

	mstream->peek_offset[0] = mstream->peek_offset[1] = 0;
}
/*---------------------------------------------------------------------------*/
/**
 * XXX - TODO: This is an ugly function.. I will improve it on 2nd iteration
 */
socket_map_t 
AllocateSocket(mctx_t mctx, int socktype)
{
	mtcp_manager_t mtcp = g_mtcp[mctx->cpu];
	socket_map_t socket = NULL;

	switch (socktype) {
	case MOS_SOCK_MONITOR_STREAM:
		mtcp->num_msp++;
	case MOS_SOCK_MONITOR_STREAM_ACTIVE:
	case MOS_SOCK_MONITOR_RAW:
		socket = TAILQ_FIRST(&mtcp->free_msmap);
		if (!socket)
			goto alloc_error;
		TAILQ_REMOVE(&mtcp->free_msmap, socket, link);
		/* if there is not invalidated events, insert the socket to the end */
		/* and find another socket in the free smap list */
		if (socket->events) {
			TRACE_INFO("There are still not invalidate events remaining.\n");
			TRACE_DBG("There are still not invalidate events remaining.\n");
			TAILQ_INSERT_TAIL(&mtcp->free_msmap, socket, link);
			socket = NULL;
			goto alloc_error;
		}
		break;

	default:
		socket = TAILQ_FIRST(&mtcp->free_smap);
		if (!socket)
			goto alloc_error;
		TAILQ_REMOVE(&mtcp->free_smap, socket, link);
		/* if there is not invalidated events, insert the socket to the end */
		/* and find another socket in the free smap list */
		if (socket->events) {
			TRACE_INFO("There are still not invalidate events remaining.\n");
			TRACE_DBG("There are still not invalidate events remaining.\n");
			TAILQ_INSERT_TAIL(&mtcp->free_smap, socket, link);
			socket = NULL;
			goto alloc_error;
		}
		socket->stream = NULL;
		/* 
		 * reset a few fields (needed for client socket) 
		 * addr = INADDR_ANY, port = INPORT_ANY
		 */
		memset(&socket->saddr, 0, sizeof(struct sockaddr_in));
		memset(&socket->ep_data, 0, sizeof(mtcp_epoll_data_t));
	}

	socket->socktype = socktype;
	socket->opts = 0;
	socket->epoll = 0;
	socket->events = 0;
	
	return socket;

 alloc_error:
	TRACE_ERROR("The concurrent sockets are at maximum.\n");
	return NULL;
}
/*---------------------------------------------------------------------------*/
void 
FreeSocket(mctx_t mctx, int sockid, int socktype)
{
	mtcp_manager_t mtcp = g_mtcp[mctx->cpu];
	socket_map_t socket = NULL;

	switch (socktype) {
	case MOS_SOCK_UNUSED:
		return;
	case MOS_SOCK_MONITOR_STREAM_ACTIVE:
		socket = &mtcp->msmap[sockid];
		FreeMonStream(mtcp, socket);
		TAILQ_INSERT_TAIL(&mtcp->free_msmap, socket, link);
		break;
	case MOS_SOCK_MONITOR_STREAM:
		mtcp->num_msp--;
	case MOS_SOCK_MONITOR_RAW:
		socket = &mtcp->msmap[sockid];
		FreeMonListener(mtcp, socket);
		TAILQ_INSERT_TAIL(&mtcp->free_msmap, socket, link);
		break;
	default: /* MOS_SOCK_STREAM_* */
		socket = &mtcp->smap[sockid];
		TAILQ_INSERT_TAIL(&mtcp->free_smap, socket, link);
		/* insert into free stream map */
		mtcp->smap[sockid].stream = NULL;
		break;
	}

	socket->socktype = MOS_SOCK_UNUSED;
	socket->epoll = MOS_EPOLLNONE;
	socket->events = 0;
}
/*---------------------------------------------------------------------------*/
socket_map_t 
GetSocket(mctx_t mctx, int sockid)
{
	if (sockid < 0 || sockid >= g_config.mos->max_concurrency) {
		errno = EBADF;
		return NULL;
	}

	return &g_mtcp[mctx->cpu]->smap[sockid];
}
/*---------------------------------------------------------------------------*/
