#include <assert.h>
#include <ctype.h>
#include <string.h>
#ifdef ENABLE_DEBUG_EVENT
#include <stdarg.h>
#endif

#include "mtcp.h"
#include "mos_api.h"
#include "debug.h"
#include "config.h"
#include "ip_in.h"
#include "tcp_out.h"
/*----------------------------------------------------------------------------*/
#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#define SKIP_SPACES(x) while (*x && isspace((int)*x)) x++;
#define SKIP_CHAR(x) while((*x) && !isspace(*x)) x++;

#define KW_AND       "and "
#define KW_OR        "or "
#define KW_NOT       "not "
#define KW_TCP       "tcp"
#define KW_NOT_TCP   "!tcp"
#define KW_NOT_TCP2  "not tcp"
#define KW_SRC       "src "
#define KW_DST       "dst "
#define KW_HOST      "host "
#define KW_NET       "net "
#define KW_MASK      "mask "
#define KW_PORT      "port "
#define KW_PORTRANGE "portrange "
/*----------------------------------------------------------------------------*/
int
IsValidFlowRule(char *cf)
{
	char *word;
	int skip_word = 0;

	/* '!tcp' or 'not tcp' are also not supported in TCP flow filter */
	if (strstr(cf, KW_NOT_TCP) || strstr(cf, KW_NOT_TCP2)) {
		TRACE_ERROR("'!tcp' or 'not tcp' is not a valid rule for TCP flow monitor.\n");
		return FALSE;
	}

	/* verify that the rule contains flow-related keywords only */
	word = cf;
	SKIP_SPACES(word);

	/* while (browse the rule by words) */
	while (*word) {
		if (skip_word) {
			skip_word = 0;
			SKIP_CHAR(word);
			SKIP_SPACES(word);
			continue;
		}
		/* parse the keyword */
		/* case "tcp" "src" "dst" "not' "and" "or" -> move to the next word */
		if (!strncmp(word, KW_TCP, sizeof(KW_TCP) - 1) ||
			!strncmp(word, KW_SRC, sizeof(KW_SRC) - 1) ||
			!strncmp(word, KW_DST, sizeof(KW_DST) - 1) ||
			!strncmp(word, KW_NOT, sizeof(KW_NOT) - 1) ||
			!strncmp(word, KW_AND, sizeof(KW_AND) - 1) ||
			!strncmp(word, KW_OR, sizeof(KW_OR) - 1)) {
			skip_word = 0;
		}
		/* case "net" "mask" "port" "portrange" -> skip a word (= param) */
		else if (!strncmp(word, KW_HOST, sizeof(KW_HOST) - 1) ||
				 !strncmp(word, KW_NET, sizeof(KW_NET) - 1) ||
				 !strncmp(word, KW_MASK, sizeof(KW_MASK) - 1) ||
				 !strncmp(word, KW_PORT, sizeof(KW_PORT) - 1) ||
				 !strncmp(word, KW_PORTRANGE, sizeof(KW_PORTRANGE) - 1)) {
			skip_word = 1;
		}
		/* default (rule has any invalid keyword) -> return error */
		else {
			TRACE_ERROR("Invalid keyword in filter (%s)\n", word);
			return FALSE;
		}

		SKIP_CHAR(word);			
		SKIP_SPACES(word);
	}	

	return TRUE;
}
/*----------------------------------------------------------------------------*/
/* Assign an address range (specified by ft) to monitor via sock */
int
mtcp_bind_monitor_filter(mctx_t mctx, int sockid, monitor_filter_t ft) 
{
	socket_map_t sock;
	mtcp_manager_t mtcp;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		errno = EACCES;
		return -1;
	}

	/* if filter is not set, do nothing and return */
	if (ft == NULL) {
		TRACE_ERROR("filter not set!\n");
		return 0;
	}

	/* retrieve the socket */
	if (sockid < 0 || sockid >= g_config.mos->max_concurrency) {
		errno = EBADF;
		TRACE_ERROR("sockid is invalid!\n");
		return -1;
	}
	sock = &mtcp->msmap[sockid];

	/* check socket type */
	switch (sock->socktype) {
	case MOS_SOCK_MONITOR_RAW:
		/* For MONITOR_RAW type, allow any bpf rule */
		if (!ft->raw_pkt_filter) {
			TRACE_ERROR("raw pkt filter is null");
			return 0;
		}
		if (SET_BPFFILTER(&sock->monitor_listener->raw_pkt_fcode,
						  ft->raw_pkt_filter) < 0) {
			TRACE_ERROR("Invalid filter expression!\n");
			errno = EINVAL;
			return -1;
		}
		break;
	case MOS_SOCK_MONITOR_STREAM:
		/* For MONITOR_STREAM_PASSIVE type, restrict to flow-level keywords */
		if (ft->stream_syn_filter) {
			if (!IsValidFlowRule(ft->stream_syn_filter)) {
				errno = EINVAL;
				return -1;		
			}
			if (SET_BPFFILTER(&sock->monitor_listener->stream_syn_fcode,
							  ft->stream_syn_filter) < 0) {
				TRACE_ERROR("Invalid filter expression!\n");
				errno = EINVAL;
				return -1;
			}
		}
		if (ft->stream_orphan_filter) {
			if (!IsValidFlowRule(ft->stream_orphan_filter)) {
				errno = EINVAL;
				return -1;
			}
			if (SET_BPFFILTER(&sock->monitor_listener->stream_orphan_fcode,
							  ft->stream_orphan_filter) < 0) {
				TRACE_ERROR("Invalid filter expression!\n");
				errno = EINVAL;
				return -1;
			}
		}
		break;
	default:
		/* return error for other socket types */
		errno = ENOPROTOOPT;
		TRACE_ERROR("Invalid sock type!\n");
		return -1;
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
void
mtcp_app_join(mctx_t mctx) 
{
	mtcp_manager_t mtcp = GetMTCPManager(mctx);
	if (!mtcp) return;
	
	RunPassiveLoop(mtcp);
	return;
}
/*----------------------------------------------------------------------------*/
/* Callback only functions */
/*----------------------------------------------------------------------------*/
void 
mtcp_set_uctx(mctx_t mctx, int msock, void *uctx) 
{
	mtcp_manager_t mtcp;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		return;
	}

	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self())
		return;

	if (msock < 0 || msock >= g_config.mos->max_concurrency) {
		TRACE_API("Socket id %d out of range.\n", msock);
		errno = EBADF;
		return;
	}

	socket_map_t socket = &mtcp->msmap[msock];
	if (socket->socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE)
		socket->monitor_stream->uctx = uctx;
	else if (socket->socktype == MOS_SOCK_MONITOR_STREAM ||
			 socket->socktype == MOS_SOCK_MONITOR_RAW)
		socket->monitor_listener->uctx = uctx;
}
/*----------------------------------------------------------------------------*/
void * 
mtcp_get_uctx(mctx_t mctx, int msock)
{
	mtcp_manager_t mtcp;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		errno = EACCES;
		return NULL;
	}

	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		errno = EPERM;
		return NULL;
	}

	if (msock < 0 || msock >= g_config.mos->max_concurrency) {
		TRACE_API("Socket id %d out of range.\n", msock);
		errno = EBADF;
		return NULL;
	}

	socket_map_t socket = &mtcp->msmap[msock];
	if (socket->socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE)
		return socket->monitor_stream->uctx;
	else if (socket->socktype == MOS_SOCK_MONITOR_STREAM ||
			 socket->socktype == MOS_SOCK_MONITOR_RAW)
		return socket->monitor_listener->uctx;
	else
		return NULL;
}
/*----------------------------------------------------------------------------*/
ssize_t 
mtcp_peek(mctx_t mctx, int msock, int side, char *buf, size_t len) 
{
	int copylen, rc;
	struct tcp_stream *cur_stream;
	mtcp_manager_t mtcp;
	socket_map_t sock;
	
	copylen = rc = 0;
	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		errno = EACCES;
		return -1;
	}
	
	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		errno = EPERM;
		return -1;
	}

	/* check if the socket is monitor stream */
	sock = &mtcp->msmap[msock];
	if (sock->socktype != MOS_SOCK_MONITOR_STREAM_ACTIVE) {
		TRACE_DBG("Invalid socket type!\n");
		errno = EBADF;
		return -1;
	}

	if (side != MOS_SIDE_CLI && side != MOS_SIDE_SVR) {
		TRACE_ERROR("Invalid side requested!\n");
		exit(EXIT_FAILURE);
		return -1;
	}

	struct tcp_stream *mstrm = sock->monitor_stream->stream;
	cur_stream = (side == mstrm->side) ? mstrm : mstrm->pair_stream;

	if (!cur_stream || !cur_stream->buffer_mgmt) {
		TRACE_DBG("Stream is NULL!! or buffer management is disabled\n");
		errno = EINVAL;
		return -1;
	}

	/* Check if the read was not just due to syn-ack recv */
	if (cur_stream->rcvvar != NULL && 
	    cur_stream->rcvvar->rcvbuf != NULL) {
		tcprb_t *rcvbuf = cur_stream->rcvvar->rcvbuf;
		loff_t *poff = &sock->monitor_stream->peek_offset[cur_stream->side];

		rc = tcprb_ppeek(rcvbuf, (uint8_t *)buf, len, *poff);
		if (rc < 0) {
			errno = ENODATA;
			return -1;
		}

		*poff += rc;
		UNUSED(copylen);

		return rc;
	} else {
		TRACE_DBG("Stream hasn't yet been initialized!\n");
		rc = 0;
	}
	
	return rc;
}
/*----------------------------------------------------------------------------*/
/**
 * Copies from the frags.. returns no. of bytes copied to buf
 */
