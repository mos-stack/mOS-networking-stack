#ifndef __IO_MODULE_H__
#define __IO_MODULE_H__

#include <sys/types.h>
#include <ifaddrs.h>
#include <net/if.h>
/*----------------------------------------------------------------------------*/
/* for type def'ns */
#include <stdint.h>
/*----------------------------------------------------------------------------*/
/**
 * Declaration to soothe down the warnings 
 */
struct mtcp_thread_context;
/**
 * io_module_funcs - contains template for the various 10Gbps pkt I/O
 *                 - libraries that can be adopted.
 *
 *		   load_module()    : Used to set system-wide I/O module 
 *				      initialization.
 *
 *                 init_handle()    : Used to initialize the driver library
 *                                  : Also use the context to create/initialize
 *                                  : a private packet I/O data structures.
 *
 *                 link_devices()   : Used to add link(s) to the mtcp stack.
 *				      Returns 0 on success; -1 on failure.
 *
 *		   release_pkt()    : release the packet if mTCP does not need
 *				      to process it (e.g. non-IPv4, non-TCP pkts).
 *
 *		   get_wptr()	    : retrieve the next empty pkt buffer for the 
 * 				      application for packet writing. Returns
 *				      ptr to pkt buffer.
 *
 *		   set_wptr()	    : set the next empty pkt buffer for the 
 * 				      application for packet writing. Returns void.
 *				      Use this function if you want to share rx/tx 
 *				      buffer (ZERO-COPY)
 *				      
 *		   send_pkts()	    : transmit batch of packets via interface 
 * 				      idx (=nif). 
 *				      Returns 0 on success; -1 on failure
 *
 *		   get_rptr()	    : retrieve next pkt for application for
 *				      packet read.
 *				      Returns ptr to pkt buffer.
 *			       
 *		   recv_pkts()	    : recieve batch of packets from the interface, 
 *				      ifidx.
 *				      Returns no. of packets that are read from
 *				      the iface.
 *
 *		   select()	    : for blocking I/O
 *
 *		   destroy_handle() : free up resources allocated during 
 * 				      init_handle(). Normally called during 
 *				      process termination.
 *
 *		   dev_ioctl()	    : contains submodules for select drivers
 *		   
 */
typedef struct io_module_func {
	void	  (*load_module_upper_half)(void);
	void	  (*load_module_lower_half)(void);
	void      (*init_handle)(struct mtcp_thread_context *ctx);
	int32_t   (*link_devices)(struct mtcp_thread_context *ctx);
	void      (*release_pkt)(struct mtcp_thread_context *ctx, int ifidx, unsigned char *pkt_data, int len);
	uint8_t * (*get_wptr)(struct mtcp_thread_context *ctx, int ifidx, uint16_t len);
	void 	  (*set_wptr)(struct mtcp_thread_context *ctx, int out_ifidx, int in_ifidx, int idx);
	int32_t   (*send_pkts)(struct mtcp_thread_context *ctx, int nif);
	uint8_t * (*get_rptr)(struct mtcp_thread_context *ctx, int ifidx, int index, uint16_t *len);
	int       (*get_nif)(struct ifreq *ifr);
	int32_t   (*recv_pkts)(struct mtcp_thread_context *ctx, int ifidx);
	int32_t	  (*select)(struct mtcp_thread_context *ctx);
	void	  (*destroy_handle)(struct mtcp_thread_context *ctx);
	int32_t	  (*dev_ioctl)(struct mtcp_thread_context *ctx, int nif, int cmd, void *argp);
} io_module_func __attribute__((aligned(__WORDSIZE)));
/*----------------------------------------------------------------------------*/
io_module_func *current_iomodule_func;
typedef struct {
	int8_t	 pktidx;
	uint32_t hash_value;
} RssInfo;
/*----------------------------------------------------------------------------*/
#define MAX_DEVICES     16
/* IO-MODULE string literals */
#define DPDK_STR		"dpdk"
#define PCAP_STR		"pcap"
#define NETMAP_STR		"netmap"
/* dev_ioctl related macros */
#define PKT_TX_IP_CSUM		0x01
#define PKT_TX_TCP_CSUM		0x02
#define PKT_RX_RSS		0x03
#define DRV_NAME		0x08

/* enable shared RX/TX buffers */
//#define SHARE_IO_BUFFER

#ifdef ENABLE_DPDK
/* registered dpdk context */
extern io_module_func dpdk_module_func;
#endif /* !ENABLE_DPDK */

#ifdef ENABLE_PCAP
extern io_module_func pcap_module_func;
#endif

#ifdef ENABLE_NETMAP
extern io_module_func netmap_module_func;
#endif
/*----------------------------------------------------------------------------*/
#endif /* !__IO_MODULE_H__ */
