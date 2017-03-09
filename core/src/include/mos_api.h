#ifndef __MOS_API_H_
#define __MOS_API_H_

#ifdef DARWIN
#include <netinet/tcp.h>
#include <netinet/if_ether.h>
#else
#include <linux/tcp.h>
#include <linux/if_ether.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <stddef.h> /* for offsetof */
#include "mtcp_epoll.h"
#include <stdbool.h>

#ifndef __MTCP_MANAGER
#define __MTCP_MANAGER
typedef struct mtcp_manager * mtcp_manager_t;
#endif
#ifndef __SOCKET_MAP
#define __SOCKET_MAP
typedef struct socket_map * socket_map_t;
#endif

/** Available hooking points */
enum mtcp_hook_point
{
	/* NOTE: The value of hooking points should not overlap with any of
	 * mos_event_types */

	/** Very first hooking point of incoming packet even before flow
	 * identification*/
	MOS_NULL	= (1 << 29),
	/** Hooking point before TCP receiver */
	MOS_HK_RCV     = (1 << 30),
	/** Hooking point after TCP sender */
	MOS_HK_SND	= (1 << 31),
};

/** Built-in events provided by mOS */
enum mos_event_type
{
	/** invalid event */
	MOS_NULL_EVENT          = (0),
	/* mos-defined tcp build-in events */
	/** A packet is coming in. */
	MOS_ON_PKT_IN 		= (0x1<<0),
	/** A packet is going out. */
	/* THIS EVENT IS NOW DEPRECATED (USED ONLY FOR DEBUGGING) */
	MOS_ON_PKT_OUT 		= (0x1<<1),
	/** SYN packet as seen by the monitor
	 *  client side: activated when the client state is set to SYN_SENT
	 *  server side: activated when the server state is set to SYN_RCVD
	 *
	 *  Retransmitted SYN packets don't activate this event.
	 */
	MOS_ON_CONN_START	= (0x1<<2),
	/** 3-way handshake is finished.
	 * server side: ACK is coming in as a response of SYNACK.
	 * client side: SYNACK is coming in as a response of SYN. */
	/* THIS EVENT IS NOW DEPRECATED */
	MOS_ON_CONN_SETUP	= (0x1<<3),
	/** New data is now readable.
	 * This event is available in only MOS_NULL hook point.
	 * mOS raises this event only once while batched packet processing. */
	MOS_ON_CONN_NEW_DATA	= (0x1<<4),
	/** Abnormal behavior is detected.
	 * NOTE: This is not fully implemented yet. */
	MOS_ON_ERROR 		= (0x1<<5),
	/** No packet is seen for a long time.
	 * This is implemented as mtcp_cb_settimer()
	 */
	MOS_ON_TIMEOUT 		= (0x1<<6),
	/** TCP state is being changed. */
	MOS_ON_TCP_STATE_CHANGE	= (0x1<<7),
	/** A packet is not SYN and has no identified flow. */
	MOS_ON_ORPHAN		= (0x1<<8),
	/** Retransmission is detected */
	MOS_ON_REXMIT           = (0x1<<9),
	/** A flow is about to be destroyed.
	 * 4-way handshake, RST packet or timeout could be the reason.
	 * NOTE: In current implementation, mOS raises this event while destroying
	 * `struct tcp_stream`. There is possibility of false-positive especially
	 * when mOS is running out of memory. */
	MOS_ON_CONN_END		= (0x1<<10),

	/** This event is for debugging. We can easily mute this later. */
	MOS_ON_DEBUG_MESSAGE  = (0x1<<11),
};

#if 0
/* This may go away in future revisions */
typedef union event_data {
	uint32_t u32;
	uint64_t u64;
	void *ptr;
} event_data_t;
#endif

/* Macros for updating packet context */
#define MOS_ETH_HDR		(1 << 0)
#define MOS_IP_HDR		(1 << 1)
#define MOS_TCP_HDR		(1 << 2)
#define MOS_TCP_PAYLOAD		(1 << 3)
#define MOS_UPDATE_IP_CHKSUM	(1 << 4)
#define MOS_UPDATE_TCP_CHKSUM	(1 << 5)
#define MOS_DROP		(1 << 6)
#define MOS_OVERWRITE		(1 << 7)
#define MOS_CHOMP		(1 << 8)
#define MOS_INSERT		(1 << 9)

