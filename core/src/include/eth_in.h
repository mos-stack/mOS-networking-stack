#ifndef __ETH_IN_H_
#define __ETH_IN_H_

#include "mtcp.h"

int
ProcessPacket(mtcp_manager_t mtcp, const int ifidx, const int index,
		uint32_t cur_ts, unsigned char *pkt_data, int len);

#endif /* __ETH_IN_H_ */
