#include <assert.h>
#include <string.h>

#include "mtcp.h"
#include "arp.h"
#include "socket.h"
#include "eth_out.h"
#include "ip_out.h"
#include "mos_api.h"
#include "tcp_util.h"
#include "tcp_in.h"
#include "tcp_out.h"
#include "tcp_ring_buffer.h"
#include "eventpoll.h"
#include "debug.h"
#include "timer.h"
#include "ip_in.h"
#include "config.h"

extern struct pkt_info *
ClonePacketCtx(struct pkt_info *to, unsigned char *frame, struct pkt_info *from);

#define VERIFY_RX_CHECKSUM	TRUE
/*----------------------------------------------------------------------------*/
static inline uint32_t
DetectStreamType(mtcp_manager_t mtcp, struct pkt_ctx *pctx,
				 uint32_t ip, uint16_t port)
{
	/* To Do: We will extend this filter to check listeners for proxy as well */
	struct sockaddr_in *addr;
	int rc, cnt_match, socktype;
	struct mon_listener *walk;
	struct sfbpf_program fcode;

	cnt_match = 0;
	rc = 0;

	if (mtcp->num_msp > 0) {
		/* mtcp_bind_monitor_filter()
		 * - create MonitorTCPStream only when the filter of any of the existing
		 *   passive sockets match the incoming flow */
		TAILQ_FOREACH(walk, &mtcp->monitors, link) {
			/* For every passive monitor sockets, */
			socktype = walk->socket->socktype;
			if (socktype != MOS_SOCK_MONITOR_STREAM)
				continue; // XXX: can this happen??

			/* if pctx hits the filter rule, handle the passive monitor socket */
			fcode = walk->stream_syn_fcode;
			if (!(ISSET_BPFFILTER(fcode) && pctx &&
				EVAL_BPFFILTER(fcode, (uint8_t *)pctx->p.iph - sizeof(struct ethhdr),
							   pctx->p.ip_len + sizeof(struct ethhdr)) == 0)) {
				walk->is_stream_syn_filter_hit = 1;// set the 'filter hit' flag to 1
				cnt_match++; // count the number of matched sockets
			}
		}

		/* if there's any passive monitoring socket whose filter is hit,
		   we should create monitor stream */
		if (cnt_match > 0)
			rc = STREAM_TYPE(MOS_SOCK_MONITOR_STREAM_ACTIVE);
	}
	
	if (mtcp->listener) {
		/* Detect end TCP stack mode */
		addr = &mtcp->listener->socket->saddr;
		if (addr->sin_port == port) {
			if (addr->sin_addr.s_addr != INADDR_ANY) {
				if (ip == addr->sin_addr.s_addr) {
					rc |= STREAM_TYPE(MOS_SOCK_STREAM);
				}
			} else {
				int i;
				
				for (i = 0; i < g_config.mos->netdev_table->num; i++) {
					if (ip == g_config.mos->netdev_table->ent[i]->ip_addr) {
						rc |= STREAM_TYPE(MOS_SOCK_STREAM);
					}
				}
			}
		}
	}
	
	return rc;
}
/*----------------------------------------------------------------------------*/
static inline tcp_stream *
CreateServerStream(mtcp_manager_t mtcp, int type, struct pkt_ctx *pctx)
{
	tcp_stream *cur_stream = NULL;
	
	/* create new stream and add to flow hash table */
	cur_stream = CreateTCPStream(mtcp, NULL, type,
			pctx->p.iph->daddr, pctx->p.tcph->dest, 
			pctx->p.iph->saddr, pctx->p.tcph->source, NULL);
	if (!cur_stream) {
		TRACE_ERROR("INFO: Could not allocate tcp_stream!\n");
		return FALSE;
	}

	cur_stream->rcvvar->irs = pctx->p.seq;
	cur_stream->sndvar->peer_wnd = pctx->p.window;
	cur_stream->rcv_nxt = cur_stream->rcvvar->irs;
	cur_stream->sndvar->cwnd = 1;
	ParseTCPOptions(cur_stream, pctx->p.cur_ts, (uint8_t *)pctx->p.tcph + 
			TCP_HEADER_LEN, (pctx->p.tcph->doff << 2) - TCP_HEADER_LEN);
	
	return cur_stream;
}
/*----------------------------------------------------------------------------*/
static inline tcp_stream *
CreateMonitorStream(mtcp_manager_t mtcp, struct pkt_ctx* pctx, 
		    uint32_t stream_type, unsigned int *hash)
{
	tcp_stream *stream = NULL;
	struct socket_map *walk;
	/* create a client stream context */
	stream = CreateDualTCPStream(mtcp, NULL, stream_type, pctx->p.iph->daddr,
				     pctx->p.tcph->dest, pctx->p.iph->saddr, 
				     pctx->p.tcph->source, NULL);
	if (!stream)
		return FALSE;
	
	stream->side = MOS_SIDE_CLI;
	stream->pair_stream->side = MOS_SIDE_SVR;
	/* update recv context */
	stream->rcvvar->irs = pctx->p.seq;
	stream->sndvar->peer_wnd = pctx->p.window;
	stream->rcv_nxt = stream->rcvvar->irs + 1;
	stream->sndvar->cwnd = 1;
	
	/* 
	 * if buffer management is off, then disable 
	 * monitoring tcp ring of either streams (only if stream
	 * is just monitor stream active)
	 */
	if (IS_STREAM_TYPE(stream, MOS_SOCK_MONITOR_STREAM_ACTIVE)) {
		assert(IS_STREAM_TYPE(stream->pair_stream,
				      MOS_SOCK_MONITOR_STREAM_ACTIVE));
		
		stream->buffer_mgmt = FALSE;
		stream->pair_stream->buffer_mgmt = FALSE;
		
		/*
		 * if there is even a single monitor asking for
		 * buffer management, enable it (that's why the
		 * need for the loop)
		 */
		uint8_t bm;
		stream->status_mgmt = 0;
		SOCKQ_FOREACH_START(walk, &stream->msocks) {
			bm = walk->monitor_stream->monitor_listener->server_buf_mgmt;
			if (bm > stream->buffer_mgmt) {
				stream->buffer_mgmt = bm;
			}
			if (walk->monitor_stream->monitor_listener->server_mon == 1) {
				stream->status_mgmt = 1;
			}
		} SOCKQ_FOREACH_END;

		stream->pair_stream->status_mgmt = 0;
		SOCKQ_FOREACH_START(walk, &stream->pair_stream->msocks) {
			bm = walk->monitor_stream->monitor_listener->client_buf_mgmt;
			if (bm > stream->pair_stream->buffer_mgmt) {
				stream->pair_stream->buffer_mgmt = bm;
			}
			if (walk->monitor_stream->monitor_listener->client_mon == 1) {
				stream->pair_stream->status_mgmt = 1;
			}			
		} SOCKQ_FOREACH_END;
	}
	
	ParseTCPOptions(stream, pctx->p.cur_ts,
			(uint8_t *)pctx->p.tcph + TCP_HEADER_LEN,
			(pctx->p.tcph->doff << 2) - TCP_HEADER_LEN);
	
	return stream;
}
/*----------------------------------------------------------------------------*/
static inline struct tcp_stream *
FindStream(mtcp_manager_t mtcp, struct pkt_ctx *pctx, unsigned int *hash)
{
	struct tcp_stream temp_stream;
	
	temp_stream.saddr = pctx->p.iph->daddr;
	temp_stream.sport = pctx->p.tcph->dest;
	temp_stream.daddr = pctx->p.iph->saddr;
	temp_stream.dport = pctx->p.tcph->source;
	
	return HTSearch(mtcp->tcp_flow_table, &temp_stream, hash);
}
/*----------------------------------------------------------------------------*/
/* Create new flow for new packet or return NULL */
/*----------------------------------------------------------------------------*/
static inline struct tcp_stream *
CreateStream(mtcp_manager_t mtcp, struct pkt_ctx *pctx, unsigned int *hash) 
{
	tcp_stream *cur_stream = NULL;
	uint32_t stream_type;
	const struct iphdr *iph = pctx->p.iph;
	const struct tcphdr* tcph = pctx->p.tcph;
	
	if (tcph->syn && !tcph->ack) {
		/* handle the SYN */
		
		stream_type = DetectStreamType(mtcp, pctx, iph->daddr, tcph->dest);
		if (!stream_type) {
			TRACE_DBG("Refusing SYN packet.\n");
#ifdef DBGMSG
			DumpIPPacket(mtcp, iph, pctx->p.ip_len);
#endif
			return NULL;
		}
		
		/* if it is accepting connections only */
		if (stream_type == STREAM_TYPE(MOS_SOCK_STREAM)) {
			cur_stream = CreateServerStream(mtcp, stream_type, pctx);
			if (!cur_stream) {
				TRACE_DBG("No available space in flow pool.\n");
#ifdef DBGMSG
				DumpIPPacket(mtcp, iph, pctx->p.ip_len);
#endif
			}
		} else if (stream_type & STREAM_TYPE(MOS_SOCK_MONITOR_STREAM_ACTIVE)) {
			/* 
			 * create both monitoring streams, and accept 
			 * connection if it is set in embedded environment
			 */
#if 1
			cur_stream = CreateClientTCPStream(mtcp, NULL, stream_type,
									pctx->p.iph->saddr, pctx->p.tcph->source,
									pctx->p.iph->daddr, pctx->p.tcph->dest,
									hash);
#else
			cur_stream = CreateMonitorStream(mtcp, pctx, stream_type, hash);
#endif
			if (!cur_stream) {
				TRACE_DBG("No available space in flow pool.\n");
#ifdef DBGMSG
				DumpIPPacket(mtcp, iph, pctx->p.ip_len);
#endif
			}
		}  else {
			/* invalid stream type! */
		}
		
		return cur_stream;
		
	} else {
		TRACE_DBG("Weird packet comes.\n");
#ifdef DBGMSG
		DumpIPPacket(mtcp, iph, pctx->p.ip_len);
#endif
		return NULL;
	}
}
/*----------------------------------------------------------------------------*/
inline void
FillPacketContextTCPInfo(struct pkt_ctx *pctx, struct tcphdr * tcph)
{
	pctx->p.tcph = tcph;
	pctx->p.payload    = (uint8_t *)tcph + (tcph->doff << 2);
	pctx->p.payloadlen = pctx->p.ip_len - (pctx->p.payload - (u_char *)pctx->p.iph);
	pctx->p.seq = ntohl(tcph->seq);
	pctx->p.ack_seq = ntohl(tcph->ack_seq);
	pctx->p.window = ntohs(tcph->window);
	pctx->p.offset = 0;
	
	return ;
}
/*----------------------------------------------------------------------------*/
/**
 * Called for every incoming packet from the NIC (when monitoring is disabled)
 */