/**
 * struct pkt_info is the struct that is actually
 * exposed to the monitor application.
 *
 * NOTE: When you retrieve the packet information using mtcp_getlastpkt()
 * via MOS_SOCK_MONITOR_RAW socket, you can only use up to L3 information.
 * (cur_ts, eth_len, ip_len, ethh, iph)
 */
struct pkt_info {
	uint32_t      cur_ts;    /**< packet receiving time (read-only:ro) */
	int8_t        in_ifidx;  /**< input interface (ro) */
	
	/* ETH */
	uint16_t      eth_len;
	
	/* IP */
	uint16_t      ip_len;
	
	/* TCP */
	uint64_t      offset;    /**< TCP ring buffer offset */
	uint16_t      payloadlen;
	uint32_t      seq;
	uint32_t      ack_seq;
	uint16_t      window;
	
	/* ~~ 28 byte boundary ~~ */
	
	/*
	 * CAUTION!!!
	 * It is extremely critical that the last 5 fields (ethh .. frame)
	 * are always placed at the end of the definition. MOS relies on
	 * this specific arrangement when it is creating a new instantiation
	 * of pctx during mtcp_getlastpkt() invocation.
	 */
	struct ethhdr *ethh;
	struct iphdr  *iph;
	struct tcphdr *tcph;
	uint8_t       *payload;
};

/** 
 * PACKET CONTEXT is the packet structure that goes through
 * the mOS core...
 */
struct pkt_ctx {
	struct pkt_info  p;

	int8_t        direction; /**< where does this packet originate from? (ro)*/
	uint8_t       forward;   /**< 0: drop, 1: forward to out_ifidx (rw) */
	int8_t        out_ifidx; /**< output interface (rw) */
	int8_t        batch_index; /**< index of packet in the rx batch */
	/* ~~ 64 byte boundary ~~ */
};
#define PKT_INFO_LEN		offsetof(struct pkt_info, ethh)

/* 
 * Sequence number change structure. 
 * Used for MOS_SEQ_REMAP.
 */
typedef struct {
	int64_t seq_off;	/* the amount of sequence number drift */
	int side;		/* which side does this sequence number change apply to? */
	uint32_t base_seq;	/* seq # of the flow where the actual sequence # translation starts */
} seq_remap_info;

typedef struct filter_arg {
	void *arg;
	size_t len;
} filter_arg_t;

/**
 * The available level number in the POSIX library for sockets is 
 * on SOL_SOCKET
 */
#ifndef SOL_SOCKET
/* Level number for (get/set)sockopt() to apply to socket itself. */
#define SOL_SOCKET 		0xffff	/* options for socket level */
#endif
#define SOL_MONSOCKET		0xfffe	/* MOS monitor socket level */

/**
 * MOS monitor socket option names (and values)
 * This will contain options pertaining to monitor stream sockets
 * See mtcp_getsockopt() and mtcp_setsockopt() the mtcp_api.h file.
 */
enum mos_socket_opts {
	MOS_FRAGINFO_CLIBUF	= 0x01,
	MOS_FRAGINFO_SVRBUF	= 0x02,
	MOS_INFO_CLIBUF		= 0x03,
	MOS_INFO_SVRBUF		= 0x04,
	MOS_TCP_STATE_CLI	= 0x05,
	MOS_TCP_STATE_SVR	= 0x06,
	MOS_CLIBUF  		= 0x09,
	MOS_SVRBUF  		= 0x0a,
	MOS_STOP_MON		= 0x0c,
	MOS_CLIOVERLAP		= 0x0f,
	MOS_SVROVERLAP		= 0x10,

	MOS_TIMESTAMP		= 0x07, /* supressed (not used) */
	MOS_SEQ_REMAP		= 0x0b, /* supressed (not used) */
	MOS_FRAG_CLIBUF   	= 0x0d, /* supressed (not used) */
	MOS_FRAG_SVRBUF   	= 0x0e, /* supressed (not used) */

};

/**
 * MOS tcp buf info structure.
 * Used by the monitor application to retreive
 * tcp_stream-related info. Usually called via
 * mtcp_getsockopt() function
 */
