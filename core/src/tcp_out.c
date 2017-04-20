#include <unistd.h>
#include <string.h>

#include "tcp_out.h"
#include "mtcp.h"
#include "ip_in.h"
#include "ip_out.h"
#include "tcp_in.h"
#include "tcp.h"
#include "tcp_stream.h"
#include "eventpoll.h"
#include "timer.h"
#include "debug.h"
#include "config.h"

#define TCP_CALCULATE_CHECKSUM		TRUE
#define ACK_PIGGYBACK			TRUE
/* Enable this for higher concurrency rate experiments */
#define TRY_SEND_BEFORE_QUEUE		/*FALSE*/ TRUE

#define TCP_MAX_WINDOW 65535

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

/*----------------------------------------------------------------------------*/
static inline uint16_t
CalculateOptionLength(uint8_t flags)
{
	uint16_t optlen = 0;

	if (flags & TCP_FLAG_SYN) {
		optlen += TCP_OPT_MSS_LEN;
#if TCP_OPT_SACK_ENABLED
		optlen += TCP_OPT_SACK_PERMIT_LEN;
#if !TCP_OPT_TIMESTAMP_ENABLED
		optlen += 2;	// insert NOP padding
#endif /* TCP_OPT_TIMESTAMP_ENABLED */
#endif /* TCP_OPT_SACK_ENABLED */

#if TCP_OPT_TIMESTAMP_ENABLED
		optlen += TCP_OPT_TIMESTAMP_LEN;
#if !TCP_OPT_SACK_ENABLED
		optlen += 2;	// insert NOP padding
#endif /* TCP_OPT_SACK_ENABLED */
#endif /* TCP_OPT_TIMESTAMP_ENABLED */

		optlen += TCP_OPT_WSCALE_LEN + 1;

	} else {

#if TCP_OPT_TIMESTAMP_ENABLED
		optlen += TCP_OPT_TIMESTAMP_LEN + 2;
#endif

#if TCP_OPT_SACK_ENABLED
		if (flags & TCP_FLAG_SACK) {
			optlen += TCP_OPT_SACK_LEN + 2;
		}
#endif
	}

	assert(optlen % 4 == 0);

	return optlen;
}
/*----------------------------------------------------------------------------*/
static inline void
GenerateTCPTimestamp(tcp_stream *cur_stream, uint8_t *tcpopt, uint32_t cur_ts)
{
	uint32_t *ts = (uint32_t *)(tcpopt + 2);

	tcpopt[0] = TCP_OPT_TIMESTAMP;
	tcpopt[1] = TCP_OPT_TIMESTAMP_LEN;
	ts[0] = htonl(cur_ts);
	ts[1] = htonl(cur_stream->rcvvar->ts_recent);
}
/*----------------------------------------------------------------------------*/
static inline void
GenerateTCPOptions(tcp_stream *cur_stream, uint32_t cur_ts, 
		uint8_t flags, uint8_t *tcpopt, uint16_t optlen)
{
	int i = 0;

	if (flags & TCP_FLAG_SYN) {
		uint16_t mss;

		/* MSS option */
		mss = cur_stream->sndvar->mss;
		tcpopt[i++] = TCP_OPT_MSS;
		tcpopt[i++] = TCP_OPT_MSS_LEN;
		tcpopt[i++] = mss >> 8;
		tcpopt[i++] = mss % 256;

		/* SACK permit */
#if TCP_OPT_SACK_ENABLED
#if !TCP_OPT_TIMESTAMP_ENABLED
		tcpopt[i++] = TCP_OPT_NOP;
		tcpopt[i++] = TCP_OPT_NOP;
#endif /* TCP_OPT_TIMESTAMP_ENABLED */
		tcpopt[i++] = TCP_OPT_SACK_PERMIT;
		tcpopt[i++] = TCP_OPT_SACK_PERMIT_LEN;
		TRACE_SACK("Local SACK permited.\n");
#endif /* TCP_OPT_SACK_ENABLED */

		/* Timestamp */
#if TCP_OPT_TIMESTAMP_ENABLED
#if !TCP_OPT_SACK_ENABLED
		tcpopt[i++] = TCP_OPT_NOP;
		tcpopt[i++] = TCP_OPT_NOP;
#endif /* TCP_OPT_SACK_ENABLED */
		GenerateTCPTimestamp(cur_stream, tcpopt + i, cur_ts);
		i += TCP_OPT_TIMESTAMP_LEN;
#endif /* TCP_OPT_TIMESTAMP_ENABLED */

		/* Window scale */
		tcpopt[i++] = TCP_OPT_NOP;
		tcpopt[i++] = TCP_OPT_WSCALE;
		tcpopt[i++] = TCP_OPT_WSCALE_LEN;
		tcpopt[i++] = cur_stream->sndvar->wscale_mine;

	} else {

#if TCP_OPT_TIMESTAMP_ENABLED
		tcpopt[i++] = TCP_OPT_NOP;
		tcpopt[i++] = TCP_OPT_NOP;
		GenerateTCPTimestamp(cur_stream, tcpopt + i, cur_ts);
		i += TCP_OPT_TIMESTAMP_LEN;
#endif

#if TCP_OPT_SACK_ENABLED
		if (flags & TCP_OPT_SACK) {
			// TODO: implement SACK support
		}
#endif
	}

	assert (i == optlen);
}
/*----------------------------------------------------------------------------*/
int
SendTCPPacketStandalone(struct mtcp_manager *mtcp, 
		uint32_t saddr, uint16_t sport, uint32_t daddr, uint16_t dport, 
		uint32_t seq, uint32_t ack_seq, uint16_t window, uint8_t flags, 
		uint8_t *payload, uint16_t payloadlen, 
		uint32_t cur_ts, uint32_t echo_ts, uint16_t ip_id, int8_t in_ifidx)
{
	struct tcphdr *tcph;
	uint8_t *tcpopt;
	uint32_t *ts;
	uint16_t optlen;
	struct pkt_ctx pctx;
	int rc = -1;
	
	memset(&pctx, 0, sizeof(pctx));
	pctx.p.in_ifidx = in_ifidx;
	optlen = CalculateOptionLength(flags);
	if (payloadlen > TCP_DEFAULT_MSS + optlen) {
		TRACE_ERROR("Payload size exceeds MSS.\n");
		assert(0);
		return ERROR;
	}

	tcph = (struct tcphdr *)IPOutputStandalone(mtcp, htons(ip_id), 
			saddr, daddr, TCP_HEADER_LEN + optlen + payloadlen, &pctx, cur_ts);
	if (tcph == NULL) {
		return ERROR;
	}
	memset(tcph, 0, TCP_HEADER_LEN + optlen);

	tcph->source = sport;
	tcph->dest = dport;

	if (flags & TCP_FLAG_SYN)
		tcph->syn = TRUE;
	if (flags & TCP_FLAG_FIN)
		tcph->fin = TRUE;
	if (flags & TCP_FLAG_RST)
		tcph->rst = TRUE;
	if (flags & TCP_FLAG_PSH)
		tcph->psh = TRUE;

	tcph->seq = htonl(seq);
	if (flags & TCP_FLAG_ACK) {
		tcph->ack = TRUE;
		tcph->ack_seq = htonl(ack_seq);
	}

	tcph->window = htons(MIN(window, TCP_MAX_WINDOW));

	tcpopt = (uint8_t *)tcph + TCP_HEADER_LEN;
	ts = (uint32_t *)(tcpopt + 4);

	tcpopt[0] = TCP_OPT_NOP;
	tcpopt[1] = TCP_OPT_NOP;
	tcpopt[2] = TCP_OPT_TIMESTAMP;
	tcpopt[3] = TCP_OPT_TIMESTAMP_LEN;
	ts[0] = htonl(cur_ts);
	ts[1] = htonl(echo_ts);

	tcph->doff = (TCP_HEADER_LEN + optlen) >> 2;
	// copy payload if exist
	if (payloadlen > 0) {
		memcpy((uint8_t *)tcph + TCP_HEADER_LEN + optlen, payload, payloadlen);
	}

#if TCP_CALCULATE_CHECKSUM
	/* offload TCP checkum if possible */
	if (likely(mtcp->iom->dev_ioctl != NULL))
		rc = mtcp->iom->dev_ioctl(mtcp->ctx,
					  pctx.out_ifidx,
					  PKT_TX_TCP_CSUM,
					  pctx.p.iph);
	/* otherwise calculate TCP checksum in S/W */
	if (rc == -1)
		tcph->check = TCPCalcChecksum((uint16_t *)tcph, 
					      TCP_HEADER_LEN + 
					      optlen + payloadlen,
					      saddr, daddr);
#endif

	if (tcph->syn || tcph->fin) {
		payloadlen++;
	}

	struct mon_listener *walk;
	/* callback for monitor raw socket */
	TAILQ_FOREACH(walk, &mtcp->monitors, link)
		if (walk->socket->socktype == MOS_SOCK_MONITOR_RAW)
			HandleCallback(mtcp, MOS_NULL, walk->socket, MOS_SIDE_BOTH,
				       &pctx, MOS_ON_PKT_IN);
	return payloadlen;
}
/*----------------------------------------------------------------------------*/
int
SendTCPPacket(struct mtcp_manager *mtcp, tcp_stream *cur_stream, 
		uint32_t cur_ts, uint8_t flags, uint8_t *payload, uint16_t payloadlen)
{
	struct tcphdr *tcph;
	uint16_t optlen;
	uint8_t wscale = 0;
	uint32_t window32 = 0;
	struct pkt_ctx pctx;
	int rc = -1;

	memset(&pctx, 0, sizeof(pctx));
	optlen = CalculateOptionLength(flags);
	if (payloadlen > cur_stream->sndvar->mss + optlen) {
		TRACE_ERROR("Payload size exceeds MSS\n");
		return ERROR;
	}

	tcph = (struct tcphdr *)IPOutput(mtcp, cur_stream, 
			TCP_HEADER_LEN + optlen + payloadlen, &pctx, cur_ts);
	if (tcph == NULL) {
		return -2;
	}
	memset(tcph, 0, TCP_HEADER_LEN + optlen);

	tcph->source = cur_stream->sport;
	tcph->dest = cur_stream->dport;

	if (flags & TCP_FLAG_SYN) {
		tcph->syn = TRUE;
		if (cur_stream->snd_nxt != cur_stream->sndvar->iss) {
			TRACE_DBG("Stream %d: weird SYN sequence. "
					"snd_nxt: %u, iss: %u\n", cur_stream->id, 
					cur_stream->snd_nxt, cur_stream->sndvar->iss);
		}
		TRACE_DBG("Stream %d: Sending SYN. seq: %u, ack_seq: %u\n", 
			  cur_stream->id, cur_stream->snd_nxt, cur_stream->rcv_nxt);
	}
	if (flags & TCP_FLAG_RST) {
		TRACE_FIN("Stream %d: Sending RST.\n", cur_stream->id);
		tcph->rst = TRUE;
	}
	if (flags & TCP_FLAG_PSH)
		tcph->psh = TRUE;

	if (flags & TCP_FLAG_WACK) {
		tcph->seq = htonl(cur_stream->snd_nxt - 1);
		TRACE_CLWND("%u Sending ACK to get new window advertisement. "
				"seq: %u, peer_wnd: %u, snd_nxt - snd_una: %u\n", 
				cur_stream->id,
				cur_stream->snd_nxt - 1, cur_stream->sndvar->peer_wnd, 
				cur_stream->snd_nxt - cur_stream->sndvar->snd_una);
	} else if (flags & TCP_FLAG_FIN) {
		tcph->fin = TRUE;
		
		if (cur_stream->sndvar->fss == 0) {
			TRACE_ERROR("Stream %u: not fss set. closed: %u\n", 
					cur_stream->id, cur_stream->closed);
		}
		tcph->seq = htonl(cur_stream->sndvar->fss);
		cur_stream->sndvar->is_fin_sent = TRUE;
		TRACE_FIN("Stream %d: Sending FIN. seq: %u, ack_seq: %u\n", 
				cur_stream->id, cur_stream->snd_nxt, cur_stream->rcv_nxt);
	} else {
		tcph->seq = htonl(cur_stream->snd_nxt);
	}

	if (flags & TCP_FLAG_ACK) {
		tcph->ack = TRUE;
		tcph->ack_seq = htonl(cur_stream->rcv_nxt);
		cur_stream->sndvar->ts_lastack_sent = cur_ts;
		cur_stream->last_active_ts = cur_ts;
		UpdateTimeoutList(mtcp, cur_stream);
	}

	if (flags & TCP_FLAG_SYN) {
		wscale = 0;
	} else {
		wscale = cur_stream->sndvar->wscale_mine;
	}

	window32 = cur_stream->rcvvar->rcv_wnd >> wscale;
	tcph->window = htons((uint16_t)MIN(window32, TCP_MAX_WINDOW));
	/* if the advertised window is 0, we need to advertise again later */
	if (window32 == 0) {
		cur_stream->need_wnd_adv = TRUE;
	}

	GenerateTCPOptions(cur_stream, cur_ts, flags, 
			(uint8_t *)tcph + TCP_HEADER_LEN, optlen);
	
	tcph->doff = (TCP_HEADER_LEN + optlen) >> 2;
	// copy payload if exist
	if (payloadlen > 0) {
		memcpy((uint8_t *)tcph + TCP_HEADER_LEN + optlen, payload, payloadlen);
	}

#if TCP_CALCULATE_CHECKSUM
	if (likely(mtcp->iom->dev_ioctl != NULL))
		rc = mtcp->iom->dev_ioctl(mtcp->ctx,
					  pctx.out_ifidx,
					  PKT_TX_TCP_CSUM,
					  pctx.p.iph);
	if (rc == -1)
		tcph->check = TCPCalcChecksum((uint16_t *)tcph, 
					      TCP_HEADER_LEN + 
					      optlen + payloadlen, 
					      cur_stream->saddr, 
					      cur_stream->daddr);
#endif
	cur_stream->snd_nxt += payloadlen;

	if (tcph->syn || tcph->fin) {
		cur_stream->snd_nxt++;
		payloadlen++;
	}

	if (payloadlen > 0) {
		if (cur_stream->state > TCP_ST_ESTABLISHED) {
			TRACE_FIN("Payload after ESTABLISHED: length: %d, snd_nxt: %u\n", 
				  payloadlen, cur_stream->snd_nxt);
		}

		/* update retransmission timer if have payload */
		cur_stream->sndvar->ts_rto = cur_ts + cur_stream->sndvar->rto;
		TRACE_RTO("Updating retransmission timer. "
				"cur_ts: %u, rto: %u, ts_rto: %u\n", 
				cur_ts, cur_stream->sndvar->rto, cur_stream->sndvar->ts_rto);
		AddtoRTOList(mtcp, cur_stream);
	}

	struct mon_listener *walk;
	/* callback for monitor raw socket */
	TAILQ_FOREACH(walk, &mtcp->monitors, link)
		if (walk->socket->socktype == MOS_SOCK_MONITOR_RAW)
			HandleCallback(mtcp, MOS_NULL, walk->socket, MOS_SIDE_BOTH,
				       &pctx, MOS_ON_PKT_IN);

	if (mtcp->num_msp /* this means that stream monitor is on */) {
		FillPacketContextTCPInfo(&pctx, tcph);

		/* New abstraction for monitor stream */
		struct tcp_stream *recvside_stream = cur_stream->pair_stream;
		struct tcp_stream *sendside_stream = cur_stream;

		if (recvside_stream) {
			if (recvside_stream->rcvvar && recvside_stream->rcvvar->rcvbuf)
				pctx.p.offset = (uint64_t)seq2loff(recvside_stream->rcvvar->rcvbuf,
								   pctx.p.seq, recvside_stream->rcvvar->irs + 1);
			
			UpdateMonitor(mtcp, sendside_stream, recvside_stream, &pctx, false);
		}
	}

#ifdef PKTDUMP
	DumpPacket(mtcp,
			(char *)tcph - sizeof(struct iphdr) - sizeof(struct ethhdr),
			payloadlen + sizeof(struct iphdr) + sizeof(struct ethhdr),
			"OUT", -1);
#endif


	return payloadlen;
}
/*----------------------------------------------------------------------------*/
static int
FlushTCPSendingBuffer(mtcp_manager_t mtcp, tcp_stream *cur_stream, uint32_t cur_ts)
{
	struct tcp_send_vars *sndvar = cur_stream->sndvar;
	const uint32_t maxlen = sndvar->mss - CalculateOptionLength(TCP_FLAG_ACK);
	uint8_t *data;
	uint32_t buffered_len;
	uint32_t seq;
	uint16_t len;
	int16_t sndlen;
	uint32_t window;
	int packets = 0;

	if (!sndvar->sndbuf) {
		TRACE_ERROR("Stream %d: No send buffer available.\n", cur_stream->id);
		assert(0);
		return 0;
	}

	SBUF_LOCK(&sndvar->write_lock);

	if (sndvar->sndbuf->len == 0) {
		packets = 0;
		goto out;
	}

	window = MIN(sndvar->cwnd, sndvar->peer_wnd);

	while (1) {
		seq = cur_stream->snd_nxt;

		if (TCP_SEQ_LT(seq, sndvar->sndbuf->head_seq)) {
			TRACE_ERROR("Stream %d: Invalid sequence to send. "
					"state: %s, seq: %u, head_seq: %u.\n", 
					cur_stream->id, TCPStateToString(cur_stream), 
					seq, sndvar->sndbuf->head_seq);
			assert(0);
			break;
		}
		buffered_len = sndvar->sndbuf->head_seq + sndvar->sndbuf->len - seq;
		if (cur_stream->state > TCP_ST_ESTABLISHED) {
			TRACE_FIN("head_seq: %u, len: %u, seq: %u, "
					"buffered_len: %u\n", sndvar->sndbuf->head_seq, 
					sndvar->sndbuf->len, seq, buffered_len);
		}
		if (buffered_len == 0)
			break;

		data = sndvar->sndbuf->head + 
				(seq - sndvar->sndbuf->head_seq);

		if (buffered_len > maxlen) {
			len = maxlen;
		} else {
			len = buffered_len;
		}
		
		if (len <= 0)
			break;

		if (cur_stream->state > TCP_ST_ESTABLISHED) {
			TRACE_FIN("Flushing after ESTABLISHED: seq: %u, len: %u, "
					"buffered_len: %u\n", seq, len, buffered_len);
		}

		if (seq - sndvar->snd_una + len > window) {
			/* Ask for new window advertisement to peer */
			if (seq - sndvar->snd_una + len > sndvar->peer_wnd) {
				TRACE_DBG("Full peer window. "
					  "peer_wnd: %u, (snd_nxt-snd_una): %u\n", 
					  sndvar->peer_wnd, seq - sndvar->snd_una);
				if (TS_TO_MSEC(cur_ts - sndvar->ts_lastack_sent) > 500) {
					EnqueueACK(mtcp, cur_stream, cur_ts, ACK_OPT_WACK);
				}
			}
			packets = -3;
			goto out;
		}
	
		sndlen = SendTCPPacket(mtcp, cur_stream, cur_ts, 
				TCP_FLAG_ACK, data, len);
		if (sndlen < 0) {
			packets = sndlen;
			goto out;
		}
		packets++;
	}

 out:
	SBUF_UNLOCK(&sndvar->write_lock);	
	return packets;
}
/*----------------------------------------------------------------------------*/
static inline int 
SendControlPacket(mtcp_manager_t mtcp, tcp_stream *cur_stream, uint32_t cur_ts)
{
	struct tcp_send_vars *sndvar = cur_stream->sndvar;
	int ret = 0;
    int flag = 0;

    switch (cur_stream->state) {
       case TCP_ST_SYN_SENT: 		/* Send SYN here */
          flag = TCP_FLAG_SYN;
          break;
       case TCP_ST_SYN_RCVD:        /* Send SYN/ACK here */
          cur_stream->snd_nxt = sndvar->iss;
          flag = TCP_FLAG_SYN | TCP_FLAG_ACK;
          break;
       case TCP_ST_ESTABLISHED:     /* Send ACK here */
       case TCP_ST_CLOSE_WAIT:	    /* Send ACK for the FIN here */
       case TCP_ST_FIN_WAIT_2:      /* Send ACK here */
       case TCP_ST_TIME_WAIT:       /* Send ACK here */
          flag = TCP_FLAG_ACK;
          break;
       case TCP_ST_LAST_ACK:
       case TCP_ST_FIN_WAIT_1:
          /* if it is on ack_list, send it after sending ack */
          if (sndvar->on_send_list || sndvar->on_ack_list) 
             return (-1);
          flag = TCP_FLAG_FIN | TCP_FLAG_ACK; /* Send FIN/ACK here */
          break;
       case TCP_ST_CLOSING:
          if (sndvar->is_fin_sent) {
             /* if the sequence is for FIN, send FIN */
             flag = (cur_stream->snd_nxt == sndvar->fss) ?
                (TCP_FLAG_FIN | TCP_FLAG_ACK) : TCP_FLAG_ACK;
          } else {
             /* if FIN is not sent, send fin with ack */
             flag = TCP_FLAG_FIN | TCP_FLAG_ACK;
          }
       case TCP_ST_CLOSED_RSVD: /* Send RST here */
          TRACE_DBG("Stream %d: Try sending RST (TCP_ST_CLOSED_RSVD)\n", 
                    cur_stream->id);
          /* first flush the data and ack */
          if (sndvar->on_send_list || sndvar->on_ack_list) 
             return (-1);
          ret = SendTCPPacket(mtcp, cur_stream, cur_ts, TCP_FLAG_RST, NULL, 0);
          if (ret >= 0)
             DestroyTCPStream(mtcp, cur_stream);
          return (ret);
       default:
          TRACE_ERROR("Stream %d: shouldn't send a control packet\n", 
                      cur_stream->id);
          assert(0); /* can't reach here! */
          return (0);
    }

    return SendTCPPacket(mtcp, cur_stream, cur_ts, flag, NULL, 0);
}
/*----------------------------------------------------------------------------*/
inline int 
WriteTCPControlList(mtcp_manager_t mtcp, 
		struct mtcp_sender *sender, uint32_t cur_ts, int thresh)
{
	tcp_stream *cur_stream;
	tcp_stream *next, *last;
	int cnt = 0;
	int ret;

