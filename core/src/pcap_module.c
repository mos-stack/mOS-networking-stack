/* for io_module_func def'ns */
#include "io_module.h"
/* for mtcp related def'ns */
#include "mtcp.h"
/* for errno */
#include <errno.h>
/* for logging */
#include "debug.h"
/* for num_devices_* */
#include "config.h"

#include <string.h>
#include <pcap.h>

/*----------------------------------------------------------------------------*/
struct pcap_private_context {
	pcap_t *handle[MAX_DEVICES];
	char   *dev_name[MAX_DEVICES];

	struct pcap_pkthdr phdr;
	const u_char *rdata;
	u_char wdata[ETHERNET_FRAME_LEN];
	uint16_t count[MAX_DEVICES];
} g_pcap_ctx;
/*----------------------------------------------------------------------------*/
void
pcap_init_handle(struct mtcp_thread_context *ctxt)
{
	ctxt->io_private_context = &g_pcap_ctx;
}
/*----------------------------------------------------------------------------*/
int32_t
pcap_recv_pkts(struct mtcp_thread_context *ctxt, int ifidx)
{
	if (ifidx < 0 || ifidx >= MAX_DEVICES)
		return -1;

	struct pcap_private_context *ppc = ctxt->io_private_context;
	if ((ppc->rdata = pcap_next(ppc->handle[ifidx], &ppc->phdr)) == NULL)
		return 0;
	
	return 1; /* PCAP always receives only one packet */
}
/*----------------------------------------------------------------------------*/
uint8_t *
pcap_get_rptr(struct mtcp_thread_context *ctxt, int ifidx, int index, uint16_t *len)
{
	struct pcap_private_context *ppc = ctxt->io_private_context;
	if (ppc->rdata == NULL)
		return NULL;

	*len = ppc->phdr.caplen;
	return (uint8_t *)ppc->rdata;
}
/*----------------------------------------------------------------------------*/
int
pcap_send_pkts(struct mtcp_thread_context *ctxt, int nif)
{
	int ret = 0;
	struct pcap_private_context *ppc = ctxt->io_private_context;

	if (ppc->count[nif] > 0) {
		ret = pcap_inject(ppc->handle[nif], ppc->wdata, ppc->phdr.len);
	}

	ppc->count[nif] = 0;
	return (ret != -1) ? 1 : 0;
}
/*----------------------------------------------------------------------------*/
uint8_t *
pcap_get_wptr(struct mtcp_thread_context *ctxt, int ifidx, uint16_t pktsize)
{
	struct pcap_private_context *ppc = ctxt->io_private_context;
	/* phdr can be reused */
	ppc->phdr.caplen = ppc->phdr.len = pktsize;
	ppc->count[ifidx] = 1;
	return ppc->wdata;
}
/*----------------------------------------------------------------------------*/
int
pcap_get_nif(struct ifreq *ifr)
{
	int i;

	for (i = 0; i < g_config.mos->netdev_table->num; i++)
		if (!strcmp(ifr->ifr_name, g_pcap_ctx.dev_name[i]))
			return i;

	return -1;
}
/*----------------------------------------------------------------------------*/
int32_t
pcap_select(struct mtcp_thread_context *ctxt)
{
	return 0;
}
/*----------------------------------------------------------------------------*/
void
pcap_destroy_handle(struct mtcp_thread_context *ctxt)
{
	int i;
	struct pcap_private_context *ppc = ctxt->io_private_context;
	if (!ppc)
		return;

	for (i = 0; i < g_config.mos->netdev_table->num; i++) {
		pcap_close(ppc->handle[i]);
	}

	ctxt->io_private_context = NULL;
}
/*----------------------------------------------------------------------------*/
void
pcap_load_module_upper_half(void)
{
	int i;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct netdev_entry **ent = g_config.mos->netdev_table->ent;

	for (i = 0; i < g_config.mos->netdev_table->num; i++) {
		g_pcap_ctx.handle[i] = pcap_open_live(ent[i]->dev_name, BUFSIZ, 1,
										1, errbuf);
		if (!g_pcap_ctx.handle[i]) {
			TRACE_ERROR("Interface '%s' not found\n", ent[i]->dev_name);
			exit(EXIT_FAILURE);
		}
		g_pcap_ctx.dev_name[i] = ent[i]->dev_name;
	}

	num_queues = 1;
}
/*----------------------------------------------------------------------------*/
io_module_func pcap_module_func = {
	.load_module_upper_half	   = pcap_load_module_upper_half,
	.load_module_lower_half	   = NULL,
	.init_handle		   = pcap_init_handle,
	.link_devices		   = NULL,
	.release_pkt		   = NULL,
	.send_pkts		   = pcap_send_pkts,
	.get_wptr   		   = pcap_get_wptr,
	.set_wptr   		   = NULL,
	.recv_pkts		   = pcap_recv_pkts,
	.get_rptr	   	   = pcap_get_rptr,
	.select			   = NULL,
	.destroy_handle		   = pcap_destroy_handle,
	.dev_ioctl		   = NULL,
	.get_nif		   = pcap_get_nif,
};
/*----------------------------------------------------------------------------*/