struct tcp_buf_info {
	/** The initial TCP sequence number of TCP ring buffer. */
	uint32_t tcpbi_init_seq;
	/** TCP sequence number of the 'last byte of payload that has 
	 * already been read by the end application' (applies in the case 
	 * of embedded monitor setup)
	 */
	uint32_t tcpbi_last_byte_read;
	/** TCP sequence number of the 'last byte of the payload that 
	 * is currently buffered and needs to be read by the end 
	 * application' (applies in the case of embedded monitor setup).
	 * 
	 * In case of standalone monitors, tcpbi_last_byte_read = 
	 * tcpbi_next_byte_expected
	 */
	uint32_t tcpbi_next_byte_expected;
	/** TCP sequence number of the 'last byte of the payload that
	 * is currently stored' in the TCP ring buffer. This value
	 * may be greater than tcpbi_next_byte_expected if packets
	 * arrive out of order.
	 */
	uint32_t tcpbi_last_byte_received;
};

/** Structure to expose TCP ring buffer's fragment information. */
struct tcp_ring_fragment {
	uint64_t offset;
	uint32_t len;
};

/**
 * mOS tcp stream states.
 * used by the monitor application to retreive
 * tcp_stream-state info. Usually called via
 * getsockopt() function
 */
enum tcpstate
{
	TCP_CLOSED		= 0,
	TCP_LISTEN		= 1,
	TCP_SYN_SENT		= 2,
	TCP_SYN_RCVD		= 3,
	TCP_ESTABLISHED		= 4,
	TCP_FIN_WAIT_1		= 5,
	TCP_FIN_WAIT_2		= 6,
	TCP_CLOSE_WAIT		= 7,
	TCP_CLOSING		= 8,
	TCP_LAST_ACK		= 9,
	TCP_TIME_WAIT		= 10
};

/** mOS segment overlapping policies */
enum {
	MOS_OVERLAP_POLICY_FIRST=0,
	MOS_OVERLAP_POLICY_LAST,
	MOS_OVERLAP_CNT
};

/** Definition of event type */
typedef uint64_t event_t;

/** Definition of monitor side */
enum {MOS_SIDE_CLI=0, MOS_SIDE_SVR, MOS_SIDE_BOTH};

/* mos callback/filter function type definition */
/** Prototype of callback function */
typedef void (*callback_t)(mctx_t mctx, int sock, int side,
			 event_t event, filter_arg_t *arg);
/** Prototype of UDE's filter function */
typedef bool (*filter_t)(mctx_t mctx, int sock, int side,
		       event_t event, filter_arg_t *arg);

/*----------------------------------------------------------------------------*/
/* Definition of monitor_filter type */
union monitor_filter {
	/** For MOS_SOCK_MONITOR_RAW type socket **/
	char *raw_pkt_filter;
	/** For MOS_SOCK_MONITOR_STREAM type socket **/
	struct {
		char *stream_syn_filter;
		char *stream_orphan_filter;
	};	
};
typedef union monitor_filter *monitor_filter_t;

/* Assign an address range (specified by ft) to monitor via sock
 *
 * (1) If sock is MOS_SOCK_MONITOR_RAW type, ft.raw_pkt_filter is applied to
 *     every packet coming in.
 * (2) If sock is MOS_SOCK_MONITOR_STREAM type,
 *     ft.stream_syn_filter is applied to the first SYN pkt of the flow.
 *     (The succeeding packets of that flow will bypass the filter operation.)
 *     ft.stream_orphan_filter is applied to the pkts that don't belong to any
 *     of the existing TCP streams which are being monitored.
 *     (e.g., non-SYN pkt with no identified flow)
 * [*] ft.stream_syn_filter and ft.stream_orphan_filter should be consisted
 *     only of the following keywords:
 *     - 'tcp, 'host', 'src', 'dst', 'net', 'mask', 'port', 'portrange'
 *     - 'and', 'or', '&', '|'
 *
 * @param [in] mctx: mtcp context
 * @param [in] sock: socket id (should be MOS_SOCK_MONITOR_RAW
 *                   or MOS_SOCK_MONITOR_STREAM type)
 * @param [in] cf: Describe a set of connections to accept
 *                 in a BPF (Berkerley Packet Filter) format
 *                 NULL if you want to monitor any packet
 * @return zero on success, -1 on error
 */
int
mtcp_bind_monitor_filter(mctx_t mctx, int sock, monitor_filter_t ft);
/*----------------------------------------------------------------------------*/

