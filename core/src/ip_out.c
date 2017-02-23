#include <string.h>

#include "ip_out.h"
#include "ip_in.h"
#include "eth_out.h"
#include "arp.h"
#include "debug.h"
#include "config.h"

/*----------------------------------------------------------------------------*/
inline int
GetOutputInterface(uint32_t daddr)
{
	int nif = -1;
	int i;
	int prefix = -1;

#ifdef ENABLE_DPDKR
	/* currently we assume that virtual ports are just looping back */
	return 0;
#endif

	/* Longest prefix matching */
	for (i = 0; i < g_config.mos->route_table->num; i++) {
		if ((daddr & g_config.mos->route_table->ent[i]->mask)
			== g_config.mos->route_table->ent[i]->masked_ip) {
			if (g_config.mos->route_table->ent[i]->prefix > prefix) {
				nif = g_config.mos->route_table->ent[i]->nif;
				prefix = g_config.mos->route_table->ent[i]->prefix;
			}
		}
	}

	if (nif < 0) {
		uint8_t *da = (uint8_t *)&daddr;
		TRACE_ERROR("[WARNING] No route to %u.%u.%u.%u\n", 
				da[0], da[1], da[2], da[3]);
		assert(0);
	}
	
	return nif;
}
/*----------------------------------------------------------------------------*/
void
ForwardIPPacket(mtcp_manager_t mtcp, struct pkt_ctx *pctx)
{
	unsigned char * haddr;
	struct iphdr *iph;
	uint32_t daddr = 0;

	if (g_config.mos->nic_forward_table != NULL) {
		pctx->out_ifidx = 
			g_config.mos->nic_forward_table->nic_fwd_table[pctx->p.in_ifidx];
		if (pctx->out_ifidx != -1) {
			haddr = pctx->p.ethh->h_dest;
			goto fast_tx;
		}
	}

	/* set daddr for easy code writing */
	daddr = pctx->p.iph->daddr;		

	if (pctx->out_ifidx < 0) {
		pctx->out_ifidx = GetOutputInterface(pctx->p.iph->daddr);
		if (pctx->out_ifidx < 0) {
			return;
		}
	}

	haddr = GetDestinationHWaddr(daddr);
	if (!haddr) {
#ifdef RUN_ARP
		/* ARP requests will not be created if it's a standalone middlebox */
		if (!pctx->forward)
			RequestARP(mtcp, daddr,
				   pctx->out_ifidx, pctx->p.cur_ts);
#else
		uint8_t *da = (uint8_t *)&(pctx->p.iph->daddr);
		TRACE_INFO("[WARNING] The destination IP %u.%u.%u.%u "
			  "is not in ARP table!\n",
			  da[0], da[1], da[2], da[3]);
#endif
		return;
	}

#ifdef SHARE_IO_BUFFER
	if (likely(mtcp->iom->set_wptr != NULL)) {
		int i;
		for (i = 0; i < ETH_ALEN; i++) {
			pctx->p.ethh->h_source[i] = g_config.mos->netdev_table->ent[pctx->out_ifidx]->haddr[i];
			pctx->p.ethh->h_dest[i] = haddr[i];
		}
		mtcp->iom->set_wptr(mtcp->ctx, pctx->out_ifidx, pctx->p.in_ifidx, pctx->batch_index);
		return;
	}
#endif

 fast_tx:	
	iph = (struct iphdr *) EthernetOutput (mtcp, pctx, ETH_P_IP,
			pctx->out_ifidx, haddr, pctx->p.ip_len, pctx->p.cur_ts);
	if (iph)
		memcpy(iph, pctx->p.iph, pctx->p.ip_len);
	else
		TRACE_ERROR("Failed to send a packet\n"
				"NULL = EthernetOutput(%p, %d, %d, %p, %d)\n",
				mtcp, ETH_P_IP, pctx->out_ifidx, haddr, pctx->p.ip_len);
#ifdef PKTDUMP
	DumpPacket(mtcp,
			(char *)iph - sizeof(struct ethhdr),
			pctx->p.ip_len + sizeof(struct ethhdr),
			"OUT", pctx->out_ifidx);
#endif
}
/*----------------------------------------------------------------------------*/
inline void
FillOutPacketIPContext(struct pkt_ctx *pctx, struct iphdr *iph, int ip_len)
{
	pctx->p.iph = iph;
	pctx->p.ip_len = ip_len;
	
	return;
}
/*----------------------------------------------------------------------------*/
uint8_t *
IPOutputStandalone(struct mtcp_manager *mtcp, 
		uint16_t ip_id, uint32_t saddr, uint32_t daddr, uint16_t tcplen,
		struct pkt_ctx *pctx, uint32_t cur_ts)
{
	struct iphdr *iph;
	int nif;
	unsigned char * haddr;
	int rc = -1;

