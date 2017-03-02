#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "mtcp.h"
#include "arp.h"
#include "eth_out.h"
#include "debug.h"
#include "config.h"

#define ARP_PAD_LEN 			18
#define ARP_HEAD_LEN 			8
#define TS_GEQ(a, b)			((int32_t)((a)-(b)) >= 0)
#define SEC_TO_TS(a)		       	(a * 1000)

/*----------------------------------------------------------------------------*/
enum arp_hrd_format
{
	arp_hrd_ethernet = 1
};
/*----------------------------------------------------------------------------*/
enum arp_opcode
{
	arp_op_request = 1, 
	arp_op_reply = 2, 
};
/*----------------------------------------------------------------------------*/
struct arphdr
{
	uint16_t ar_hrd;			/* hardware address format */
	uint16_t ar_pro;			/* protocol address format */
	uint8_t ar_hln;				/* hardware address length */
	uint8_t ar_pln;				/* protocol address length */
	uint16_t ar_op;				/* arp opcode */
	
	uint8_t ar_sha[ETH_ALEN];	/* sender hardware address */
	uint32_t ar_sip;			/* sender ip address */
	uint8_t ar_tha[ETH_ALEN];	/* targe hardware address */
	uint32_t ar_tip;			/* target ip address */

	uint8_t pad[ARP_PAD_LEN];
} __attribute__ ((packed));
/*----------------------------------------------------------------------------*/
struct arp_queue_entry
{
	uint32_t ip;
	int nif_out;
	uint32_t ts_out;

	TAILQ_ENTRY(arp_queue_entry) arp_link;
};
/*----------------------------------------------------------------------------*/
struct arp_manager
{
	TAILQ_HEAD (, arp_queue_entry) list;
	pthread_mutex_t lock;
};
/*----------------------------------------------------------------------------*/
struct arp_manager g_arpm;
/*----------------------------------------------------------------------------*/
void 
DumpARPPacket(struct arphdr *arph);
/*----------------------------------------------------------------------------*/
int 
InitARPTable()
{
	TAILQ_INIT(&g_arpm.list);
	pthread_mutex_init(&g_arpm.lock, NULL);

	return 0;
}
/*----------------------------------------------------------------------------*/
unsigned char *
GetHWaddr(uint32_t ip)
{
	int i;
	unsigned char *haddr = NULL;
	for (i = 0; i < g_config.mos->netdev_table->num; i++) {
		if (ip == g_config.mos->netdev_table->ent[i]->ip_addr) {
			haddr = g_config.mos->netdev_table->ent[i]->haddr;
			break;
		}	
	}

	return haddr;
}
/*----------------------------------------------------------------------------*/
unsigned char *
GetDestinationHWaddr(uint32_t dip)
{
	unsigned char *d_haddr = NULL;
	int prefix = 0;
	int i;

#ifdef L3_TRANSPARENT
	static unsigned char none_haddr[ETH_ALEN] = {0,};
	return none_haddr;
#endif

	/* Longest prefix matching */
	for (i = 0; i < g_config.mos->arp_table->num; i++) {
		if (g_config.mos->arp_table->ent[i]->prefix == 1) {
			if (g_config.mos->arp_table->ent[i]->ip == dip) {
				d_haddr = g_config.mos->arp_table->ent[i]->haddr;
				break;
			}	
		} else {
			if ((dip & g_config.mos->arp_table->ent[i]->mask) ==
					g_config.mos->arp_table->ent[i]->masked_ip) {
				
				if (g_config.mos->arp_table->ent[i]->prefix > prefix) {
					d_haddr = g_config.mos->arp_table->ent[i]->haddr;
					prefix = g_config.mos->arp_table->ent[i]->prefix;
				}
			}
		}
	}

	return d_haddr;
}
/*----------------------------------------------------------------------------*/
static int 
ARPOutput(struct mtcp_manager *mtcp, int nif, int opcode,
	  uint32_t dst_ip, unsigned char *dst_haddr,
	  unsigned char *target_haddr)
{
	if (!dst_haddr)
		return -1;

	/* Allocate a buffer */
	struct arphdr *arph = (struct arphdr *)EthernetOutput(mtcp, NULL,
			ETH_P_ARP, nif, dst_haddr, sizeof(struct arphdr), 0);
	if (!arph) {
		return -1;
	}

	/* Fill arp header */
	arph->ar_hrd = htons(arp_hrd_ethernet);
	arph->ar_pro = htons(ETH_P_IP);
	arph->ar_hln = ETH_ALEN;
	arph->ar_pln = 4;
	arph->ar_op = htons(opcode);

	/* Fill arp body */
	arph->ar_sip = g_config.mos->netdev_table->ent[nif]->ip_addr;
	arph->ar_tip = dst_ip;

	memcpy(arph->ar_sha, g_config.mos->netdev_table->ent[nif]->haddr, arph->ar_hln);
	if (target_haddr) {
		memcpy(arph->ar_tha, target_haddr, arph->ar_hln);
	} else {
		memcpy(arph->ar_tha, dst_haddr, arph->ar_hln);
	}
	memset(arph->pad, 0, ARP_PAD_LEN);

#if DBGMSG
	DumpARPPacket(arph);
#endif

	return 0;
}
/*----------------------------------------------------------------------------*/
int 
RegisterARPEntry(uint32_t ip, const unsigned char *haddr)
{
	int idx = g_config.mos->arp_table->num;
	g_config.mos->arp_table->ent[idx] = calloc(1, sizeof(struct _arp_entry));
	if (!g_config.mos->arp_table->ent[idx])
		exit(0);
	g_config.mos->arp_table->ent[idx]->prefix = 32;
	g_config.mos->arp_table->ent[idx]->ip = ip;
	memcpy(g_config.mos->arp_table->ent[idx]->haddr, haddr, ETH_ALEN);
	g_config.mos->arp_table->ent[idx]->mask = -1;
	g_config.mos->arp_table->ent[idx]->masked_ip = ip;

	g_config.mos->arp_table->num = idx + 1;

	TRACE_CONFIG("Learned new arp entry.\n");
	PrintARPTable();

	return 0;
}
/*----------------------------------------------------------------------------*/
void 
RequestARP(mtcp_manager_t mtcp, uint32_t ip, int nif, uint32_t cur_ts)
{
	struct arp_queue_entry *ent;
	unsigned char haddr[ETH_ALEN];
	unsigned char taddr[ETH_ALEN];

	pthread_mutex_lock(&g_arpm.lock);
	/* if the arp request is in progress, return */
	TAILQ_FOREACH(ent, &g_arpm.list, arp_link) {
		if (ent->ip == ip) {
			pthread_mutex_unlock(&g_arpm.lock);
			return;
		}
	}

	ent = (struct arp_queue_entry *)calloc(1, sizeof(struct arp_queue_entry));
	if (ent == NULL) {
		TRACE_ERROR("Unable to allocate memory for ARP entry!\n");
		exit(EXIT_FAILURE);
	}
	ent->ip = ip;
	ent->nif_out = nif;
	ent->ts_out = cur_ts;
	TAILQ_INSERT_TAIL(&g_arpm.list, ent, arp_link);
	pthread_mutex_unlock(&g_arpm.lock);

	/* else, broadcast arp request */
	memset(haddr, 0xFF, ETH_ALEN);
	memset(taddr, 0x00, ETH_ALEN);
	ARPOutput(mtcp, nif, arp_op_request, ip, haddr, taddr);
}
/*----------------------------------------------------------------------------*/
static int 
ProcessARPRequest(mtcp_manager_t mtcp, 
		struct arphdr *arph, int nif, uint32_t cur_ts)
{
	unsigned char *temp;

