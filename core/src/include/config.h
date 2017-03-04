#ifndef __CONFIG_H_
#define __CONFIG_H_
/*----------------------------------------------------------------------------*/
#include <sys/queue.h>
#include <stdint.h>
#include <net/if.h>

#include "io_module.h"
/*----------------------------------------------------------------------------*/
#define WORD_LEN 20
#define STR_LEN  200
#define MOS_APP_ARGC 20
#define MAX_APP_BLOCK 10
#define MAX_MOS_BLOCK 1
#define MAX_ARP_ENTRY 128
#define MAX_ETH_ENTRY MAX_DEVICES
#define MAX_FORWARD_ENTRY MAX_ETH_ENTRY
#define MAX_ROUTE_ENTRY 128
#define ETH_ALEN 6
/*----------------------------------------------------------------------------*/
#define APP_BLOCK_NAME    	"application"
#define MOS_BLOCK_NAME    	"mos"
#define NETDEV_BLOCK_NAME 	"netdev"
#define ARP_BLOCK_NAME    	"arp_table"
#define ROUTE_BLOCK_NAME  	"route_table"
#define FORWARD_BLOCK_NAME 	"nic_forward_table"
/*----------------------------------------------------------------------------*/
struct conf_block;
extern int8_t end_app_exists;
extern int8_t mon_app_exists;
/*----------------------------------------------------------------------------*/
typedef void (*FEED)(struct conf_block *blk, char *line, int len);
typedef void (*ADDCHILD)(struct conf_block *blk, struct conf_block *child);
typedef int  (*ISVALID)(struct conf_block *blk);
typedef void (*PRINT)(struct conf_block *blk);
/*----------------------------------------------------------------------------*/
struct conf_block {
	char *name;
	char *buf;
	int len;

	FEED feed;
	ADDCHILD addchild;
	ISVALID isvalid;
	PRINT print;

	void *conf;

	TAILQ_HEAD(, conf_block) *list;

	TAILQ_ENTRY(conf_block) link;
};
/*----------------------------------------------------------------------------*/
struct netdev_entry {
	char dev_name[WORD_LEN];
	int ifindex;
	int stat_print;
	unsigned char haddr[ETH_ALEN];
	uint32_t netmask;
	uint32_t ip_addr;
	uint64_t cpu_mask;

	struct ifreq ifr;

	uint32_t gateway;

	TAILQ_ENTRY(netdev_entry) link;
};
struct netdev_conf {
	int num;
	struct netdev_entry *ent[MAX_ETH_ENTRY];

	TAILQ_HEAD(, netdev_entry) list;
};
/*----------------------------------------------------------------------------*/
struct _arp_entry {
	uint32_t ip;
	int8_t prefix;
	uint32_t mask;
	uint32_t masked_ip;
	uint8_t haddr[ETH_ALEN];

	TAILQ_ENTRY(_arp_entry) link;
};
struct arp_conf {
	int num;
	struct _arp_entry *ent[MAX_ARP_ENTRY];

	TAILQ_HEAD(, _arp_entry) list;
};
/*----------------------------------------------------------------------------*/
struct route_entry {
	uint32_t ip;
	int prefix;
	uint32_t mask;
	uint32_t masked_ip;
	int nif;
	char dev_name[WORD_LEN];

	TAILQ_ENTRY(route_entry) link;
};
struct route_conf {
	int num;
	struct route_entry *ent[MAX_ROUTE_ENTRY];
	TAILQ_HEAD(, route_entry) list;
};
/*----------------------------------------------------------------------------*/
struct nic_forward_entry {
	char nif_in[WORD_LEN];
	char nif_out[WORD_LEN];

	TAILQ_ENTRY(nic_forward_entry) link;
};
struct nic_forward_conf {
	int num;
	struct nic_forward_entry *ent[MAX_FORWARD_ENTRY];
	int nic_fwd_table[MAX_FORWARD_ENTRY];
	TAILQ_HEAD(, nic_forward_entry) list;
};
/*----------------------------------------------------------------------------*/
struct app_conf {
	char type[WORD_LEN];
	char run[STR_LEN];
	uint64_t cpu_mask;
	int ip_forward;

	int app_argc;
	char *app_argv[MOS_APP_ARGC];
};
/*----------------------------------------------------------------------------*/
struct mos_conf {
	int num_cores;
	int nb_mem_channels;
	int max_concurrency;
	int no_ring_buffers;
	int rmem_size;
	int wmem_size;
	int tcp_tw_interval;
	int tcp_timeout;
	int multiprocess;
	int multiprocess_curr_core;
	int multiprocess_is_master;
	char mos_log[STR_LEN];
	char stat_print[STR_LEN];
	char port[STR_LEN];
	uint64_t cpu_mask;
	int forward;

	struct netdev_conf *netdev_table;
	struct arp_conf *arp_table;
	struct route_conf *route_table;
	struct nic_forward_conf *nic_forward_table;

	struct conf_block *netdev;
	struct conf_block *arp;
	struct conf_block *route;
	struct conf_block *nic_forward;
};
/*----------------------------------------------------------------------------*/
TAILQ_HEAD(, conf_block) g_free_blkh;
struct config {
	TAILQ_HEAD(, conf_block) app_blkh;
	TAILQ_HEAD(, conf_block) mos_blkh;

	struct mos_conf *mos;
} g_config;

int num_cpus;
int num_queues;
int num_devices;
//struct ps_device devices[MAX_DEVICES];

int num_devices_attached;
int devices_attached[MAX_DEVICES];
/*----------------------------------------------------------------------------*/
int
LoadConfigurationUpperHalf(const char *fname);

void
LoadConfigurationLowerHalf(void);

/* set configurations from the setted 
   interface information */
int
SetInterfaceInfo();

/* set configurations from the files */
int 
SetRoutingTable();

int 
LoadARPTable();

/* print setted configuration */
void
PrintConf(struct config *conf);

/* set socket modes */
int
SetSocketMode(int8_t socket_mode);

void
ParseMACAddress(unsigned char *haddr, char *haddr_str);

int
ParseIPAddress(uint32_t *ip_addr, char *ip_str);

void
FreeConfigResources();

int
mystrtol(const char *nptr, int base);
/*----------------------------------------------------------------------------*/
#endif /* __CONFIG_H_ */