	nif = GetOutputInterface(daddr);
	if (nif < 0)
		return NULL;

	haddr = GetDestinationHWaddr(daddr);
	if (!haddr) {
#ifdef RUN_ARP
		/* ARP requests will not be created if it's a standalone middlebox */
		/* tcp will retry sending the packet later */
		if (!pctx->forward)
			RequestARP(mtcp, daddr, nif, mtcp->cur_ts);
#else
		uint8_t *da = (uint8_t *)&daddr;
		TRACE_INFO("[WARNING] The destination IP %u.%u.%u.%u "
			"is not in ARP table!\n",
			da[0], da[1], da[2], da[3]);
#endif
		return NULL;
	}
	
	iph = (struct iphdr *)EthernetOutput(mtcp, pctx,
			ETH_P_IP, nif, haddr, tcplen + IP_HEADER_LEN, cur_ts);
	if (!iph) {
		return NULL;
	}

	iph->ihl = IP_HEADER_LEN >> 2;
	iph->version = 4;
	iph->tos = 0;
	iph->tot_len = htons(IP_HEADER_LEN + tcplen);
	iph->id = htons(ip_id);
	iph->frag_off = htons(0x4000);	// no fragmentation
	iph->ttl = 64;
	iph->protocol = IPPROTO_TCP;
	iph->saddr = saddr;
	iph->daddr = daddr;
	iph->check = 0;
	
	/* offload IP checkum if possible */
	if (likely(mtcp->iom->dev_ioctl != NULL))
		rc = mtcp->iom->dev_ioctl(mtcp->ctx, nif, PKT_TX_IP_CSUM, iph);
	/* otherwise calculate IP checksum in S/W */
	if (rc == -1)
		iph->check = ip_fast_csum(iph, iph->ihl);

	FillOutPacketIPContext(pctx, iph, tcplen + IP_HEADER_LEN);

	return (uint8_t *)(iph + 1);
}
/*----------------------------------------------------------------------------*/
uint8_t *
IPOutput(struct mtcp_manager *mtcp, tcp_stream *stream, uint16_t tcplen, 
			struct pkt_ctx *pctx, uint32_t cur_ts)
{
	struct iphdr *iph;
	int nif;
	unsigned char *haddr;
	int rc = -1;

	if (stream->sndvar->nif_out >= 0) {
		nif = stream->sndvar->nif_out;
	} else {
		nif = GetOutputInterface(stream->daddr);
		stream->sndvar->nif_out = nif;
	}	

	haddr = GetDestinationHWaddr(stream->daddr);
	if (!haddr) {
#ifdef RUN_ARP
		/* if not found in the arp table, send arp request and return NULL */
		/* ARP requests will not be created if it's a standalone middlebox */
		/* tcp will retry sending the packet later */
		if (!pctx->forward)
			RequestARP(mtcp, stream->daddr, stream->sndvar->nif_out, mtcp->cur_ts);
#else
		uint8_t *da = (uint8_t *)&stream->daddr;
		TRACE_INFO("[WARNING] The destination IP %u.%u.%u.%u "
			  "is not in ARP table!\n",
			  da[0], da[1], da[2], da[3]);
#endif
		return NULL;
	}
	iph = (struct iphdr *)EthernetOutput(mtcp, pctx, ETH_P_IP,
			stream->sndvar->nif_out, haddr, tcplen + IP_HEADER_LEN, cur_ts);
	if (!iph) {
		return NULL;
	}

	iph->ihl = IP_HEADER_LEN >> 2;
	iph->version = 4;
	iph->tos = 0;
	iph->tot_len = htons(IP_HEADER_LEN + tcplen);
	iph->id = htons(stream->sndvar->ip_id++);
	iph->frag_off = htons(0x4000);	// no fragmentation
	iph->ttl = 64;
	iph->protocol = IPPROTO_TCP;
	iph->saddr = stream->saddr;
	iph->daddr = stream->daddr;
	iph->check = 0;

	/* offload IP checkum if possible */
	if (likely(mtcp->iom->dev_ioctl != NULL))
		rc = mtcp->iom->dev_ioctl(mtcp->ctx, nif, PKT_TX_IP_CSUM, iph);
	/* otherwise calculate IP checksum in S/W */
	if (rc == -1)
		iph->check = ip_fast_csum(iph, iph->ihl);

	FillOutPacketIPContext(pctx, iph, tcplen + IP_HEADER_LEN);

	return (uint8_t *)(iph + 1);
}