static inline int
ExtractPayloadFromFrags(struct tcp_ring_buffer *rcvbuf, char *buf,
						size_t count, off_t seq_num)
{
	int cpbytesleft;
	struct fragment_ctx *it;
	
	it = rcvbuf->fctx;
	cpbytesleft = count;
	/* go through each frag */
	while (it) {
		/* first check whether sequent number matches */
		if (TCP_SEQ_BETWEEN(seq_num, it->seq, it->seq + it->len)) {
			/* copy buf starting from seq# seq_num */
			/* copy the MIN of seq-range and bytes to be copied */
			memcpy(buf + count - cpbytesleft, 
			       rcvbuf->head + seq_num - rcvbuf->head_seq, 
			       MIN(it->len - (seq_num - it->seq), cpbytesleft));
			/* update target seq num */
			seq_num += it->len - (seq_num - it->seq);
			/* update cpbytes left */
			cpbytesleft -= it->len - (seq_num - it->seq);
			if (cpbytesleft == 0)
				break;
		}
		it = it->next;
	}
	
	count -= cpbytesleft;

	/* return number of bytes copied */
	return count;
}
/*----------------------------------------------------------------------------*/
/* Please see in-code comments for description */
ssize_t
mtcp_ppeek(mctx_t mctx, int msock, int side, 
			  char *buf, size_t count, uint64_t off)
{
	mtcp_manager_t mtcp;
	struct tcp_stream *cur_stream;
	int rc;
	socket_map_t sock;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		errno = EACCES;
		goto ppeek_error;
	}

	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		errno = EPERM;
		goto ppeek_error;
	}

	/* check if the socket is monitor stream */
	sock = &mtcp->msmap[msock];
	if (sock->socktype != MOS_SOCK_MONITOR_STREAM_ACTIVE) {
		TRACE_DBG("Invalid socket type!\n");
		errno = ESOCKTNOSUPPORT;
		goto ppeek_error;
	}

	if (side != MOS_SIDE_CLI && side != MOS_SIDE_SVR) {
		TRACE_ERROR("Invalid side requested!\n");
		exit(EXIT_FAILURE);
		return -1;
	}

	struct tcp_stream *mstrm = sock->monitor_stream->stream;
	cur_stream = (side == mstrm->side) ? mstrm : mstrm->pair_stream;

	if (!cur_stream || !cur_stream->buffer_mgmt) {
		TRACE_DBG("Stream is either NULL or ring buffer is not managed!!\n");
		errno = EACCES;
		goto ppeek_error;
	}

	rc = 0;
	/* Check if the read was not just due to syn-ack recv */
	if (cur_stream->rcvvar != NULL && 
	    cur_stream->rcvvar->rcvbuf != NULL) {
		tcprb_t *rcvbuf = cur_stream->rcvvar->rcvbuf;
		return tcprb_ppeek(rcvbuf, (uint8_t *)buf, count, off);
	} else {
		errno = EPERM;
		goto ppeek_error;
	}

	return rc;

 ppeek_error:
	return -1;
}
/*----------------------------------------------------------------------------*/
#ifdef MTCP_CB_GETCURPKT_CREATE_COPY
static __thread unsigned char local_frame[ETHERNET_FRAME_LEN];
inline struct pkt_info *
ClonePacketCtx(struct pkt_info *to, unsigned char *frame, struct pkt_info *from)
{
	/* memcpy the entire ethernet frame */
	assert(from);
	assert(from->eth_len > 0);
	assert(from->eth_len <= ETHERNET_FRAME_LEN);
	memcpy(frame, from->ethh, from->eth_len);

	/* only memcpy till the last field before ethh */
	/* memcpy(to, from, PCTX_COPY_LEN); */
	memcpy(to, from, PKT_INFO_LEN);
	/* set iph */
	to->ethh = (struct ethhdr *)frame;
	/* set iph */
	to->iph = from->iph ?
		(struct iphdr *)((uint8_t *)(frame + ETHERNET_HEADER_LEN)) : NULL;
	if (to->iph) {
		/* set tcph */
		to->tcph = from->tcph ?
			(struct tcphdr *)(((uint8_t *)(to->iph)) + (to->iph->ihl<<2)) : NULL;
		if (to->tcph)
			/* set payload */
			to->payload = from->tcph ?
				((uint8_t *)(to->tcph) + (to->tcph->doff<<2)) : NULL;
	}
	return to;
}
/*----------------------------------------------------------------------------*/
int
mtcp_getlastpkt(mctx_t mctx, int sock, int side, struct pkt_info *pkt)
{
	mtcp_manager_t mtcp;
	socket_map_t socket;
	struct pkt_ctx *cur_pkt_ctx;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		errno = EACCES;
		return -1;
	}

	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		errno = EPERM;
		return -1;
	}
	
	/* check if the socket is monitor stream */
	socket = &mtcp->msmap[sock];
