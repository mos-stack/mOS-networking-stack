#ifndef __ETH_OUT_H_
#define __ETH_OUT_H_

#include <stdint.h>

#include "mtcp.h"
#include "tcp_stream.h"
#include "mos_api.h"

#define MAX_SEND_PCK_CHUNK 64

int 
FlushSendChunkBuf(mtcp_manager_t mtcp, int nif);

uint8_t *
EthernetOutput(struct mtcp_manager *mtcp, struct pkt_ctx *pctx,
		uint16_t h_proto, int nif, unsigned char* dst_haddr, uint16_t iplen,
		uint32_t cur_ts);

void
ForwardEthernetFrame(struct mtcp_manager *mtcp, struct pkt_ctx *pctx);

#endif /* __ETH_OUT_H_ */
