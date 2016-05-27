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
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_errno.h>

/*----------------------------------------------------------------------------*/
/* Number of packets to attempt to read from queue. */
#define MAX_PKT_BURST			32/*64*//*128*//*32*/

#define MBUF_SIZE 			(2048 + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define NB_MBUF				8192
#define MEMPOOL_CACHE_SIZE		256

/* Define common names for structures shared between ovs_dpdk and client. */
#define MP_CLIENT_RXQ_NAME "dpdkr%u_tx"
#define MP_CLIENT_TXQ_NAME "dpdkr%u_rx"
struct rte_ring *rx_ring[MAX_CPUS];
struct rte_ring *tx_ring[MAX_CPUS];

/* packet memory pools for storing packet bufs */
static struct rte_mempool *pktmbuf_pool[MAX_CPUS] = {NULL};
struct rte_mbuf* rmbufs[MAX_CPUS][MAX_PKT_BURST];
struct rte_mbuf* wmbufs[MAX_CPUS][MAX_PKT_BURST];
unsigned rx_pkts[MAX_CPUS];
unsigned tx_pkts[MAX_CPUS];
/*----------------------------------------------------------------------------*/
int max_core = 1; // XXX: needs to be fixed
/*----------------------------------------------------------------------------*/
/*
 * Given the rx queue name template above, get the queue name.
 */
static inline const char *
get_rx_queue_name(unsigned id)
{
    /* Buffer for return value. Size calculated by %u being replaced
     * by maximum 3 digits (plus an extra byte for safety).
     */
    static char buffer[sizeof(MP_CLIENT_RXQ_NAME) + 2];

    snprintf(buffer, sizeof(buffer) - 1, MP_CLIENT_RXQ_NAME, id);
    return buffer;
}
/*
 * Given the tx queue name template above, get the queue name.
 */