#ifndef RECORDPKT_PER_STREAM 
	switch (socket->socktype) {
	case MOS_SOCK_MONITOR_STREAM_ACTIVE:
	case MOS_SOCK_MONITOR_RAW:
	case MOS_SOCK_MONITOR_STREAM:
		if (mtcp->pctx == NULL) {
			errno = EACCES;
			return -1;
		}
		cur_pkt_ctx = mtcp->pctx;
		break;
	default:
		TRACE_DBG("Invalid socket type!\n");
		errno = EBADF;
		return -1;		
	}
#else /* RECORDPKT_PER_STREAM */
	struct tcp_stream *cur_stream;
	if (socket->socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE) {
		if (side != MOS_SIDE_CLI && side != MOS_SIDE_SVR) {
			TRACE_ERROR("Invalid side requested!\n");
			exit(EXIT_FAILURE);
			return -1;
		}

		struct tcp_stream *mstrm = socket->monitor_stream->stream;
		cur_stream = (side == mstrm->side) ? mstrm : mstrm->pair_stream;

		cur_pkt_ctx = &cur_stream->last_pctx;
		if (!cur_pkt_ctx->p.ethh) {
			errno = ENODATA;
			return -1;
		}
	} else if (socket->socktype == MOS_SOCK_MONITOR_RAW) {
		cur_pkt_ctx = mtcp->pctx;
	} else if (socket->socktype == MOS_SOCK_MONITOR_STREAM) {
		/* 
		 * if it is a monitor socket, then this means that
		 * this is a request for an orphan tcp packet
		 */
		cur_pkt_ctx = mtcp->pctx;
	} else {
		TRACE_DBG("Invalid socket type!\n");
		errno = EBADF;
		return -1;
	}
