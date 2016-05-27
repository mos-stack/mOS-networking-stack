#ifndef __MTCP_EPOLL_H_
#define __MTCP_EPOLL_H_

#include "mtcp_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** `mtcp_epoll_ctl()` operations */
enum mtcp_epoll_op
{
	MOS_EPOLL_CTL_ADD = 1,
	MOS_EPOLL_CTL_DEL = 2,
	MOS_EPOLL_CTL_MOD = 3,
};

/** Event types for mtcp epoll */
enum epoll_event_type
{
	/* mtcp-defined epoll events */
	MOS_EPOLLNONE		= (0x1<<0),
	MOS_EPOLLIN			= (0x1<<1),
	MOS_EPOLLPRI		= (0x1<<2),
	MOS_EPOLLOUT		= (0x1<<3),
	MOS_EPOLLRDNORM		= (0x1<<4),
	MOS_EPOLLRDBAND		= (0x1<<5),
	MOS_EPOLLWRNORM		= (0x1<<6),
	MOS_EPOLLWRBAND		= (0x1<<7),
	MOS_EPOLLMSG		= (0x1<<8),
	MOS_EPOLLERR		= (0x1<<9),
	MOS_EPOLLHUP		= (0x1<<10),
	MOS_EPOLLRDHUP 		= (0x1<<11),

	/* mtcp-defined epoll events */
	MOS_EPOLLONESHOT	= (0x1 << 30),
	MOS_EPOLLET			= (0x1 << 31)
};

/** Control messages from state update module to react module
 * XXX: Is this only for internal use? */
enum mtcp_action
{
	/* mtcp action */
	MOS_ACT_SEND_DATA 	=	(0x01<<1),
	MOS_ACT_SEND_ACK_NOW 	=	(0x01<<2),
	MOS_ACT_SEND_ACK_AGG 	=	(0x01<<3),
	MOS_ACT_SEND_CONTROL 	=	(0x01<<4),
	MOS_ACT_SEND_RST	=	(0x01<<5),
	MOS_ACT_DESTROY	 	=	(0x01<<6),
	/* only used by monitoring socket */
	MOS_ACT_READ_DATA	=	(0x01<<7),
	MOS_ACT_CNT
};

/** epoll data structure */
typedef union mtcp_epoll_data
{
	void *ptr;
	int sock;
	uint32_t u32;
	uint64_t u64;
} mtcp_epoll_data_t;

/** epoll data structure */
struct mtcp_epoll_event
{
	uint64_t events;
	mtcp_epoll_data_t data;
};

/** Create new epoll descriptor.
 * @param [in] mctx: mtcp context
 * @param [in] size: backlog size
 * @return new epoll descriptor on success, -1 on error
 *
 * Same with `epoll_create()`
 */
int
mtcp_epoll_create(mctx_t mctx, int size);

/** Control epoll.
 * @param [in] mctx: mtcp context
 * @param [in] epid: epoll descriptor
 * @param [in] op: operation
 *                 (MOS_EPOLL_CTL_ADD, MOS_EPOLL_CTL_DEL, MOS_EPOLL_CTL_MOD)
 * @param [in] sock: socket ID
 * @param [in] event: event to be controlled
 * @return zero on success, -1 on error
 *
 * Same with `epoll_ctl()`
 */
int
mtcp_epoll_ctl(mctx_t mctx, int epid,
		int op, int sock, struct mtcp_epoll_event *event);

/** Wait for events.
 * @param [in] mctx: mtcp context
 * @param [in] epid: epoll descriptor
 * @param [in] events: occured events
 * @param [in] maxevents: maximum number of events to read
 * @param [in] timeout: timeout
 * @return number of events occured, -1 on error
 *
 * Same with `epoll_wait()`
 */
int
mtcp_epoll_wait(mctx_t mctx, int epid,
		struct mtcp_epoll_event *events, int maxevents, int timeout);

/** Convert built-in event ID to string
 * @param [in] event: built-in event ID
 * @return string of the event name
 */
char *
EventToString(uint32_t event);

#ifdef __cplusplus
};
#endif

#endif /* __MTCP_EPOLL_H_ */