	thresh = MIN(thresh, sender->control_list_cnt);

	/* Send TCP control messages */
	cnt = 0;
	cur_stream = TAILQ_FIRST(&sender->control_list);
	last = TAILQ_LAST(&sender->control_list, control_head);
	while (cur_stream) {
		if (++cnt > thresh)
			break;

		TRACE_LOOP("Inside control loop. cnt: %u, stream: %d\n", 
				cnt, cur_stream->id);
		next = TAILQ_NEXT(cur_stream, sndvar->control_link);

		TAILQ_REMOVE(&sender->control_list, cur_stream, sndvar->control_link);
		sender->control_list_cnt--;

		if (cur_stream->sndvar->on_control_list) {
			cur_stream->sndvar->on_control_list = FALSE;
			//TRACE_DBG("Stream %u: Sending control packet\n", cur_stream->id);
			ret = SendControlPacket(mtcp, cur_stream, cur_ts);
			if (ret < 0) {
				TAILQ_INSERT_HEAD(&sender->control_list, 
						cur_stream, sndvar->control_link);
				cur_stream->sndvar->on_control_list = TRUE;
				sender->control_list_cnt++;
				/* since there is no available write buffer, break */
				break;
			}
		} else {
			TRACE_ERROR("Stream %d: not on control list.\n", cur_stream->id);
		}

		if (cur_stream == last) 
			break;
		cur_stream = next;
	}