#endif /* !RECORDPKT_PER_STREAM */ 
	ClonePacketCtx(pkt, local_frame, &(cur_pkt_ctx->p));
	return 0;
}
#else
/*----------------------------------------------------------------------------*/
int
mtcp_getlastpkt(mctx_t mctx, int sock, int side, struct pkt_ctx **pctx)
{
	mtcp_manager_t mtcp;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		errno = EACCES;
		return -1;
	}

	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		errno = EPERM;
		return -1;
	}
	/* just pass direct pointer */
	*pctx = mtcp->pctx;

	return 0;
}
#endif
/*----------------------------------------------------------------------------*/
int
mtcp_sendpkt(mctx_t mctx, int sock, const struct pkt_info *pkt)
{
	mtcp_manager_t mtcp;
	socket_map_t socket;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp || !pkt) {
		errno = EACCES;
		return -1;
	}

	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		errno = EPERM;
		return -1;
	}

	/* check if the socket is monitor stream */
	socket = &mtcp->msmap[sock];

	if (!(pkt->iph) || !(pkt->tcph)) {
		errno = ENODATA;
		TRACE_INFO("mtcp_sendpkt() only supports TCP packet for now.\n");
		return -1;
	}

	if (socket->socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE) {
		SendTCPPacketStandalone(mtcp,
				   pkt->iph->saddr, pkt->tcph->source,
				   pkt->iph->daddr, pkt->tcph->dest,
				   htonl(pkt->tcph->seq), htonl(pkt->tcph->ack_seq),
				   ntohs(pkt->tcph->window), TCP_FLAG_ACK,
				   pkt->payload, pkt->payloadlen,
				   socket->monitor_stream->stream->rcvvar->ts_recent,
				   socket->monitor_stream->stream->rcvvar->ts_lastack_rcvd,
				   pkt->iph->id, pkt->in_ifidx);


	}

	return 0;
}
/*----------------------------------------------------------------------------*/
/** Disable events from the monitor stream socket
 * @param [in] mtcp: mtcp_manager
 * @param [in] sock: socket
 *
 * returns 0 on success, -1 on failure
 *
 * This is used for flow management based monitoring sockets
 */
int
RemoveMonitorEvents(mtcp_manager_t mtcp, socket_map_t socket, int side)
{
	struct mon_stream *mstream;
	struct mon_listener *mlistener;

	if (mtcp == NULL) {
		TRACE_DBG("mtcp is not defined!!!\n");
		errno = EACCES;
		return -1;
	}
	
	switch (socket->socktype) {
	case MOS_SOCK_MONITOR_STREAM_ACTIVE:
		mstream = socket->monitor_stream;
		if (mstream == NULL) {
			TRACE_ERROR("Mon Stream does not exist!\n");
			/* exit(-1); */
			errno = ENODATA;
			return -1;
		}

		if (side == MOS_SIDE_SVR) mstream->server_mon = 0;
		else if (side == MOS_SIDE_CLI) mstream->client_mon = 0;

		if (mstream->server_mon == 0 && mstream->client_mon == 0) {
#ifdef NEWEV
			/* 
			 * if stree_dontcare is NULL, then we know that all 
			 * events have already been disabled
			 */
			if (mstream->stree_pre_rcv != NULL) {
				stree_dec_ref(mtcp->ev_store, mstream->stree_dontcare);
				stree_dec_ref(mtcp->ev_store, mstream->stree_pre_rcv);
				stree_dec_ref(mtcp->ev_store, mstream->stree_post_snd);
				
				mstream->stree_dontcare = NULL;
				mstream->stree_pre_rcv = NULL;
				mstream->stree_post_snd = NULL;
			}
#else
			/* no error checking over here.. 
			 * but its okay.. this code is 
			 * deprecated 
			 */
			CleanupEvP(&mstream->dontcare_evp);
			CleanupEvP(&mstream->pre_tcp_evp);
			CleanupEvP(&mstream->post_tcp_evp);
#endif
		}
		break;
	case MOS_SOCK_MONITOR_STREAM:
		mlistener = socket->monitor_listener;
		if (mlistener == NULL) {
			TRACE_ERROR("Mon listener does not exist!\n");
			errno = ENODATA;
			return -1;
		}

		if (side == MOS_SIDE_SVR) mlistener->server_mon = 0;
		else if (side == MOS_SIDE_CLI) mlistener->client_mon = 0;
		
		if (mlistener->server_mon == 0 && mlistener->client_mon == 0) {
#ifdef NEWEV
			/* 
			 * if stree_dontcare is NULL, then we know that all 
			 * events have already been disabled
			 */
			if (mlistener->stree_pre_rcv != NULL) {
				stree_dec_ref(mtcp->ev_store, mlistener->stree_dontcare);
				stree_dec_ref(mtcp->ev_store, mlistener->stree_pre_rcv);
				stree_dec_ref(mtcp->ev_store, mlistener->stree_post_snd);
				
				mlistener->stree_dontcare = NULL;
				mlistener->stree_pre_rcv = NULL;
				mlistener->stree_post_snd = NULL;
			}
#else
			/* no error checking over here.. 
			 * but its okay.. this code is 
			 * deprecated 
			 */
			CleanupEvB(mtcp, &mlistener->dontcare_evb);
			CleanupEvB(mtcp, &mlistener->pre_tcp_evb);
			CleanupEvB(mtcp, &mlistener->post_tcp_evb);
#endif
		}
		break;
	default:
		TRACE_ERROR("Invalid socket type!\n");
	}
	
	return 0;
}
/*----------------------------------------------------------------------------*/
/**
 * Disable monitoring based on side variable.
 */
