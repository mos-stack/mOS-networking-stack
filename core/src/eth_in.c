#include <string.h>

#include "ip_in.h"
#include "eth_in.h"
#include "eth_out.h"
#include "arp.h"
#include "debug.h"
#include "ip_out.h"
#include "config.h"

/*----------------------------------------------------------------------------*/
inline void
FillInPacketEthContext (struct pkt_ctx *pctx, uint32_t cur_ts, int in_ifidx,
		        int index, struct ethhdr *ethh, int eth_len)
{
	pctx->p.cur_ts = cur_ts;
	pctx->p.in_ifidx = in_ifidx;
	pctx->out_ifidx = -1;
	pctx->p.ethh = ethh;
	pctx->p.eth_len = eth_len;
	pctx->batch_index = index;
	pctx->forward = g_config.mos->forward;
	
	return;
}
/*----------------------------------------------------------------------------*/
int
ProcessPacket(mtcp_manager_t mtcp, const int ifidx, const int index,
		uint32_t cur_ts, unsigned char *pkt_data, int len)
{
	struct pkt_ctx pctx;
	struct mon_listener *walk;
	struct ethhdr *ethh = (struct ethhdr *)pkt_data;
	int ret = -1;
	u_short h_proto = ntohs(ethh->h_proto);

	memset(&pctx, 0, sizeof(pctx));

#ifdef PKTDUMP
	DumpPacket(mtcp, (char *)pkt_data, len, "IN", ifidx);
#endif

#ifdef NETSTAT
	mtcp->nstat.rx_packets[ifidx]++;
	mtcp->nstat.rx_bytes[ifidx] += len + ETHER_OVR;
#endif /* NETSTAT */

	/**
	 * To Do: System level configurations or callback can enable each functionality
	 * - Check PROMISCUOUS MODE
	 * - ARP
	 * - SLOWPATH
	 */

	FillInPacketEthContext(&pctx, cur_ts, ifidx, index, ethh, len);

	if (h_proto == ETH_P_IP) {
		/* process ipv4 packet */
		ret = ProcessInIPv4Packet(mtcp, &pctx);
	} else {
		/* callback for monitor raw socket */
		TAILQ_FOREACH(walk, &mtcp->monitors, link)
			if (walk->socket->socktype == MOS_SOCK_MONITOR_RAW)
				HandleCallback(mtcp, MOS_NULL, walk->socket, MOS_SIDE_BOTH,
					       &pctx, MOS_ON_PKT_IN);
		
		/* drop the packet if forwarding is off */
		if (!mtcp->num_msp || !pctx.forward) {
#ifdef RUN_ARP
			if (h_proto == ETH_P_ARP) {
				ret = ProcessARPPacket(mtcp, cur_ts, ifidx, pkt_data, len);
				return TRUE;
			} else 
#endif
				{
					DumpPacket(mtcp, (char *)pkt_data, len, "??", ifidx);
					if (mtcp->iom->release_pkt)
						mtcp->iom->release_pkt(mtcp->ctx, ifidx, pkt_data, len);
				}
		} else { /* else forward */
			ForwardEthernetFrame(mtcp, &pctx);
			return TRUE;
		}
	}
	
#ifdef NETSTAT
	if (ret < 0) {
		mtcp->nstat.rx_errors[ifidx]++;
	}
#endif

	return ret;
}
/*----------------------------------------------------------------------------*/