static inline const char *
get_tx_queue_name(unsigned id)
{
    /* Buffer for return value. Size calculated by %u being replaced
     * by maximum 3 digits (plus an extra byte for safety).
     */
    static char buffer[sizeof(MP_CLIENT_TXQ_NAME) + 2];

    snprintf(buffer, sizeof(buffer) - 1, MP_CLIENT_TXQ_NAME, id);
    return buffer;
}
/*----------------------------------------------------------------------------*/
void
dpdkr_init_handle(struct mtcp_thread_context *ctxt)
{
	int i;
	
	/* Allocate wmbufs for each registered port */
	for (i = 0; i < MAX_PKT_BURST; i++) {
		wmbufs[ctxt->cpu][i] = rte_pktmbuf_alloc(pktmbuf_pool[ctxt->cpu]);
		if (wmbufs[ctxt->cpu][i] == NULL) {
			TRACE_ERROR("Failed to allocate %d:wmbuf on core %d!\n",
						ctxt->cpu, i);
			exit(EXIT_FAILURE);
		}
	}
	/* set mbufs queue length to 0 to begin with */
	rx_pkts[ctxt->cpu] = 0;
	tx_pkts[ctxt->cpu] = 0;
}
/*----------------------------------------------------------------------------*/
uint8_t *
dpdkr_get_wptr(struct mtcp_thread_context *ctxt, int nif, uint16_t pktsize)
{
	struct rte_mbuf *m;
	uint8_t *ptr;
	int len_of_mbuf;

	/* sanity check */
	if (unlikely(tx_pkts[ctxt->cpu] == MAX_PKT_BURST))
		return NULL;

	len_of_mbuf = tx_pkts[ctxt->cpu];
	m = wmbufs[ctxt->cpu][len_of_mbuf];
	
	/* retrieve the right write offset */
	ptr = (void *)rte_pktmbuf_mtod(m, struct ether_hdr *);
	m->pkt_len = m->data_len = pktsize;
	m->nb_segs = 1;
	m->next = NULL;

	/* increment the len_of_mbuf var */
	tx_pkts[ctxt->cpu] = len_of_mbuf + 1;
	
	return (uint8_t *)ptr;
}
/*----------------------------------------------------------------------------*/
void
dpdkr_set_wptr(struct mtcp_thread_context *ctxt, int out_nif, int in_nif, int index)
{
	int len_of_mbuf;

	/* sanity check */
	if (unlikely(tx_pkts[ctxt->cpu] == MAX_PKT_BURST))
		return;

	len_of_mbuf = tx_pkts[ctxt->cpu];
	wmbufs[ctxt->cpu][len_of_mbuf] = rmbufs[ctxt->cpu][index];
	wmbufs[ctxt->cpu][len_of_mbuf]->udata64 = 0;

	tx_pkts[ctxt->cpu] = len_of_mbuf + 1;

	return;
}
/*----------------------------------------------------------------------------*/
int
dpdkr_send_pkts(struct mtcp_thread_context *ctxt, int nif)
{
	int rslt = 0;
	
	if (tx_pkts[ctxt->cpu] > 0) {
		// blocking enqueue
		do {
			rslt = rte_ring_enqueue_bulk(tx_ring[ctxt->cpu],
										 (void *)wmbufs[ctxt->cpu],
										 tx_pkts[ctxt->cpu]);
		} while (rslt == -ENOBUFS);

#ifndef SHARE_IO_BUFFER
		int i;
		/* Allocate wmbufs for each registered port */
		for (i = 0; i < tx_pkts[ctxt->cpu]; i++) {
			wmbufs[ctxt->cpu][i] = rte_pktmbuf_alloc(pktmbuf_pool[ctxt->cpu]);
			if (wmbufs[ctxt->cpu][i] == NULL) {
				TRACE_ERROR("Failed to allocate %d:wmbuf on core %d!\n",
							ctxt->cpu, i);
			exit(EXIT_FAILURE);
			}
		}
#endif
		
		tx_pkts[ctxt->cpu] = 0;
	}

	return rslt;
}
/*----------------------------------------------------------------------------*/
static inline void
free_pkts(struct rte_mbuf **mtable, unsigned len)
{
	int i;
	
	/* free the freaking packets */
	for (i = 0; i < len; i++) {
		if (mtable[i]->udata64 == 1) {
			rte_pktmbuf_free_seg(mtable[i]);
			RTE_MBUF_PREFETCH_TO_FREE(mtable[i+1]);
		}
	}
}
/*----------------------------------------------------------------------------*/
int32_t
dpdkr_recv_pkts(struct mtcp_thread_context *ctxt, int ifidx)
{
	rx_pkts[ctxt->cpu] = MAX_PKT_BURST;	

	/* Try dequeuing max possible packets first, if that fails, get the
	 * most we can. Loop body should only execute once, maximum.
	 */
	while (unlikely(rte_ring_dequeue_bulk(rx_ring[ctxt->cpu],
										  (void *)rmbufs[ctxt->cpu],
										  rx_pkts[ctxt->cpu]) != 0) &&
		   rx_pkts[ctxt->cpu] > 0) {
		rx_pkts[ctxt->cpu] = (uint16_t)RTE_MIN(rte_ring_count(rx_ring[ctxt->cpu]), MAX_PKT_BURST);
	}
	return rx_pkts[ctxt->cpu];

}
/*----------------------------------------------------------------------------*/
uint8_t *
dpdkr_get_rptr(struct mtcp_thread_context *ctxt, int ifidx, int index, uint16_t *len)
{
	struct rte_mbuf *m;
	uint8_t *pktbuf;

	m = rmbufs[ctxt->cpu][index];
	m->udata64 = 1;
	*len = m->pkt_len;
	pktbuf = rte_pktmbuf_mtod(m, uint8_t *);

#ifdef DPDKR_ADD_IP_TOS_TAG
	uint16_t cksum;
	struct ipv4_hdr *ipv4_hdr;
	ipv4_hdr = (struct ipv4_hdr *)(rte_pktmbuf_mtod(m, unsigned char *) +
								   sizeof(struct ether_hdr));
	ipv4_hdr->type_of_service += 8;
	cksum = ntohs(ipv4_hdr->hdr_checksum);
	cksum -= 8;
	ipv4_hdr->hdr_checksum = htons(cksum);
#endif

	return pktbuf;
}
/*----------------------------------------------------------------------------*/
void
dpdkr_load_module_upper_half(void)
{
	int retval = 0;
	char *argv[] = {"", 
					"-c", 
					"F",
					"-n", 
					"4",
					"--proc-type=secondary",
					""
	};
	const int argc = 6;

	if ((retval = rte_eal_init(argc, argv)) < 0) {
		fprintf(stderr, "rte_eal_init() error!\n");
		exit(-1);
	}	
	fprintf(stderr, "rte_eal_init() completed.\n\n");

	/* Our client id number - tells us which rx queue to read, and tx
	 * queue to write to.
	 */	
	int c;
	for (c = 0; c < max_core; c++) {
		rx_ring[c] = NULL;
		tx_ring[c] = NULL;
		rx_pkts[c] = 0;
		
		rx_ring[c] = rte_ring_lookup(get_rx_queue_name(c));
		if (rx_ring[c] == NULL) {
			rte_exit(EXIT_FAILURE,
					 "Cannot get RX ring - is server process running?\n");
		}
		
		tx_ring[c] = rte_ring_lookup(get_tx_queue_name(c));
		if (tx_ring[c] == NULL) {
			rte_exit(EXIT_FAILURE,
					 "Cannot get TX ring - is server process running?\n");
		}
		fprintf(stderr, "rte_ring_lookup() completed.\n\n");
   		printf("\nClient process %d handling packets\n", c);
	}
}
/*----------------------------------------------------------------------------*/
void
dpdkr_load_module_lower_half(void)
{
	int c;
	char name[30];

	for (c = 0; c < max_core; c++) {
		sprintf(name, "ovs_mp_1500_0_262144");
		pktmbuf_pool[c] = rte_mempool_lookup(name);
		if (pktmbuf_pool[c] == NULL)
			rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");
	}

	fprintf(stderr, "rte_mempool_create() completed.\n");
}
/*----------------------------------------------------------------------------*/
void
dpdkr_destroy_handle(struct mtcp_thread_context *ctxt)
{
	fprintf(stderr, "dpdkr_destroy_handle is called!\n");
	free_pkts(wmbufs[ctxt->cpu], MAX_PKT_BURST);
}
/*----------------------------------------------------------------------------*/
io_module_func dpdkr_module_func = {
	.load_module_upper_half = dpdkr_load_module_upper_half,
	.load_module_lower_half = dpdkr_load_module_lower_half,
	.init_handle		    = dpdkr_init_handle,
	.link_devices		    = NULL,
	.release_pkt		    = NULL,
	.send_pkts		        = dpdkr_send_pkts,
	.get_wptr   		    = dpdkr_get_wptr,
	.set_wptr		        = dpdkr_set_wptr,
	.recv_pkts		        = dpdkr_recv_pkts,
	.get_rptr	   	        = dpdkr_get_rptr,
	.select			        = NULL,
	.destroy_handle		    = dpdkr_destroy_handle,
	.dev_ioctl		        = NULL,
};
/*----------------------------------------------------------------------------*/