int
mtcp_cb_stop(mctx_t mctx, int sock, int side)
{
	mtcp_manager_t mtcp;
	socket_map_t socket;
	struct tcp_stream *stream;
	struct socket_map *walk;
	uint8_t mgmt;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		errno = EACCES;
		return -1;
	}

	socket = &mtcp->msmap[sock];

	/* works for both monitor listener and stream sockets */
	RemoveMonitorEvents(mtcp, socket, side);

	/* passive monitoring socket is not connected to any stream */
	if (socket->socktype == MOS_SOCK_MONITOR_STREAM) {
	  /* it should return an EPERM error instead of quitting silently */
	  errno = EPERM;
	  return -1;
	}
	
	if (side == MOS_SIDE_CLI) {
		/* see if the associated stream requires monitoring any more */
		stream = (socket->monitor_stream->stream->side == MOS_SIDE_CLI) ? 
			socket->monitor_stream->stream :
			socket->monitor_stream->stream->pair_stream;
		
		mgmt = 0;
		SOCKQ_FOREACH_START(walk, &stream->msocks) {
			if (walk->monitor_stream->client_mon == 1) {
				mgmt = 1;
				break;
			}
		} SOCKQ_FOREACH_END;
		/* if all streams have mgmt off, then tag the stream for destruction */
		if (mgmt == 0) {
			stream = (socket->monitor_stream->stream->side == MOS_SIDE_CLI) ? 
				socket->monitor_stream->stream :
				socket->monitor_stream->stream->pair_stream;
			stream->status_mgmt = 0;
		}
	}
	
	if (side == MOS_SIDE_SVR) {
		/* see if the associated stream requires monitoring any more */
		stream = (socket->monitor_stream->stream->side == MOS_SIDE_SVR) ? 
			socket->monitor_stream->stream :
			socket->monitor_stream->stream->pair_stream;
		mgmt = 0;
		SOCKQ_FOREACH_START(walk, &stream->msocks) {
			if (walk->monitor_stream->server_mon == 1) {
				mgmt = 1;
				break;
			}
		} SOCKQ_FOREACH_END;
		/* if all streams have mgmt off, then tag the stream for destruction */
		if (mgmt == 0) {
			stream = (socket->monitor_stream->stream->side == MOS_SIDE_SVR) ? 
				socket->monitor_stream->stream :
				socket->monitor_stream->stream->pair_stream;
			stream->status_mgmt = 0;
		}
	}
	
	return 0;
}
/*----------------------------------------------------------------------------*/
/**
 * send a RST packet to the TCP stream (uni-directional)
 */
static inline void
SendRSTPacketStandalone(mtcp_manager_t mtcp, struct tcp_stream *stream) {
	SendTCPPacketStandalone(mtcp,
				stream->saddr, stream->sport, stream->daddr, stream->dport,
				stream->snd_nxt, stream->rcv_nxt, 0, TCP_FLAG_RST | TCP_FLAG_ACK,
				NULL, 0, mtcp->cur_ts, 0, 0, -1);
}
/*----------------------------------------------------------------------------*/
/**
 * Reset the connection (send RST packets to both sides)
 */
int
mtcp_reset_conn(mctx_t mctx, int sock)
{
	mtcp_manager_t mtcp;
	socket_map_t socket;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		errno = EACCES;
		return -1;
	}

	socket = &mtcp->msmap[sock];

	/* passive monitoring socket is not connected to any stream */
	if (socket->socktype == MOS_SOCK_MONITOR_STREAM) {
		errno = EINVAL;
		return -1;
	}

	/* send RST packets to the both sides */
	SendRSTPacketStandalone(mtcp, socket->monitor_stream->stream);
	SendRSTPacketStandalone(mtcp, socket->monitor_stream->stream->pair_stream);
	
	return 0;
}
/*----------------------------------------------------------------------------*/
uint32_t
mtcp_cb_get_ts(mctx_t mctx)
{
	mtcp_manager_t mtcp;
	
	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		TRACE_DBG("Can't access MTCP manager!\n");
		errno = EACCES;
		return 0;
	}

	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		errno = EPERM;
		return 0;
	}
	
	return TS_TO_USEC(mtcp->cur_ts);
}
/*----------------------------------------------------------------------------*/
/* Macros related to getpeername */
#define TILL_SVRADDR		offsetof(struct sockaddr_in, sin_zero)
#define TILL_SVRPORT		offsetof(struct sockaddr_in, sin_addr)
#define TILL_SVRFAMILY		offsetof(struct sockaddr_in, sin_port)
#define TILL_CLIADDR		sizeof(struct sockaddr) + TILL_SVRADDR
#define TILL_CLIPORT		sizeof(struct sockaddr) + TILL_SVRPORT
#define TILL_CLIFAMILY		sizeof(struct sockaddr) + TILL_SVRFAMILY