/** Register a callback function in hook_point
 * @param [in] mctx: mtcp context
 * @param [in] sock: socket id
 * @param [in] event: event id
 * @param [in] hook_point: MOS_HK_RCV, MOS_HK_SND, MOS_DONTCARE
 * @param [in] cb: callback fucntion
 * @return zero on success, -1 on error
 *
 * (both for packet-level and flow-level) for events in hook_point
 */
int
mtcp_register_callback(mctx_t mctx, int sock, event_t event,
		                       int hook_point, callback_t cb);

/** Remove registered callback functions
 * @param [in] mctx: mtcp context
 * @param [in] sock: socket id
 * @param [in] event: event id
 * @param [in] hook_point: MOS_HK_RCV, MOS_HK_SND, MOS_NULL
 * @return zero on success, -1 on error
 *
 * (both for packet-level and flow-level) for events in hook_point
 */
//int
//mtcp_unregister_callback(mctx_t mctx, int sock, event_t event,
//		                       int hook_point);

/** Allocate a child event
 * @param [in] event: event id
 * @return new event id on success, 0 on error
 */
event_t
mtcp_alloc_event(event_t event);

/** Define a user-defined event function
 * @param [in] event: event id
 * @param [in] filter: filter fucntion for new event
 * @param [in] arg: a filter argument to be delivered to the filter
 * @return new event id on success, 0 on error
 *
 * (both for packet-level and flow-level)
 */
event_t
mtcp_define_event(event_t event, filter_t filter, struct filter_arg *arg);

/** Raise a event
 * @param [in] mctx: mtcp context
 * @param [in] event: event id
 * @return 0 on success, -1 on error
 */
int
mtcp_raise_event(mctx_t mctx, event_t event);

/*
 * Callback only functions
 */

/** Set user-level context
 * (e.g., to store any per-flow user-defined meatadata)
 * @param [in] mctx: mtcp context
 * @param [in] sock: the monitor socket id
 * @param [in] uctx: user-level context
 */
void
mtcp_set_uctx(mctx_t mctx, int sock, void *uctx);

/** Get user-level context
 * (e.g., to retrieve user-defined metadata stored in mtcp_set_uctx())
 * @param [in] mctx: mtcp context
 * @param [in] sock: the monitor socket id
 * @return user-level context for input flow_ocntext
 */
void *
mtcp_get_uctx(mctx_t mctx, int sock);

/** Peeking bytestream from flow_context
 * @param [in] mctx: mtcp context
 * @param [in] sock: monitoring stream socket id
 * @param [in] side: side of monitoring (client side, server side or both)
 * @param [in] buf: buffer for read byte stream
 * @param [in] len: requested length
 *
 * It will return the number of bytes actually read.
 * It will return -1 if there is an error
*/
ssize_t
mtcp_peek(mctx_t mctx, int sock, int side,
	     char *buf, size_t len);

/**
 * The mtcp_ppeek() function reads up to count bytes from the TCP ring
 * buffer of the monitor socket sock in mctx into buf, starting from
 * the TCP sequence number seq_num.
 * Note that seq_num can point the data in the fragmented buffer list
 * of the TCP ring buffer. If there is no received byte with TCP sequence
 * number seq_num in the TCP ring buffer, it returns error. If there are
 * received bytes starting from seq_num, count is set to be the number
 * of bytes read from the buffer. After mtcp_ppeek(), the data in the
 * TCP ring buffer will not be flushed, and the monitor offset used by
 * mtcp_peek() is not changed.
 *
 * @param [in] mctx: mtcp context
 * @param [in] sock: monitoring stream socket id
 * @param [in] side: side of monitoring (client side, server side or both)
 * @param [in] buf: buffer for read byte stream
 * @param [in] count: No. of bytes to be read
 * @param [in] seq_num: byte offset of the TCP bytestream (absolute offset: offset 0 = init_seq_num)
 * @return # of bytes actually read on success, -1 for error
 */
ssize_t mtcp_ppeek(mctx_t mctx, int sock, int side, 
			  char *buf, size_t count, uint64_t off);

/* Use this macro to copy packets when mtcp_getlastpkt is called */
#define MTCP_CB_GETCURPKT_CREATE_COPY