	return cnt;
}
/*----------------------------------------------------------------------------*/
inline int 
WriteTCPDataList(mtcp_manager_t mtcp, 
		struct mtcp_sender *sender, uint32_t cur_ts, int thresh)
{
	tcp_stream *cur_stream;
	tcp_stream *next, *last;
	int cnt = 0;
	int ret;

	/* Send data */
	cnt = 0;
	cur_stream = TAILQ_FIRST(&sender->send_list);
	last = TAILQ_LAST(&sender->send_list, send_head);
	while (cur_stream) {
		if (++cnt > thresh)
			break;

		TRACE_LOOP("Inside send loop. cnt: %u, stream: %d\n", 
				cnt, cur_stream->id);
		next = TAILQ_NEXT(cur_stream, sndvar->send_link);

		TAILQ_REMOVE(&sender->send_list, cur_stream, sndvar->send_link);
		if (cur_stream->sndvar->on_send_list) {
			ret = 0;

			/* Send data here */
			/* Only can send data when ESTABLISHED or CLOSE_WAIT */
			if (cur_stream->state == TCP_ST_ESTABLISHED) {
				if (cur_stream->sndvar->on_control_list) {
					/* delay sending data after until on_control_list becomes off */
					//TRACE_DBG("Stream %u: delay sending data.\n", cur_stream->id);
					ret = -1;
				} else {
					ret = FlushTCPSendingBuffer(mtcp, cur_stream, cur_ts);
				}
			} else if (cur_stream->state == TCP_ST_CLOSE_WAIT || 
					cur_stream->state == TCP_ST_FIN_WAIT_1 || 
					cur_stream->state == TCP_ST_LAST_ACK) {
				ret = FlushTCPSendingBuffer(mtcp, cur_stream, cur_ts);
			} else {
				TRACE_DBG("Stream %d: on_send_list at state %s\n", 
						cur_stream->id, TCPStateToString(cur_stream));
#if DUMP_STREAM
				DumpStream(mtcp, cur_stream);
#endif
			}

			if (ret < 0) {
				TAILQ_INSERT_TAIL(&sender->send_list, cur_stream, sndvar->send_link);
				/* since there is no available write buffer, break */
				break;

			} else {
				cur_stream->sndvar->on_send_list = FALSE;
				sender->send_list_cnt--;
				/* the ret value is the number of packets sent. */
				/* decrease ack_cnt for the piggybacked acks */
#if ACK_PIGGYBACK
				if (cur_stream->sndvar->ack_cnt > 0) {
					if (cur_stream->sndvar->ack_cnt > ret) {
						cur_stream->sndvar->ack_cnt -= ret;
					} else {
						cur_stream->sndvar->ack_cnt = 0;
					}
				}
#endif
#if 1
				if (cur_stream->control_list_waiting) {
					if (!cur_stream->sndvar->on_ack_list) {
						cur_stream->control_list_waiting = FALSE;
						AddtoControlList(mtcp, cur_stream, cur_ts);
					}
				}
#endif
			}
		} else {
			TRACE_ERROR("Stream %d: not on send list.\n", cur_stream->id);
#ifdef DUMP_STREAM
			DumpStream(mtcp, cur_stream);
#endif
		}

		if (cur_stream == last) 
			break;
		cur_stream = next;
	}