int
mtcp_getpeername(mctx_t mctx, int sockfd, struct sockaddr *saddr,
				 socklen_t *addrlen, int side)
{
	mtcp_manager_t mtcp;
	socket_map_t socket;
	struct tcp_stream *stream;
	struct sockaddr_in *sin;
	int rc;

	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		TRACE_DBG("Can't access MTCP manager!\n");
		errno = EACCES;
		return -1;
	}

	/* check if sockfd is within limits */
	if (sockfd < 0 || sockfd >= g_config.mos->max_concurrency) {
		TRACE_API("Socket id %d out of range.\n", sockfd);
		errno = EBADF;
		return -1;
	}

	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		errno = EPERM;
		return -1;
	}

	socket = &mtcp->msmap[sockfd];
	sin = (struct sockaddr_in *)saddr;
	rc = 0;

	/* retrieve both streams */
	stream = socket->monitor_stream->stream;

	if (side != stream->side)
		stream = stream->pair_stream;

	if (stream == NULL) {
		errno = ENOTCONN;
		return -1;
	}

	/* reset to 2 * sizeof(struct sockaddr) if addrlen is too big */
	if (*addrlen > 2 * sizeof(struct sockaddr))
		*addrlen = 2 * sizeof(struct sockaddr);

	/* according per manpage, address can be truncated */
	switch (*addrlen) {
	case (2 * sizeof(struct sockaddr)):
	case TILL_CLIADDR:
		sin[1].sin_addr.s_addr = stream->side == MOS_SIDE_SVR ?
								 stream->daddr : stream->saddr;
	case TILL_CLIPORT:
		sin[1].sin_port = stream->side == MOS_SIDE_SVR ?
						  stream->dport : stream->sport;
	case TILL_CLIFAMILY:
		sin[1].sin_family = AF_INET;
	case (sizeof(struct sockaddr)):
	case TILL_SVRADDR:
		sin->sin_addr.s_addr = stream->side == MOS_SIDE_SVR ?
							   stream->saddr : stream->daddr;
	case TILL_SVRPORT:
		sin->sin_port = stream->side == MOS_SIDE_SVR ?
						stream->sport : stream->dport;
	case TILL_SVRFAMILY:
		sin->sin_family = AF_INET;
		break;
	default:
		rc = -1;
		*addrlen = 0xFFFF;
		errno = EINVAL;
	}

	return rc;
}
/*----------------------------------------------------------------------------*/
int
mtcp_setlastpkt(mctx_t mctx, int sock, int side, off_t offset,
		byte *data, uint16_t datalen, int option)
{
	mtcp_manager_t mtcp;
	struct pkt_ctx *cur_pkt_ctx;
	struct ethhdr *ethh;
	struct iphdr *iph;
	struct tcphdr *tcph;
	unsigned char *payload;

#if 0
	socket_map_t socket;
	struct tcp_stream *cur_stream;
#endif

	/* checking if mtcp is valid */
	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		errno = EACCES;
		TRACE_ERROR("Invalid mtcp!\n");
		return -1;
	}

	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		errno = EPERM;
		TRACE_ERROR("Invalid thread id!\n");
		return -1;
	}

#if 0	
	/* check if the socket is monitor stream */
	socket = &mtcp->msmap[sock];
	if (socket->socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE) {
		if (side != MOS_SIDE_CLI && side != MOS_SIDE_SVR) {
			TRACE_ERROR("Invalid side requested!\n");
			exit(EXIT_FAILURE);
			return -1;
		}

		struct tcp_stream *mstrm = socket->monitor_stream->stream;
		cur_stream = (side == mstrm->side) ? mstrm : mstrm->pair_stream;

		if (!cur_stream->allow_pkt_modification)
			return -1;
	} else if (socket->socktype != MOS_SOCK_MONITOR_RAW) {
		TRACE_ERROR("Invalid socket type!\n");
		exit(EXIT_FAILURE);
		return -1;
	}
