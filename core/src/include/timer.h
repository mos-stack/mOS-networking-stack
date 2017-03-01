#ifndef __TIMER_H_
#define __TIMER_H_
/*----------------------------------------------------------------------------*/
#include "mtcp.h"
#include "tcp_stream.h"
#include <sys/time.h>
/*----------------------------------------------------------------------------*/
#define RTO_HASH 2048
/*----------------------------------------------------------------------------*/
#define TIMEVAL_ADD(a, b) \
do { (a)->tv_sec += (b)->tv_sec; \
	if (((a)->tv_usec += (b)->tv_usec) > 1000000) { \
		(a)->tv_sec++; (a)->tv_usec -= 1000000; } \
} while (0)

#define TIMEVAL_LT(a, b)			\
	timercmp(a, b, <)
/*----------------------------------------------------------------------------*/
struct timer {
	int id;
	struct timeval exp; /* expiration time */
	callback_t cb;

	TAILQ_ENTRY(timer) timer_link;
};

struct rto_hashstore 
{
	uint32_t rto_now_idx; // pointing the hs_table_s index
	uint32_t rto_now_ts; // 
	
	TAILQ_HEAD(rto_head , tcp_stream) rto_list[RTO_HASH+1];
};
/*----------------------------------------------------------------------------*/
struct rto_hashstore* 
InitRTOHashstore();

extern inline void 
AddtoRTOList(mtcp_manager_t mtcp, tcp_stream *cur_stream);

extern inline void 
RemoveFromRTOList(mtcp_manager_t mtcp, tcp_stream *cur_stream);

extern inline void 
AddtoTimewaitList(mtcp_manager_t mtcp, tcp_stream *cur_stream, uint32_t cur_ts);

extern inline void 
RemoveFromTimewaitList(mtcp_manager_t mtcp, tcp_stream *cur_stream);

extern inline void 
AddtoTimeoutList(mtcp_manager_t mtcp, tcp_stream *cur_stream);

extern inline void 
RemoveFromTimeoutList(mtcp_manager_t mtcp, tcp_stream *cur_stream);

extern inline void 
UpdateTimeoutList(mtcp_manager_t mtcp, tcp_stream *cur_stream);

extern inline void
UpdateRetransmissionTimer(mtcp_manager_t mtcp, 
		tcp_stream *cur_stream, uint32_t cur_ts);

void
CheckRtmTimeout(mtcp_manager_t mtcp, uint32_t cur_ts, int thresh);

void 
CheckTimewaitExpire(mtcp_manager_t mtcp, uint32_t cur_ts, int thresh);

void 
CheckConnectionTimeout(mtcp_manager_t mtcp, uint32_t cur_ts, int thresh);

void
DelTimer(mtcp_manager_t mtcp, struct timer *timer);
/*----------------------------------------------------------------------------*/
#endif /* __TIMER_H_ */