	return cnt;
}
/*----------------------------------------------------------------------------*/
inline int 
WriteTCPACKList(mtcp_manager_t mtcp, 
		struct mtcp_sender *sender, uint32_t cur_ts, int thresh)
{
	tcp_stream *cur_stream;
	tcp_stream *next, *last;
	int to_ack;
	int cnt = 0;
	int ret;

	/* Send aggregated acks */
	cnt = 0;
	cur_stream = TAILQ_FIRST(&sender->ack_list);
	last = TAILQ_LAST(&sender->ack_list, ack_head);
	while (cur_stream) {
		if (++cnt > thresh)
			break;

		TRACE_LOOP("Inside ack loop. cnt: %u\n", cnt);
		next = TAILQ_NEXT(cur_stream, sndvar->ack_link);

		if (cur_stream->sndvar->on_ack_list) {
			/* this list is only to ack the data packets */
			/* if the ack is not data ack, then it will not process here */
			to_ack = FALSE;
			if (cur_stream->state == TCP_ST_ESTABLISHED || 
					cur_stream->state == TCP_ST_CLOSE_WAIT || 
					cur_stream->state == TCP_ST_FIN_WAIT_1 || 
					cur_stream->state == TCP_ST_FIN_WAIT_2 || 
					cur_stream->state == TCP_ST_TIME_WAIT) {
				/* TIMEWAIT is possible since the ack is queued 
				   at FIN_WAIT_2 */
				tcprb_t *rb;
				if ((rb = cur_stream->rcvvar->rcvbuf) &&
					TCP_SEQ_LEQ(cur_stream->rcv_nxt,
						(cur_stream->rcvvar->irs + 1) + rb->pile
						+ tcprb_cflen(rb))) {
					to_ack = TRUE;
				}
			} else {
				TRACE_DBG("Stream %u (%s): "
						"Try sending ack at not proper state. "
						"seq: %u, ack_seq: %u, on_control_list: %u\n", 
						cur_stream->id, TCPStateToString(cur_stream), 
						cur_stream->snd_nxt, cur_stream->rcv_nxt, 
						cur_stream->sndvar->on_control_list);
#ifdef DUMP_STREAM
				DumpStream(mtcp, cur_stream);
#endif
			}

			if (to_ack) {
				/* send the queued ack packets */
				while (cur_stream->sndvar->ack_cnt > 0) {
					ret = SendTCPPacket(mtcp, cur_stream, 
							cur_ts, TCP_FLAG_ACK, NULL, 0);
					if (ret < 0) {
						/* since there is no available write buffer, break */
						break;
					}
					cur_stream->sndvar->ack_cnt--;
				}

				/* if is_wack is set, send packet to get window advertisement */
				if (cur_stream->sndvar->is_wack) {
					cur_stream->sndvar->is_wack = FALSE;
					ret = SendTCPPacket(mtcp, cur_stream, 
							cur_ts, TCP_FLAG_ACK | TCP_FLAG_WACK, NULL, 0);
					if (ret < 0) {
						/* since there is no available write buffer, break */
						cur_stream->sndvar->is_wack = TRUE;
					}
				}

				if (!(cur_stream->sndvar->ack_cnt || cur_stream->sndvar->is_wack)) {
					cur_stream->sndvar->on_ack_list = FALSE;
					TAILQ_REMOVE(&sender->ack_list, cur_stream, sndvar->ack_link);
					sender->ack_list_cnt--;
				}
			} else {
				cur_stream->sndvar->on_ack_list = FALSE;
				cur_stream->sndvar->ack_cnt = 0;
				cur_stream->sndvar->is_wack = 0;
				TAILQ_REMOVE(&sender->ack_list, cur_stream, sndvar->ack_link);
				sender->ack_list_cnt--;
			}

			if (cur_stream->control_list_waiting) {
				if (!cur_stream->sndvar->on_send_list) {
					cur_stream->control_list_waiting = FALSE;
					AddtoControlList(mtcp, cur_stream, cur_ts);
				}
			}
		} else {
			TRACE_ERROR("Stream %d: not on ack list.\n", cur_stream->id);
			TAILQ_REMOVE(&sender->ack_list, cur_stream, sndvar->ack_link);
			sender->ack_list_cnt--;
#ifdef DUMP_STREAM
			thread_printf(mtcp, mtcp->log_fp, 
					"Stream %u: not on ack list.\n", cur_stream->id);
			DumpStream(mtcp, cur_stream);
#endif
		}

		if (cur_stream == last)
			break;
		cur_stream = next;
	}

