#include <stdio.h>

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

#ifdef DARWIN
#include <netinet/if_ether.h>
#include <netinet/tcp.h>
#else
#include <linux/if_ether.h>
#include <linux/tcp.h>
#endif
#include <string.h>
#include <netinet/ip.h>

#include "mtcp.h"
#include "arp.h"
#include "eth_out.h"
#include "debug.h"
#include "mos_api.h"
#include "config.h"

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#ifndef ERROR
#define ERROR (-1)
#endif

#define MAX(a, b) ((a)>(b)?(a):(b))
#define MIN(a, b) ((a)<(b)?(a):(b))

#define MAX_WINDOW_SIZE 65535

/*----------------------------------------------------------------------------*/
enum ETH_BUFFER_RETURN {BUF_RET_MAYBE, BUF_RET_ALWAYS};
/*----------------------------------------------------------------------------*/
int 
FlushSendChunkBuf(mtcp_manager_t mtcp, int nif)
{
	return 0;
}
/*----------------------------------------------------------------------------*/
inline void
FillOutPacketEthContext(struct pkt_ctx *pctx, uint32_t cur_ts, int out_ifidx,
						struct ethhdr *ethh, int eth_len)
{
	pctx->p.cur_ts = cur_ts;
	pctx->p.in_ifidx = -1;
	pctx->out_ifidx = out_ifidx;
	pctx->p.ethh = ethh;
	pctx->p.eth_len = eth_len;
}
/*----------------------------------------------------------------------------*/
uint8_t *
EthernetOutput(struct mtcp_manager *mtcp, struct pkt_ctx *pctx,
		uint16_t h_proto, int nif, unsigned char* dst_haddr, uint16_t iplen,
		uint32_t cur_ts)
{
	uint8_t *buf;
	struct ethhdr *ethh;
	int i;

	/* 
 	 * -sanity check- 
	 * return early if no interface is set (if routing entry does not exist)
	 */
	if (nif < 0) {
		TRACE_INFO("No interface set!\n");
		return NULL;
	}
	
	if (!mtcp->iom->get_wptr) {
		TRACE_INFO("get_wptr() in io_module is undefined.");
		return NULL;
	}
	buf = mtcp->iom->get_wptr(mtcp->ctx, nif, iplen + ETHERNET_HEADER_LEN);
	if (!buf) {
		TRACE_DBG("Failed to get available write buffer\n");
		return NULL;
	}

	ethh = (struct ethhdr *)buf;
	for (i = 0; i < ETH_ALEN; i++) {
		ethh->h_source[i] = g_config.mos->netdev_table->ent[nif]->haddr[i];
		ethh->h_dest[i] = dst_haddr[i];
	}
	ethh->h_proto = htons(h_proto);

	if (pctx)
		FillOutPacketEthContext(pctx, cur_ts, nif,
					ethh, iplen + ETHERNET_HEADER_LEN);

	return (uint8_t *)(ethh + 1);
}
/*----------------------------------------------------------------------------*/
void
ForwardEthernetFrame(struct mtcp_manager *mtcp, struct pkt_ctx *pctx)
{
	uint8_t *buf;

	if (g_config.mos->nic_forward_table != NULL) {
		pctx->out_ifidx = 
			g_config.mos->nic_forward_table->nic_fwd_table[pctx->p.in_ifidx];

		if (pctx->out_ifidx == -1) {
			TRACE_DBG("Could not find outgoing index (index)!\n");
			return;
		}

		if (!mtcp->iom->get_wptr) {
			TRACE_INFO("get_wptr() in io_module is undefined.");
			return;
		}

		buf = mtcp->iom->get_wptr(mtcp->ctx, pctx->out_ifidx, pctx->p.eth_len);
		
		if (!buf) {
			TRACE_DBG("Failed to get available write buffer\n");
			return;
		}
		
		memcpy(buf, pctx->p.ethh, pctx->p.eth_len);
	} else {
		TRACE_DBG("Ethernet forwarding table entry does not exist.\n");
	}
}
/*----------------------------------------------------------------------------*/
