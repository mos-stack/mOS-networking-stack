#ifndef __TCP_H_
#define __TCP_H_

#include "tcp_stream.h"
#include "mos_api.h"
#include "mtcp.h"

extern inline void
FillPacketContextTCPInfo(struct pkt_ctx *pctx, struct tcphdr * tcph);

extern inline void 
FillFlowContext(struct pkt_ctx *pctx, 
		tcp_stream *cur_stream, int cpu);

extern inline void
HandleSingleCallback(mtcp_manager_t mtcp, uint32_t hooking_point, 
		struct pkt_ctx *pctx, uint64_t events);

int
ProcessInTCPPacket(mtcp_manager_t mtcp, struct pkt_ctx *pctx);

void 
UpdateMonitor(mtcp_manager_t mtcp, struct tcp_stream *sendside_stream, 
			struct tcp_stream *recvside_stream, struct pkt_ctx *pctx,
			bool is_pkt_reception);
#endif