static void 
HandleSockStream(mtcp_manager_t mtcp, struct tcp_stream *cur_stream, 
				struct pkt_ctx *pctx) 
{
	UpdateRecvTCPContext(mtcp, cur_stream, pctx);
	DoActionEndTCPPacket(mtcp, cur_stream, pctx);	
}
/*----------------------------------------------------------------------------*/
void 
UpdateMonitor(mtcp_manager_t mtcp, struct tcp_stream *sendside_stream, 
			struct tcp_stream *recvside_stream, struct pkt_ctx *pctx,
			bool is_pkt_reception) 
{
	struct socket_map *walk;

	assert(pctx);

#ifdef RECORDPKT_PER_STREAM 
	/* clone sendside_stream even if sender is disabled */
	ClonePacketCtx(&sendside_stream->last_pctx.p,
		       sendside_stream->last_pkt_data, &(pctx.p));
#endif
	/* update send stream context first */
	if (sendside_stream->status_mgmt) {
		sendside_stream->cb_events = MOS_ON_PKT_IN;

		if (is_pkt_reception)
			UpdatePassiveSendTCPContext(mtcp, sendside_stream, pctx);

		sendside_stream->allow_pkt_modification = true;
		/* POST hook of sender */
		SOCKQ_FOREACH_START(walk, &sendside_stream->msocks) {
			HandleCallback(mtcp, MOS_HK_SND, walk, sendside_stream->side,
				       pctx, sendside_stream->cb_events);
		} SOCKQ_FOREACH_END;
		sendside_stream->allow_pkt_modification = false;
	}

	/* Attach Server-side stream */
	if (recvside_stream == NULL) {
		assert(sendside_stream->side == MOS_SIDE_CLI);
		if ((recvside_stream = AttachServerTCPStream(mtcp, sendside_stream, 0,
				pctx->p.iph->saddr, pctx->p.tcph->source,
				pctx->p.iph->daddr, pctx->p.tcph->dest)) == NULL) {
			DestroyTCPStream(mtcp, sendside_stream);
			return;
		}
		/* update recv context */
		recvside_stream->rcvvar->irs = pctx->p.seq;
		recvside_stream->sndvar->peer_wnd = pctx->p.window;
		recvside_stream->rcv_nxt = recvside_stream->rcvvar->irs + 1;
		recvside_stream->sndvar->cwnd = 1;

		ParseTCPOptions(recvside_stream, pctx->p.cur_ts,
				(uint8_t *)pctx->p.tcph + TCP_HEADER_LEN,
				(pctx->p.tcph->doff << 2) - TCP_HEADER_LEN);
	}

	/* Perform post-send tcp activities */
	PostSendTCPAction(mtcp, pctx, recvside_stream, sendside_stream);

	if (/*1*/recvside_stream->status_mgmt) {
		recvside_stream->cb_events = MOS_ON_PKT_IN;

		/* Predict events which may be raised prior to performing TCP processing */
		PreRecvTCPEventPrediction(mtcp, pctx, recvside_stream);
		
		/* retransmitted packet should avoid event simulation */
		//if ((recvside_stream->cb_events & MOS_ON_REXMIT) == 0)
			/* update receive stream context (recv_side stream) */
		if (is_pkt_reception)
			UpdateRecvTCPContext(mtcp, recvside_stream, pctx);
		else
			UpdatePassiveRecvTCPContext(mtcp, recvside_stream, pctx);

		/* POST hook of receiver */
		SOCKQ_FOREACH_START(walk, &recvside_stream->msocks) {
			HandleCallback(mtcp, MOS_HK_RCV, walk, recvside_stream->side,
				       pctx, recvside_stream->cb_events);
		} SOCKQ_FOREACH_END;
	}
	
	/* reset callback events counter */
	recvside_stream->cb_events = 0;
	sendside_stream->cb_events = 0;
}
/*----------------------------------------------------------------------------*/
static void 
HandleMonitorStream(mtcp_manager_t mtcp, struct tcp_stream *sendside_stream, 
			struct tcp_stream *recvside_stream, struct pkt_ctx *pctx) 
{
	UpdateMonitor(mtcp, sendside_stream, recvside_stream, pctx, true);