#endif

	/* see if cur_pkt_ctx is valid */
	cur_pkt_ctx = mtcp->pctx;
	if (cur_pkt_ctx == NULL) {
		TRACE_ERROR("pctx is NULL!\n");
		errno = ENODATA;
		return -1;
	}

	/* check if offset is valid */
	if (offset < 0) {
		TRACE_ERROR("Invalid offset position!\n");
		errno = EINVAL;
		return -1;
	}

	if (__builtin_popcount(option & (MOS_DROP | MOS_CHOMP | 
					 MOS_INSERT | MOS_OVERWRITE)) != 1) {
		TRACE_ERROR("mtcp_setlastpkt() function only allows one of "
			    "(MOS_DROP | MOS_CHOMP | MOS_INSERT | MOS_OVERWRITE) "
			    "to be set at a time.\n");
		errno = EAGAIN;
		return -1;
	}

	/* drop pkt has the highest priority */
	if (option & MOS_DROP) {
		mtcp->pctx->forward = 0;
		return 0;
	} else if (option & MOS_ETH_HDR) {
		/* validity test */
		if ((ethh=cur_pkt_ctx->p.ethh) == NULL ||
		    offset + datalen > sizeof(struct ethhdr)) {
			TRACE_ERROR("Ethernet setting has gone out of bounds "
				    "(offset: %ld, datalen: %d)\n",
				    offset, datalen);
			errno = EINVAL;
			return -1;
		}
		if (option & MOS_CHOMP) {
			TRACE_ERROR("Illegal call. "
				    "Ethernet header can't be chopped down!\n");
			errno = EACCES;
			return -1;
		} else if (option & MOS_INSERT) {
			TRACE_ERROR("Illegal call. "
				    "Ethernet header can't be extended!\n");
			errno = EACCES;
			return -1;
		} else /* if (option & MOS_OVERWRITE) */ {
			memcpy((uint8_t *)ethh + offset, data, datalen);
		}
		/* iph, tcph, and payload do not need to change */
	} else if (option & MOS_IP_HDR) {
		/* validity test */
		if (cur_pkt_ctx->p.ethh == NULL ||
		    cur_pkt_ctx->p.ethh->h_proto != ntohs(ETH_P_IP) ||
		    (iph=(struct iphdr *)(cur_pkt_ctx->p.ethh + 1)) == NULL) {
			TRACE_ERROR("ethh or iph are out of bounds\n");
			errno = EACCES;
			return -1;
		} 
		if (option & MOS_OVERWRITE) { 
			if (offset + datalen > (iph->ihl<<2)) {
				TRACE_ERROR("IP setting has gone out of bounds "
					    "(offset: %ld, datalen: %d)\n",
					    offset, datalen);
				errno = EINVAL;
				return -1;
			}
			memcpy((uint8_t *)iph + offset, data, datalen);
		}
		if (option & MOS_CHOMP) {
			memmove((uint8_t *)iph + offset,
				(uint8_t *)iph + offset + datalen, 
				cur_pkt_ctx->p.ip_len - offset - datalen);

			/* iph does not need to change */
			if (iph->protocol == IPPROTO_TCP) {
				cur_pkt_ctx->p.tcph = (struct tcphdr *)((uint8_t *)iph + (iph->ihl<<2));
				cur_pkt_ctx->p.payload = (uint8_t *)cur_pkt_ctx->p.tcph + 
					(cur_pkt_ctx->p.tcph->doff<<2);
			} else {
				/* reset tcph if iph does not have tcp proto */
				cur_pkt_ctx->p.tcph = NULL;
			}
			/* update iph total length */
			cur_pkt_ctx->p.ip_len = ntohs(iph->tot_len);
			/* update eth frame length */
			cur_pkt_ctx->p.eth_len = cur_pkt_ctx->p.ip_len + sizeof(struct ethhdr);
		} else if (option & MOS_INSERT) {
			memmove((uint8_t *)iph + offset + datalen,
				(uint8_t *)iph + offset + 1,
				cur_pkt_ctx->p.ip_len - offset);
			memcpy((uint8_t *)iph + offset,
			       data, datalen);
			
			/* iph does not need to change */
			if (iph->protocol == IPPROTO_TCP) {
				cur_pkt_ctx->p.tcph = (struct tcphdr *)((uint8_t *)iph + (iph->ihl<<2));
				cur_pkt_ctx->p.payload = (uint8_t *)cur_pkt_ctx->p.tcph + 
					(cur_pkt_ctx->p.tcph->doff<<2);
			} else {
				/* reset tcph if iph does not have tcp proto */
				cur_pkt_ctx->p.tcph = NULL;
			}
			/* update iph total length */
			cur_pkt_ctx->p.ip_len = ntohs(iph->tot_len);
			/* update eth frame length */
			cur_pkt_ctx->p.eth_len = cur_pkt_ctx->p.ip_len + sizeof(struct ethhdr);
		}
		/* can't update payloadlen because we don't know tcph->doff */
	} else if (option & MOS_TCP_HDR) {
		/* validity test */
		iph = (struct iphdr *)(cur_pkt_ctx->p.ethh + 1);
		if (iph == NULL ||
		    iph->protocol != IPPROTO_TCP ||
		    (tcph=(struct tcphdr *)((uint8_t *)iph + (iph->ihl<<2))) == NULL) {
			TRACE_ERROR("TCP setting has gone out of bounds "
				    "(offset: %ld, datalen: %d)\n",
				    offset, datalen);
			errno = EINVAL;
			return -1;
		}
		if (option & MOS_OVERWRITE) {
			if (offset + datalen > (tcph->doff<<2)) {
				TRACE_ERROR("TCP setting has gone out of bounds "
					    "(offset: %ld, datalen: %d)\n",
					    offset, datalen);
				errno = EINVAL;
				return -1;
			}
			memcpy((uint8_t *)tcph + offset, data, datalen);
			/* update tcp seq # */
			cur_pkt_ctx->p.seq = ntohl(tcph->seq);
			/* update tcp ack_seq # */
			cur_pkt_ctx->p.ack_seq = ntohl(tcph->ack_seq);
			/* update tcp window */
			cur_pkt_ctx->p.window = ntohs(tcph->window);
			
			/* 150422 dhkim TODO: seq and offset are two different form of same
			 * variable. We also need to update the offset. */
		}
		if (option & MOS_CHOMP) {
			memmove((uint8_t *)tcph + offset,
				(uint8_t *)tcph + offset + datalen, 
				cur_pkt_ctx->p.payloadlen + (tcph->doff<<2) 
				- offset - datalen);
			/* update payload ptr */
			cur_pkt_ctx->p.payload = (uint8_t *)tcph + (tcph->doff<<2);
		} else if (option & MOS_INSERT) {
			memmove((uint8_t *)tcph + offset + datalen,
				(uint8_t *)tcph + offset + 1, 
				cur_pkt_ctx->p.payloadlen + (tcph->doff<<2) 
				- offset);
			memcpy((uint8_t *)tcph + offset, data, datalen);
			/* update payload ptr */
			cur_pkt_ctx->p.payload = (uint8_t *)tcph + (tcph->doff<<2);
		}
	} else if (option & MOS_TCP_PAYLOAD) {
		iph = (struct iphdr *)(cur_pkt_ctx->p.ethh + 1);
		tcph = (struct tcphdr *)((uint8_t *)iph + (iph->ihl<<2));
		payload = (uint8_t *)tcph + (tcph->doff<<2);
		if (option & MOS_OVERWRITE) {
			if (offset + datalen > ntohs(iph->tot_len) -
			    (iph->ihl<<2) - (tcph->doff<<2)) {
				TRACE_ERROR("Payload setting has gone out of bounds "
					    "(offset: %ld, datalen: %d)\n",
					    offset, datalen);
				errno = EINVAL;
				return -1;
			}
			memcpy(payload + offset, data, datalen);
		}
		if (option & MOS_CHOMP) {
			memmove(payload + offset,
				payload + offset + datalen, 
				(cur_pkt_ctx->p.payloadlen - 
				 offset - datalen));
			/* update payload length */
			cur_pkt_ctx->p.payloadlen = cur_pkt_ctx->p.ip_len -
				(tcph->doff<<2) - (iph->ihl<<2);
		} else if (option & MOS_INSERT) {
			memmove(payload + offset + datalen,
				payload + offset + 1,
				cur_pkt_ctx->p.payloadlen - offset);
			memcpy(payload + offset, data, datalen);
			cur_pkt_ctx->p.payloadlen = cur_pkt_ctx->p.ip_len -
				(tcph->doff<<2) - (iph->ihl<<2);
		}
	} else {
		TRACE_ERROR("Invalid option!\n");
		errno = EINVAL;
		return -1;
	}

	/* update ip checksum */
	if (option & MOS_UPDATE_IP_CHKSUM) {
		iph = (struct iphdr *)(cur_pkt_ctx->p.ethh + 1);
		iph->check = 0;
		iph->check = ip_fast_csum(iph, iph->ihl);
	}
	
	/* update tcp checksum */
	if (option & MOS_UPDATE_TCP_CHKSUM) {
		iph = (struct iphdr *)(cur_pkt_ctx->p.ethh + 1);
		tcph = (struct tcphdr *)((uint8_t *)iph + (iph->ihl<<2));
		tcph->check = 0;
		tcph->check = TCPCalcChecksum((uint16_t *)tcph,
					      ntohs(iph->tot_len) - (iph->ihl<<2),
					      iph->saddr, iph->daddr);
	}
	return 0;
}
/*----------------------------------------------------------------------------*/
#if 0
inline int
mtcp_cb_updatecurpkt(mctx_t mctx, off_t offset, unsigned char *data,
		     uint16_t datalen, int option)
{
	return mtcp_setlastpkt(mctx, sock, side, offset, data, datalen, option);
}
#endif
/*----------------------------------------------------------------------------*/
/**
 * THIS IS A DEPRECETED FUNCTION...
 */
