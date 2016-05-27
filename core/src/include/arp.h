#ifndef __ARP_H_
#define __ARP_H_

#include "mos_api.h"

#define MAX_ARPENTRY			1024
#define RUN_ARP				1
#define ARP_TIMEOUT_SEC			1

int 
InitARPTable();

unsigned char * 
GetHWaddr(uint32_t ip);

unsigned char *
GetDestinationHWaddr(uint32_t dip);

void 
RequestARP(mtcp_manager_t mtcp, uint32_t ip, int nif, uint32_t cur_ts);

int 
ProcessARPPacket(mtcp_manager_t mtcp, uint32_t cur_ts,
		const int ifidx, unsigned char* pkt_data, int len);

void 
PublishARP(mtcp_manager_t mtcp);

void 
PrintARPTable();

void
ForwardARPPacket(mtcp_manager_t mtcp, struct pkt_ctx *pctx);

void
ARPTimer(mtcp_manager_t mtcp, uint32_t cur_ts);

#endif /* __ARP_H_ */