	return cnt;
}
/*----------------------------------------------------------------------------*/
inline struct mtcp_sender *
GetSender(mtcp_manager_t mtcp, tcp_stream *cur_stream)
{
	if (cur_stream->sndvar->nif_out < 0) {
		return mtcp->g_sender;

	} else if (cur_stream->sndvar->nif_out >= g_config.mos->netdev_table->num) {
		TRACE_ERROR("(NEVER HAPPEN) Failed to find appropriate sender.\n");
		return NULL;

	} else {
		return mtcp->n_sender[cur_stream->sndvar->nif_out];
	}
}
/*----------------------------------------------------------------------------*/
inline void 
AddtoControlList(mtcp_manager_t mtcp, tcp_stream *cur_stream, uint32_t cur_ts)
{
#if TRY_SEND_BEFORE_QUEUE
	int ret;
	struct mtcp_sender *sender = GetSender(mtcp, cur_stream);
	assert(sender != NULL);

	ret = SendControlPacket(mtcp, cur_stream, cur_ts);
	if (ret < 0) {
#endif
		if (!cur_stream->sndvar->on_control_list) {
			struct mtcp_sender *sender = GetSender(mtcp, cur_stream);
			assert(sender != NULL);

			cur_stream->sndvar->on_control_list = TRUE;
			TAILQ_INSERT_TAIL(&sender->control_list, cur_stream, sndvar->control_link);
			sender->control_list_cnt++;
			//TRACE_DBG("Stream %u: added to control list (cnt: %d)\n", 
			//		cur_stream->id, sender->control_list_cnt);
		}
#if TRY_SEND_BEFORE_QUEUE
	} else {
		if (cur_stream->sndvar->on_control_list) {
			cur_stream->sndvar->on_control_list = FALSE;
			TAILQ_REMOVE(&sender->control_list, cur_stream, sndvar->control_link);
			sender->control_list_cnt--;
		}
	}
#endif
}
/*----------------------------------------------------------------------------*/
inline void 
AddtoSendList(mtcp_manager_t mtcp, tcp_stream *cur_stream)
{
	struct mtcp_sender *sender = GetSender(mtcp, cur_stream);
	assert(sender != NULL);

	if(!cur_stream->sndvar->sndbuf) {
		TRACE_ERROR("[%d] Stream %d: No send buffer available.\n", 
				mtcp->ctx->cpu,
				cur_stream->id);
		assert(0);
		return;
	}

	if (!cur_stream->sndvar->on_send_list) {
		cur_stream->sndvar->on_send_list = TRUE;
		TAILQ_INSERT_TAIL(&sender->send_list, cur_stream, sndvar->send_link);
		sender->send_list_cnt++;
	}
}
/*----------------------------------------------------------------------------*/
inline void 
AddtoACKList(mtcp_manager_t mtcp, tcp_stream *cur_stream)
{
	struct mtcp_sender *sender = GetSender(mtcp, cur_stream);
	assert(sender != NULL);

	if (!cur_stream->sndvar->on_ack_list) {
		cur_stream->sndvar->on_ack_list = TRUE;
		TAILQ_INSERT_TAIL(&sender->ack_list, cur_stream, sndvar->ack_link);
		sender->ack_list_cnt++;
	}
}
/*----------------------------------------------------------------------------*/
inline void 
RemoveFromControlList(mtcp_manager_t mtcp, tcp_stream *cur_stream)
{
	struct mtcp_sender *sender = GetSender(mtcp, cur_stream);
	assert(sender != NULL);

	if (cur_stream->sndvar->on_control_list) {
		cur_stream->sndvar->on_control_list = FALSE;
		TAILQ_REMOVE(&sender->control_list, cur_stream, sndvar->control_link);
		sender->control_list_cnt--;
		//TRACE_DBG("Stream %u: Removed from control list (cnt: %d)\n", 
		//		cur_stream->id, sender->control_list_cnt);
	}
}
/*----------------------------------------------------------------------------*/
inline void 
RemoveFromSendList(mtcp_manager_t mtcp, tcp_stream *cur_stream)
{
	struct mtcp_sender *sender = GetSender(mtcp, cur_stream);
	assert(sender != NULL);

	if (cur_stream->sndvar->on_send_list) {
		cur_stream->sndvar->on_send_list = FALSE;
		TAILQ_REMOVE(&sender->send_list, cur_stream, sndvar->send_link);
		sender->send_list_cnt--;
	}
}
/*----------------------------------------------------------------------------*/
inline void 
RemoveFromACKList(mtcp_manager_t mtcp, tcp_stream *cur_stream)
{
	struct mtcp_sender *sender = GetSender(mtcp, cur_stream);
	assert(sender != NULL);

	if (cur_stream->sndvar->on_ack_list) {
		cur_stream->sndvar->on_ack_list = FALSE;
		TAILQ_REMOVE(&sender->ack_list, cur_stream, sndvar->ack_link);
		sender->ack_list_cnt--;
	}
}
/*----------------------------------------------------------------------------*/
inline void 
EnqueueACK(mtcp_manager_t mtcp, 
		tcp_stream *cur_stream, uint32_t cur_ts, uint8_t opt)
{
	if (!(cur_stream->state == TCP_ST_ESTABLISHED || 
			cur_stream->state == TCP_ST_CLOSE_WAIT || 
			cur_stream->state == TCP_ST_FIN_WAIT_1 || 
			cur_stream->state == TCP_ST_FIN_WAIT_2)) {
		TRACE_DBG("Stream %u: Enqueueing ack at state %s\n", 
				cur_stream->id, TCPStateToString(cur_stream));
	}

	if (opt == ACK_OPT_NOW) {
		if (cur_stream->sndvar->ack_cnt < cur_stream->sndvar->ack_cnt + 1) {
			cur_stream->sndvar->ack_cnt++;
		}
	} else if (opt == ACK_OPT_AGGREGATE) {
		if (cur_stream->sndvar->ack_cnt == 0) {
			cur_stream->sndvar->ack_cnt = 1;
		}
	} else if (opt == ACK_OPT_WACK) {
		cur_stream->sndvar->is_wack = TRUE;
	}
	AddtoACKList(mtcp, cur_stream);
}
/*----------------------------------------------------------------------------*/
inline void 
DumpControlList(mtcp_manager_t mtcp, struct mtcp_sender *sender)
{
	tcp_stream *stream;

	TRACE_DBG("Dumping control list (count: %d):\n", sender->control_list_cnt);
	TAILQ_FOREACH(stream, &sender->control_list, sndvar->control_link) {
		TRACE_DBG("Stream id: %u in control list\n", stream->id);
	}
}
/*----------------------------------------------------------------------------*/
static inline void
UpdatePassiveSendTCPContext_SynSent(struct tcp_stream *cur_stream,
				    struct pkt_ctx *pctx)
{
	assert(cur_stream);
	assert(pctx);