/** Get current packet of mtcp context
 * @param [in] mctx: mTCP/mOS context
 * @param [in] sock: monitoring stream socket id
 * @param [in] side: side of monitoring
 *                   (MOS_NULL for MOS_SOCK_MONITOR_RAW socket)
 * @param [in] p: ptr to packet info ptr
 * (only L2-L3 information is available for MOS_SOCK_MONITOR_RAW socket)
 * @return 0 on success, -1 on failure
 * This is useful for running callback-only applications
 */
int
mtcp_getlastpkt(mctx_t mctx, int sock, int side, struct pkt_info *p);

/** Register user's custom timer
 * @param [in] mctx: mtcp context
 * @param [in] id: timer id
 * @param [in] timeout: timeout length
 * @param [in] cb: callback function
 */
int
mtcp_settimer(mctx_t mctx, int id, struct timeval *timeout, callback_t cb);

/** A sibling function to mtcp_settimer that returns
 * the current timestamp of the machine in microseconds.
 * This avoids the monitor application to call current
 * time getter functions (e.g. gettimeofday) that may
 * incur overhead.
 *
 * @param [in] mctx: mtcp context
 * Returns timestamp on success, 0 on failure.
 */
uint32_t
mtcp_cb_get_ts(mctx_t mctx);

/** Pause mtcp application context since it is not running anything
 * @param [in] mctx: mtcp context
 *
 * This is useful for running callback-only applications
 */
void
mtcp_app_join(mctx_t mctx);

/** Get IP addrs/ports for both sides. 
 * (Server IP/port in 0th element) (Client IP/port in 1st element)
 * Should only be called with MOS_SOCK_MONITOR_STREAM_ACTIVE socket
 * _NOTE_: Code is currently not set for MOS_SOCK_STREAM!!!
 * Returns 0 on success, -1 on failure
 */
int
mtcp_getpeername(mctx_t mctx, int sock, struct sockaddr *saddr, socklen_t *addrlen, int side);

/**
 * Updates the Ethernet frame at a given offset across
 * datalen bytes.
 *
 * @param [in] mctx: mtcp context
 * @param [in] sock: monitoring socket
 * @param [in] side: monitoring side
 *                   (MOS_NULL for MOS_SOCK_MONITOR_RAW socket)
 * @param [in] offset: the offset from where the data needs to be written
 * @param [in] data: the data buffer that needs to be written
 * @param [in] datalen: the length of data that needs to be written
 * @param [in] option: disjunction of MOS_ETH_HDR, MOS_IP_HDR, MOS_TCP_HDR,
 *			MOS_TCP_PAYLOAD, MOS_DROP_PKT, MOS_UPDATE_TCP_CHKSUM,
 *			MOS_UPDATE_IP_CHKSUM
 * @return Returns 0 on success, -1 on failure
 *
 * If you want to chomp/insert something in the payload:
 * (i) first update the ip header to adjust iph->tot_len field; (MOS_OVERWRITE)
 * (ii) then update the tcp payload accordingly (MOS_CHOMP or MOS_INSERT)
 *
 * MOS_DROP, MOS_OVERWRITE, MOS_CHOMP and MOS_INSERT are mutually 
 * exclusive operations
 */
int
mtcp_setlastpkt(mctx_t mctx, int sock, int side, off_t offset,
		byte *data, uint16_t datalen, int option);

/** Drop current packet (don't forward it to the peer node)
 * @param [in] mctx: mtcp context
 *
 * This is useful for running callback-only applications
 * This function is now deprecated...
 */
//int
//mtcp_cb_dropcurpkt(mctx_t mctx);

/* Reset the connection (send RST to both sides)
 * (This API will be updated after discussion.)
 */
int
mtcp_reset_conn(mctx_t mctx, int sock);

int
mtcp_set_debug_string(mtcp_manager_t mtcp, const char *fmt, ...);

int
mtcp_get_debug_string(mctx_t mctx, char *buf, int len);

/**************************************************************************/
/** Send a TCP packet of struct pkt_info
 * @param [in] mctx: mTCP/mOS context
 * @param [in] sock: monitoring stream socket id
 * @param [in] pkt: ptr to packet info (e.g., captured by mtcp_getlastpkt)
 * @return 0 on success, -1 on failure
 * (NOTE: this function supports only TCP packet for now.
 *  we will add the support for any ethernet packets when required)
 */ 
int
mtcp_sendpkt(mctx_t mctx, int sock, const struct pkt_info *pkt);

/**************************************************************************/

#endif /* __MOS_API_H_ */