	recvside_stream = sendside_stream->pair_stream;

	if (HAS_STREAM_TYPE(recvside_stream, MOS_SOCK_STREAM)) {
		DoActionEndTCPPacket(mtcp, recvside_stream, pctx);
	} else {
		/* forward packets */
		if (pctx->forward)
			ForwardIPPacket(mtcp, pctx);
		
		if (recvside_stream->stream_type == sendside_stream->stream_type &&
		    IS_STREAM_TYPE(recvside_stream, MOS_SOCK_MONITOR_STREAM_ACTIVE)) {
			if (((recvside_stream->state == TCP_ST_TIME_WAIT &&
				  g_config.mos->tcp_tw_interval == 0) ||
			     recvside_stream->state == TCP_ST_CLOSED_RSVD ||
			     !recvside_stream->status_mgmt) &&
			    ((sendside_stream->state == TCP_ST_TIME_WAIT &&
				  g_config.mos->tcp_tw_interval == 0) ||
			     sendside_stream->state == TCP_ST_CLOSED_RSVD ||
			     !sendside_stream->status_mgmt))

				DestroyTCPStream(mtcp, recvside_stream);
			}
	}
}
/*----------------------------------------------------------------------------*/
int
ProcessInTCPPacket(mtcp_manager_t mtcp, struct pkt_ctx *pctx)
{
	uint64_t events = 0;
	struct tcp_stream *cur_stream;
	struct iphdr* iph;
	struct tcphdr* tcph;
	struct mon_listener *walk;
	unsigned int hash = 0;
	
	iph = pctx->p.iph;
	tcph = (struct tcphdr *)((u_char *)pctx->p.iph + (pctx->p.iph->ihl << 2));
	
	FillPacketContextTCPInfo(pctx, tcph);

	/* callback for monitor raw socket */
	TAILQ_FOREACH(walk, &mtcp->monitors, link)
		if (walk->socket->socktype == MOS_SOCK_MONITOR_RAW)
			HandleCallback(mtcp, MOS_NULL, walk->socket, MOS_SIDE_BOTH,
				       pctx, MOS_ON_PKT_IN);

	if (pctx->p.ip_len < ((iph->ihl + tcph->doff) << 2))
		return ERROR;

#if VERIFY_RX_CHECKSUM
	if (TCPCalcChecksum((uint16_t *)pctx->p.tcph,
						(tcph->doff << 2) + pctx->p.payloadlen,
						iph->saddr, pctx->p.iph->daddr)) {
		TRACE_DBG("Checksum Error: Original: 0x%04x, calculated: 0x%04x\n",
				tcph->check, TCPCalcChecksum((uint16_t *)tcph,
				(tcph->doff << 2) + pctx->p.payloadlen,
				iph->saddr, iph->daddr));
		if (pctx->forward && mtcp->num_msp)
			ForwardIPPacket(mtcp, pctx);
		return ERROR;
	}
#endif
	events |= MOS_ON_PKT_IN;

	/* Check whether a packet is belong to any stream */
	cur_stream = FindStream(mtcp, pctx, &hash);
	if (!cur_stream) {
		/*
		 * No need to create stream for monitor. 
		 *  But do create 1 for client case!  
		 */
		if (mtcp->listener == NULL && mtcp->num_msp == 0) {
			//if (pctx->forward)
			//	ForwardIPPacket(mtcp, pctx);
			return TRUE;
		}
		/* Create new flow for new packet or return NULL */
		cur_stream = CreateStream(mtcp, pctx, &hash);
		if (!cur_stream)
			events = MOS_ON_ORPHAN;
	}

	if (cur_stream) {
		cur_stream->cb_events = events;

		if (cur_stream->rcvvar && cur_stream->rcvvar->rcvbuf)
			pctx->p.offset = (uint64_t)seq2loff(cur_stream->rcvvar->rcvbuf,
					pctx->p.seq, cur_stream->rcvvar->irs + 1);

		if (IS_STREAM_TYPE(cur_stream, MOS_SOCK_STREAM))
			HandleSockStream(mtcp, cur_stream, pctx);			
		
		else if (HAS_STREAM_TYPE(cur_stream, MOS_SOCK_MONITOR_STREAM_ACTIVE))
			HandleMonitorStream(mtcp, cur_stream, cur_stream->pair_stream, pctx);
		else
			assert(0);
	} else {
		struct mon_listener *walk;
		struct sfbpf_program fcode;
		/* 
		 * event callback for pkt_no_conn; MOS_SIDE_BOTH
		 * means that we can't judge sides here 
		 */
		TAILQ_FOREACH(walk, &mtcp->monitors, link) {
			/* mtcp_bind_monitor_filter()
			 * - apply stream orphan filter to every pkt before raising ORPHAN event */
			fcode = walk->stream_orphan_fcode;
			if (!(ISSET_BPFFILTER(fcode) && pctx &&
				EVAL_BPFFILTER(fcode, (uint8_t *)pctx->p.iph - sizeof(struct ethhdr),
							   pctx->p.ip_len + sizeof(struct ethhdr)) == 0)) {
				HandleCallback(mtcp, MOS_NULL, walk->socket, MOS_SIDE_BOTH,
					       pctx, events);
			}
		}
		if (mtcp->listener) {
			/* RFC 793 (page 65) says
			   "An incoming segment containing a RST is discarded."
			   if the TCP state is CLOSED (= TCP stream does not exist). */
			if (!tcph->rst)
				/* Send RST if it is run as EndTCP only mode */
				SendTCPPacketStandalone(mtcp,
							iph->daddr, tcph->dest, iph->saddr, tcph->source,
							0, pctx->p.seq + pctx->p.payloadlen + 1, 0, TCP_FLAG_RST | TCP_FLAG_ACK,
							NULL, 0, pctx->p.cur_ts, 0, 0, -1);
		} else if (pctx->forward) {
			/* Do forward or drop if it run as Monitor only mode */
			ForwardIPPacket(mtcp, pctx);
		}
	}
	
	return TRUE;
}
/*----------------------------------------------------------------------------*/