	/* add event */
	if (cur_stream->state < TCP_ST_SYN_SENT) {
		cur_stream->cb_events |= MOS_ON_TCP_STATE_CHANGE;
		cur_stream->cb_events |= MOS_ON_CONN_START;
	}
	/* initialize TCP send variables of send-side stream */
	cur_stream->sndvar->cwnd = 1;
	cur_stream->sndvar->ssthresh = cur_stream->sndvar->mss * 10;
	cur_stream->sndvar->ip_id = htons(pctx->p.iph->id);
	cur_stream->sndvar->iss = pctx->p.seq;
	cur_stream->snd_nxt = pctx->p.seq + 1;
	cur_stream->state = TCP_ST_SYN_SENT;
	cur_stream->last_active_ts = pctx->p.cur_ts;

	/* receive-side conn start event can also be tagged here */
	/* blocked since tcp_in.c takes care of this.. */
	/* cur_stream->pair_stream->cb_events |= MOS_ON_CONN_START; */
}
/*----------------------------------------------------------------------------*/
/**
 * Called (when monitoring mode is enabled).. for every incoming packet from the 
 * NIC.
 */
void
UpdatePassiveSendTCPContext(mtcp_manager_t mtcp, struct tcp_stream *cur_stream, 
			    struct pkt_ctx *pctx)
{
	struct tcphdr *tcph;

