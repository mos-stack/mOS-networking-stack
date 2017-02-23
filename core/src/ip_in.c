#define __MOS_CORE_

#include <string.h>
#include <netinet/ip.h>
#include <stdbool.h>

#include "ip_in.h"
#include "ip_out.h"
#include "tcp.h"
#include "mtcp_api.h"
#include "debug.h"
#include "mos_api.h"
#include "icmp.h"
#include "config.h"

#define ETH_P_IP_FRAG   0xF800
#define ETH_P_IPV6_FRAG 0xF6DD


/*----------------------------------------------------------------------------*/
inline void
FillInPacketIPContext (struct pkt_ctx *pctx, struct iphdr *iph, int ip_len)
{
	pctx->p.iph = iph;
	pctx->p.ip_len = ip_len;
	
	return;
}
/*----------------------------------------------------------------------------*/
inline int 
ProcessInIPv4Packet(mtcp_manager_t mtcp, struct pkt_ctx *pctx)
{
	bool release = false;
	int ret;
	struct mon_listener *walk;
	/* check and process IPv4 packets */
	struct iphdr* iph =
		(struct iphdr *)((char *)pctx->p.ethh + sizeof(struct ethhdr));
	int ip_len = ntohs(iph->tot_len);

	/* drop the packet shorter than ip header */
	if (ip_len < sizeof(struct iphdr)) {
		ret = ERROR;
		goto __return;
	}

	if (ip_fast_csum(iph, iph->ihl)) {
		ret = ERROR;
		goto __return;
	}

	if (iph->version != IPVERSION ) {
		release = true;
		ret = FALSE;
		goto __return;
	}

	FillInPacketIPContext(pctx, iph, ip_len);

	switch (iph->protocol) {
		case IPPROTO_TCP:
			return ProcessInTCPPacket(mtcp, pctx);
		case IPPROTO_ICMP:
			if (ProcessICMPPacket(mtcp, pctx))
				return TRUE;
		default:
			/* forward other protocols without any processing */
			if (!mtcp->num_msp || !pctx->forward)
				release = true;
			else
				ForwardIPPacket(mtcp, pctx);
			
			ret = FALSE;
			goto __return;
	}

__return:
	/* callback for monitor raw socket */
	TAILQ_FOREACH(walk, &mtcp->monitors, link)
		if (walk->socket->socktype == MOS_SOCK_MONITOR_RAW)
			HandleCallback(mtcp, MOS_NULL, walk->socket, MOS_SIDE_BOTH,
				       pctx, MOS_ON_PKT_IN);
	if (release && mtcp->iom->release_pkt)
		mtcp->iom->release_pkt(mtcp->ctx, pctx->p.in_ifidx,
				       (unsigned char *)pctx->p.ethh, pctx->p.eth_len);
	return ret;
}
/*----------------------------------------------------------------------------*/
