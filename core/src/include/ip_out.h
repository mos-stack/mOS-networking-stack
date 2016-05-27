#ifndef __IP_OUT_H_
#define __IP_OUT_H_

#include <stdint.h>
#include "tcp_stream.h"
#include "mos_api.h"

extern inline int 
GetOutputInterface(uint32_t daddr);

void
ForwardIPPacket(mtcp_manager_t mtcp, struct pkt_ctx *pctx);

void
ForwardIPv4Packet(mtcp_manager_t mtcp, int nif_in, char *buf, int len);

uint8_t *
IPOutputStandalone(struct mtcp_manager *mtcp, 
		uint16_t ip_id, uint32_t saddr, uint32_t daddr, uint16_t tcplen,
		struct pkt_ctx *pctx, uint32_t cur_ts);

uint8_t *
IPOutput(struct mtcp_manager *mtcp, tcp_stream *stream, uint16_t tcplen, 
			struct pkt_ctx *pctx, uint32_t cur_ts);

#endif /* __IP_OUT_H_ */