	assert(cur_stream);
	tcph = pctx->p.tcph;

	/* if it is a new TCP stream from client */
	if (tcph->syn && !tcph->ack && cur_stream->state <= TCP_ST_SYN_SENT) {
		TRACE_STATE("Stream %d: %s\n", 
			    cur_stream->id, TCPStateToString(cur_stream));
		UpdatePassiveSendTCPContext_SynSent(cur_stream, pctx);
		AddtoTimeoutList(mtcp, cur_stream);
		return;
	}
	
	if (tcph->ack) {
		cur_stream->sndvar->ts_lastack_sent = pctx->p.cur_ts;
		cur_stream->last_active_ts = pctx->p.cur_ts;
	}

	cur_stream->snd_nxt = pctx->p.seq + pctx->p.payloadlen;

	/* test for reset packet */
	if (tcph->rst) {
		cur_stream->have_reset = TRUE;
		/* test for reset packet */
		cur_stream->state = TCP_ST_CLOSED_RSVD;
		cur_stream->cb_events |= MOS_ON_TCP_STATE_CHANGE;
		TRACE_STATE("Stream %d: %s\n", 
				cur_stream->id, 
				TCPStateToString(cur_stream));
		return;
	}

	/*
	 * for all others, state transitioning is based on 
	 * current tcp_stream state 
	 */
	switch (cur_stream->state) {
	case TCP_ST_SYN_SENT:
		/* control should not come here */
		/* UpdatePassiveReceiveTCPContext() should take care of this */
#ifdef BE_RESILIENT_TO_PACKET_DROP
		if (tcph->ack && TCP_SEQ_GT(pctx->p.seq, cur_stream->sndvar->iss)) {
			cur_stream->state = TCP_ST_ESTABLISHED;
			cur_stream->cb_events |= MOS_ON_TCP_STATE_CHANGE;
			cur_stream->snd_nxt = pctx->p.seq;
			cur_stream->rcv_nxt = pctx->p.ack_seq;
			goto __Handle_TCP_ST_ESTABLISHED;
		}
#endif
		break;
	case TCP_ST_SYN_RCVD:
		if (!tcph->ack)
			break;

		if (tcph->syn) {
			cur_stream->sndvar->iss = pctx->p.seq;
			cur_stream->snd_nxt = cur_stream->sndvar->iss + 1;
			TRACE_DBG("Stream %d (TCP_ST_SYN_RCVD): "
				  "setting seq: %u = iss\n", 
				  cur_stream->id, pctx->p.seq);
		}
#ifdef BE_RESILIENT_TO_PACKET_DROP
		else {
			cur_stream->state = TCP_ST_ESTABLISHED;
			cur_stream->cb_events |= MOS_ON_TCP_STATE_CHANGE;
			cur_stream->snd_nxt = pctx->p.seq;
			cur_stream->rcv_nxt = pctx->p.ack_seq;
			goto __Handle_TCP_ST_ESTABLISHED;
		}
#endif
		TRACE_STATE("Stream %d: %s\n", 
			    cur_stream->id,
			    TCPStateToString(cur_stream));
		break;
	case TCP_ST_ESTABLISHED:
#ifdef BE_RESILIENT_TO_PACKET_DROP
__Handle_TCP_ST_ESTABLISHED:
#endif
		/* if application decides to close, fin pkt is sent */
#ifdef BE_RESILIENT_TO_PACKET_DROP
		if (tcph->ack && TCP_SEQ_GT(ntohl(tcph->ack_seq), cur_stream->rcv_nxt))
		{
			RAISE_DEBUG_EVENT(mtcp, cur_stream,
					"Move rcv_nxt from %u to %u.\n",
					cur_stream->rcv_nxt, ntohl(tcph->ack_seq));
			cur_stream->rcv_nxt = ntohl(tcph->ack_seq);
		}
#endif
		if (tcph->fin) {
			cur_stream->state = TCP_ST_FIN_WAIT_1;
			cur_stream->cb_events |= MOS_ON_TCP_STATE_CHANGE;
			cur_stream->sndvar->fss = pctx->p.seq + pctx->p.payloadlen;
			cur_stream->sndvar->is_fin_sent = TRUE;
			cur_stream->snd_nxt++;
			TRACE_STATE("Stream %d: %s\n", 
				    cur_stream->id,
				    TCPStateToString(cur_stream));
		} else {
			/* creating tcp send buffer still pending.. */
			/* do we need peek for send buffer? */
		}
		break;
	case TCP_ST_CLOSE_WAIT:
		/* if application decides to close, fin pkt is sent */
#ifdef BE_RESILIENT_TO_PACKET_DROP
		if (tcph->ack && TCP_SEQ_GT(ntohl(tcph->ack_seq), cur_stream->rcv_nxt))
		{
			RAISE_DEBUG_EVENT(mtcp, cur_stream,
					"Move rcv_nxt from %u to %u.\n",
					cur_stream->rcv_nxt, ntohl(tcph->ack_seq));
			cur_stream->rcv_nxt = ntohl(tcph->ack_seq);
		}
#endif
		if (tcph->fin) {
			cur_stream->sndvar->fss = pctx->p.seq + pctx->p.payloadlen;
			cur_stream->sndvar->is_fin_sent = TRUE;
			cur_stream->snd_nxt++;
			cur_stream->state = TCP_ST_LAST_ACK;
			cur_stream->cb_events |= MOS_ON_TCP_STATE_CHANGE;
			TRACE_STATE("Stream %d: %s\n", 
				    cur_stream->id,
				    TCPStateToString(cur_stream));
		} else if (tcph->ack) {
			TRACE_STATE("Stream %d: %s\n", 
				    cur_stream->id,
				    TCPStateToString(cur_stream));
		}
		break;
	case TCP_ST_LAST_ACK:
		/* control should not come here */
		/* UpdatePassiveReceiveTCPContext() should take care of this */
		break;
	case TCP_ST_FIN_WAIT_1:
		/* control should not come here */
		/* UpdatePassiveReceiveTCPContext() should take care of this */
		break;
	case TCP_ST_FIN_WAIT_2:
		/* control should not come here */
		/* UpdatePassiveReceiveTCPContext() should take care of this */
		break;
	case TCP_ST_CLOSING:
		/* control should not come here */
		/* UpdatePassiveReceiveTCPContext() should take care of this */
		break;
	case TCP_ST_TIME_WAIT:
		/* control may come here but... */
		/* UpdatePassiveReceiveTCPContext() should take care of this */
		if (tcph->ack) {
			TRACE_STATE("Stream %d: %s\n", 
				    cur_stream->id,
				    TCPStateToString(cur_stream));
		}
		break;
	case TCP_ST_CLOSED:
	case TCP_ST_CLOSED_RSVD:
		/* Waiting to be destroyed */
		break;
	default:
		TRACE_DBG("This should not happen.. Error state: %s reached!\n"
			  "tcph->syn: %d, tcph->ack: %d\n",
			  TCPStateToString(cur_stream), pctx->p.tcph->syn,
			  pctx->p.tcph->ack);
		assert(0);
		/* This will be enabled once passiverecvcontext is completed */
		/*exit(EXIT_FAILURE);*/
	}

	UNUSED(mtcp);
	return;
}
/*----------------------------------------------------------------------------*/
void
PostSendTCPAction(mtcp_manager_t mtcp, struct pkt_ctx *pctx, 
		  struct tcp_stream *recvside_stream, 
		  struct tcp_stream *sendside_stream)
{
	/* this is empty for the time being */
}
/*----------------------------------------------------------------------------*/