int
mtcp_cb_dropcurpkt(mctx_t mctx)
{
	mtcp_manager_t mtcp;
	
	/* checking if mtcp is valid */
	mtcp = GetMTCPManager(mctx);
	if (!mtcp) {
		TRACE_ERROR("Invalid mtcp!\n");
		errno = EACCES;
		return -1;
	}
	
	/* check if the calling thread is in MOS context */
	if (mtcp->ctx->thread != pthread_self()) {
		TRACE_ERROR("Invalid thread id!\n");
		errno = EPERM;
		return -1;
	}
	
	/* see if cur_pkt_ctx is valid */
	if (mtcp->pctx == NULL) {
		TRACE_ERROR("pctx is NULL!\n");
		errno = ENODATA;
		return -1;
	}
	
	mtcp->pctx->forward = 0;
	
	return 0;
}
/*----------------------------------------------------------------------------*/
int
mtcp_set_debug_string(mtcp_manager_t mtcp, const char *fmt, ...)
{
#ifdef ENABLE_DEBUG_EVENT
	va_list args;
	int i;

	assert(mtcp);

	if (fmt == NULL) {
		mtcp->dbg_buf[0] = '\0';
		return 0;
	}

	va_start(args, fmt);
	i = vsnprintf(mtcp->dbg_buf, DBG_BUF_LEN - 1, fmt, args);
	va_end(args);

	return i;
#else
	return -1;
#endif /* ENABLE_DEBUG_EVENT */
}
/*----------------------------------------------------------------------------*/
int
mtcp_get_debug_string(mctx_t mctx, char *buf, int len)
{
#ifdef ENABLE_DEBUG_EVENT
	mtcp_manager_t mtcp;
	int copylen;

	if (len < 0)
		return -1;
	else if (len == 0)
		return 0;

	if (!(mtcp = GetMTCPManager(mctx)))
		return -1;

	copylen = MIN(strlen(mtcp->dbg_buf), len);
	strncpy(buf, mtcp->dbg_buf, copylen);

	return copylen;
#else
	return -1;
#endif /* ENABLE_DEBUG_EVENT */
}
/*----------------------------------------------------------------------------*/