	/* register the arp entry if not exist */
	temp = GetDestinationHWaddr(arph->ar_sip);
	if (!temp) {
		RegisterARPEntry(arph->ar_sip, arph->ar_sha);
	}

	/* send arp reply */
	ARPOutput(mtcp, nif, arp_op_reply, arph->ar_sip, arph->ar_sha, NULL);

	return 0;
}
/*----------------------------------------------------------------------------*/
static int 
ProcessARPReply(mtcp_manager_t mtcp, struct arphdr *arph, uint32_t cur_ts)
{
	unsigned char *temp;
	struct arp_queue_entry *ent;

	/* register the arp entry if not exist */
	temp = GetDestinationHWaddr(arph->ar_sip);
	if (!temp) {
		RegisterARPEntry(arph->ar_sip, arph->ar_sha);
	}

	/* remove from the arp request queue */
	pthread_mutex_lock(&g_arpm.lock);
	TAILQ_FOREACH(ent, &g_arpm.list, arp_link) {
		if (ent->ip == arph->ar_sip) {
			TAILQ_REMOVE(&g_arpm.list, ent, arp_link);
			free(ent);
			break;
		}
	}
	pthread_mutex_unlock(&g_arpm.lock);

	return 0;
}
/*----------------------------------------------------------------------------*/
int 
ProcessARPPacket(mtcp_manager_t mtcp, uint32_t cur_ts,
		                  const int ifidx, unsigned char *pkt_data, int len)
{
	struct arphdr *arph = (struct arphdr *)(pkt_data + sizeof(struct ethhdr));
	int i;
	int to_me = FALSE;
	
	/* process the arp messages destined to me */
	for (i = 0; i < g_config.mos->netdev_table->num; i++) {
		if (arph->ar_tip == g_config.mos->netdev_table->ent[i]->ip_addr) {
			to_me = TRUE;
		}
	}

	if (!to_me)
		return TRUE;

#if DBGMSG
	DumpARPPacket(arph);
#endif

	switch (ntohs(arph->ar_op)) {
		case arp_op_request:
			ProcessARPRequest(mtcp, arph, ifidx, cur_ts);
			break;

		case arp_op_reply:
			ProcessARPReply(mtcp, arph, cur_ts);
			break;

		default:
			break;
	}

	return TRUE;
}
/*----------------------------------------------------------------------------*/
// Publish my address
void 
PublishARP(mtcp_manager_t mtcp)
{
	int i;
	for (i = 0; i < g_config.mos->netdev_table->num; i++) {
		ARPOutput(mtcp, g_config.mos->netdev_table->ent[i]->ifindex, arp_op_request, 0, NULL, NULL);
	}
}
/*----------------------------------------------------------------------------*/
/* ARPTimer: wakes up every milisecond and check the ARP timeout              */
/*           timeout is set to 1 second                                       */
/*----------------------------------------------------------------------------*/
void
ARPTimer(mtcp_manager_t mtcp, uint32_t cur_ts)
{
        struct arp_queue_entry *ent, *ent_tmp;
	
        /* if the arp requet is timed out, retransmit */
        pthread_mutex_lock(&g_arpm.lock);
        TAILQ_FOREACH_SAFE(ent, &g_arpm.list, arp_link, ent_tmp) {
                if (TS_GEQ(cur_ts, ent->ts_out + SEC_TO_TS(ARP_TIMEOUT_SEC))) {
                        TRACE_INFO("[CPU%2d] ARP request timed out.\n",
				   mtcp->ctx->cpu);
                        TAILQ_REMOVE(&g_arpm.list, ent, arp_link);
                        free(ent);
                }
        }
        pthread_mutex_unlock(&g_arpm.lock);
}
/*----------------------------------------------------------------------------*/
void
PrintARPTable()
{
	int i;
		
	/* print out process start information */
	TRACE_CONFIG("ARP Table:\n");
	for (i = 0; i < g_config.mos->arp_table->num; i++) {
			
		uint8_t *da = (uint8_t *)&g_config.mos->arp_table->ent[i]->ip;

		TRACE_CONFIG("IP addr: %u.%u.%u.%u, "
				"dst_hwaddr: %02X:%02X:%02X:%02X:%02X:%02X\n",
				da[0], da[1], da[2], da[3],
				g_config.mos->arp_table->ent[i]->haddr[0],
				g_config.mos->arp_table->ent[i]->haddr[1],
				g_config.mos->arp_table->ent[i]->haddr[2],
				g_config.mos->arp_table->ent[i]->haddr[3],
				g_config.mos->arp_table->ent[i]->haddr[4],
				g_config.mos->arp_table->ent[i]->haddr[5]);
	}
	if (g_config.mos->arp_table->num == 0)
		TRACE_CONFIG("(blank)\n");

	TRACE_CONFIG("----------------------------------------------------------"
			"-----------------------\n");
}
/*----------------------------------------------------------------------------*/
void 
DumpARPPacket(struct arphdr *arph)
{
	uint8_t *t;

	fprintf(stderr, "ARP header: \n");
	fprintf(stderr, "Hareware type: %d (len: %d), "
			"protocol type: %d (len: %d), opcode: %d\n", 
			ntohs(arph->ar_hrd), arph->ar_hln, 
			ntohs(arph->ar_pro), arph->ar_pln, ntohs(arph->ar_op));
	t = (uint8_t *)&arph->ar_sip;
	fprintf(stderr, "Sender IP: %u.%u.%u.%u, "
			"haddr: %02X:%02X:%02X:%02X:%02X:%02X\n", 
			t[0], t[1], t[2], t[3], 
			arph->ar_sha[0], arph->ar_sha[1], arph->ar_sha[2], 
			arph->ar_sha[3], arph->ar_sha[4], arph->ar_sha[5]);
	t = (uint8_t *)&arph->ar_tip;
	fprintf(stderr, "Target IP: %u.%u.%u.%u, "
			"haddr: %02X:%02X:%02X:%02X:%02X:%02X\n", 
			t[0], t[1], t[2], t[3], 
			arph->ar_tha[0], arph->ar_tha[1], arph->ar_tha[2], 
			arph->ar_tha[3], arph->ar_tha[4], arph->ar_tha[5]);
}
/*----------------------------------------------------------------------------*/
void
ForwardARPPacket(struct mtcp_manager *mtcp, struct pkt_ctx *pctx)
{
	unsigned char *haddr;
      
	if (g_config.mos->nic_forward_table != NULL) {
		pctx->out_ifidx = 
			g_config.mos->nic_forward_table->nic_fwd_table[pctx->p.in_ifidx];
		if (pctx->out_ifidx != -1) {
			haddr = pctx->p.ethh->h_dest;
			struct arphdr *arph = (struct arphdr *)
				EthernetOutput(mtcp, NULL, ETH_P_ARP,
					       pctx->out_ifidx, haddr,
					       sizeof(struct arphdr), 0);
			if (!arph)
				return;
			memcpy(arph, (pctx->p.ethh + 1), sizeof(struct arphdr));
		}
	}
}
/*----------------------------------------------------------------------------*/
