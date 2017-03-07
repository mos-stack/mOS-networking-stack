#include <stdlib.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>

#include "mtcp.h"
#include "config.h"
#include "tcp_in.h"
#include "arp.h"
#include "debug.h"
#include "mtcp_util.h"
/* for setting up io modules */
#include "io_module.h"
/* for if_nametoindex */
#include <net/if.h>

#define MAX_PROCLINE_LEN 1024

#ifdef DARWIN
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wempty-body"
#endif

/*----------------------------------------------------------------------------*/
int8_t end_app_exists = 0;
int8_t mon_app_exists = 0;
addr_pool_t ap[ETH_NUM] = {NULL};
char *file = NULL;
/*----------------------------------------------------------------------------*/
/* return 0 on failure */
#define MATCH_ITEM(name, item) \
	((strncmp(#name, item, strlen(#name)) == 0) \
	 && (!isalnum(item[strlen(#name)])))

#define TRY_ASSIGN_NUM(name, base, item, value) \
	((strncmp(#name, item, strlen(#name)) == 0) \
	 && (!isalnum(item[strlen(#name)])) \
	 && (sscanf(value, \
			(sizeof((base)->name) == sizeof(char)) ? "%hhi" : \
			(sizeof((base)->name) == sizeof(short)) ? "%hi" : \
			(sizeof((base)->name) == sizeof(int)) ? "%i" : \
			(sizeof((base)->name) == sizeof(long)) ? "%li" : \
			(sizeof((base)->name) == sizeof(long long)) ? "%lli" : "ERROR", \
			    &(base)->name) > 0))

#define TRY_ASSIGN_STR(name, base, item, value) \
	(((strncmp(#name, item, strlen(#name)) == 0) \
	  && (!isalnum(item[strlen(#name)])) \
	  && (strlen(value) < sizeof((base)->name))) ? \
	 (strcpy((base)->name, value), 1) : 0)

#define LINE_FOREACH(line, llen, buf, blen) \
	for(line = buf, \
		llen = ((strchr(line, '\n') == NULL) ? (buf + blen - line) \
											 : strchr(line, '\n') - line); \
		line + llen < buf + blen; \
		line += llen + 1, \
		llen = ((strchr(line, '\n') == NULL) ? (buf + blen - line) \
											 : strchr(line, '\n') - line)) \
/*----------------------------------------------------------------------------*/
static int
SetMultiProcessSupport(char *multiprocess_details)
{
	char *token = " =";
	char *sample;
	char *saveptr;

	TRACE_CONFIG("Loading multi-process configuration\n");

	sample = strtok_r(multiprocess_details, token, &saveptr);
	if (sample == NULL) {
		TRACE_CONFIG("No option for multi-process support given!\n");
		return -1;
	}
	g_config.mos->multiprocess_curr_core = mystrtol(sample, 10);
	
	sample = strtok_r(NULL, token, &saveptr);
	if (sample != NULL && !strcmp(sample, "master"))
		g_config.mos->multiprocess_is_master = 1;
	
	return 0;
}
/*----------------------------------------------------------------------------*/
static int
DetectWord(char *buf, int len, char **word, int *wlen)
{
	int i;
	for (i = 0; i < len; i++) {
		if (isspace(buf[i]))
			continue;

		if (isalpha(buf[i])) {
			*word = &buf[i];
			break;
		} else
			/* not word */
			return -1;
	}

	if (i == len)
		return -1;

	for (*wlen = 0; *wlen < len; (*wlen)++) {
		if (isalnum((*word)[*wlen]) || (*word)[*wlen] == '_')
			continue;

		assert(*wlen != 0);
		break;
	}

	assert(*word >= buf && *word + *wlen <= buf + len);

	return 0;
}
/*----------------------------------------------------------------------------*/
static int
ReadItemValue(char *line, int llen, char *item, int ilen, char *value, int vlen)
{
	const char *end = &line[llen];
	char *word = NULL;
	int wlen = 0;

	if (DetectWord(line, llen, &word, &wlen) < 0 || wlen > ilen)
		return -1;

	line = word + wlen;

	/* skip space */
	while (line < end && isspace(*line))
		line++;

	if (*(line++) != '=')
		return -1;

	while (line < end && isspace(*line))
		line++;

	if (end - line > vlen)
		return -1;

	while (isspace(*(end - 1)))
		end--;

	if (end <= line)
		return -1;

	strncpy(item, word, wlen);

	strncpy(value, line, (size_t)(end - line));

	return 0;
}
/*----------------------------------------------------------------------------*/
static void
FeedAppConfLine(struct conf_block *blk, char *line, int len)
{
	struct app_conf * const conf = (struct app_conf *)blk->conf;

	char item[WORD_LEN + 1] = {0};
	char value[STR_LEN + 1] = {0};

	if (ReadItemValue(line, len, item, WORD_LEN, value, STR_LEN) < 0)
		return;

	if      (TRY_ASSIGN_STR(type,       conf, item, value));
	else if (TRY_ASSIGN_STR(run,        conf, item, value)) {
		StrToArgs(conf->run, &conf->app_argc, conf->app_argv, MOS_APP_ARGC);
#if 0
		conf->app_argv[conf->app_argc++] = strtok(conf->run, " \t\n\r");
		while (conf->app_argc < MOS_APP_ARGC &&
				(conf->app_argv[conf->app_argc] = strtok(NULL, " \t\n\r")))
			conf->app_argc++;
#endif
	} else if (TRY_ASSIGN_NUM(cpu_mask,   conf, item, value));
	else if (TRY_ASSIGN_NUM(ip_forward, conf, item, value));
}
/*----------------------------------------------------------------------------*/
static void
FeedMosConfLine(struct conf_block *blk, char *line, int len)
{
	struct mos_conf * const conf = (struct mos_conf *)blk->conf;

	char item[WORD_LEN + 1] = {0};
	char value[STR_LEN + 1] = {0};

	if (ReadItemValue(line, len, item, WORD_LEN, value, STR_LEN) < 0)
		return;

	if (TRY_ASSIGN_NUM(nb_mem_channels, conf, item, value));
	else if (TRY_ASSIGN_NUM(forward,         conf, item, value));
	else if (TRY_ASSIGN_NUM(max_concurrency, conf, item, value));
	else if (TRY_ASSIGN_NUM(rmem_size,       conf, item, value));
	else if (TRY_ASSIGN_NUM(wmem_size,       conf, item, value));
	else if (TRY_ASSIGN_NUM(tcp_tw_interval, conf, item, value))
		g_config.mos->tcp_tw_interval =
			SEC_TO_USEC(g_config.mos->tcp_tw_interval) / TIME_TICK;
	else if (TRY_ASSIGN_NUM(tcp_timeout,     conf, item, value))
		g_config.mos->tcp_timeout =
			SEC_TO_USEC(g_config.mos->tcp_timeout) / TIME_TICK;
	else if (TRY_ASSIGN_NUM(no_ring_buffers, conf, item, value));
	else if (TRY_ASSIGN_STR(mos_log,         conf, item, value));
	else if (TRY_ASSIGN_STR(stat_print,      conf, item, value));
	else if (TRY_ASSIGN_STR(port,            conf, item, value));
	else if (strcmp(item, "multiprocess") == 0) {
		conf->multiprocess = 1;
		SetMultiProcessSupport(value);
	}
}
/*----------------------------------------------------------------------------*/
static void
FeedNetdevConfLine(struct conf_block *blk, char *line, int len)
{
	struct netdev_conf * const conf = (struct netdev_conf *)blk->conf;

#ifndef DARWIN
	int i;
#endif
	uint64_t cpu_mask;
	char *word = NULL;
	int wlen;

	if (DetectWord(line, len, &word, &wlen) < 0 || wlen > WORD_LEN || wlen <= 0)
		return;

	line = word + wlen;

	if (sscanf(line, "%li", &cpu_mask) <= 0)
		return;

	struct netdev_entry *ent = calloc(1, sizeof(struct netdev_entry));
	if (!ent) {
		TRACE_ERROR("Could not allocate memory for netdev_entry!\n");
		exit(EXIT_FAILURE);
	}

	strncpy(ent->dev_name, word, wlen);
	ent->cpu_mask = cpu_mask;
	g_config.mos->cpu_mask |= cpu_mask;

	strncpy(ent->ifr.ifr_name, ent->dev_name, IFNAMSIZ-1);
	ent->ifr.ifr_name[IFNAMSIZ-1] = '\0';

	/* Create socket */
	int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}			
	
	/* getting address */
	if (ioctl(sock, SIOCGIFADDR, &ent->ifr) == 0 ) {
		struct in_addr sin = ((struct sockaddr_in *)&ent->ifr.ifr_addr)->sin_addr;
		ent->ip_addr = *(uint32_t *)&sin;
	}

	/* Net MASK */
	if (ioctl(sock, SIOCGIFNETMASK, &ent->ifr) == 0) {
		struct in_addr sin = ((struct sockaddr_in *)&ent->ifr.ifr_addr)->sin_addr;
		ent->netmask = *(uint32_t *)&sin;
	}

#ifdef DARWIN
	/* FIXME: How can I retrieve a mac address in MAC OS? */
#else
	if (ioctl(sock, SIOCGIFHWADDR, &ent->ifr) == 0 ) {
		for (i = 0; i < 6; i ++) {
			ent->haddr[i] = ent->ifr.ifr_addr.sa_data[i];
		}
	}
#endif

	close(sock);

	ent->ifindex = -1;

	TAILQ_INSERT_TAIL(&conf->list, ent, link);
	conf->ent[conf->num] = ent;
	conf->num++;
}
/*----------------------------------------------------------------------------*/
static void
FeedArpConfLine(struct conf_block *blk, char *line, int len)
{
	struct arp_conf * const conf = (struct arp_conf *)blk->conf;

	char address[WORD_LEN];
	int prefix;
	uint8_t haddr[ETH_ALEN] = {0};

	/* skip first space */
	while (isspace(*line))
		line++, len--;

	if (sscanf(line, "%[0-9.]/%d %hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
				address, &prefix, &haddr[0], &haddr[1], &haddr[2],
				&haddr[3], &haddr[4], &haddr[5]) != 8)
		return;

	struct _arp_entry *ent = calloc(1, sizeof(struct _arp_entry));
	if (!ent) {
		TRACE_ERROR("Could not allocate memory for arp_entry!\n");
		exit(EXIT_FAILURE);
	}

	ent->ip = inet_addr(address);
	ent->prefix = prefix;
	ent->mask = htonl((prefix == 0) ? 0 : ((-1) << (32 - prefix)));
	ent->masked_ip = ent->mask & ent->ip;
	memcpy(ent->haddr, haddr, ETH_ALEN);
	TAILQ_INSERT_TAIL(&conf->list, ent, link);
	conf->ent[conf->num] = ent;
	conf->num++;
}
/*----------------------------------------------------------------------------*/
static void
FeedRouteConfLine(struct conf_block *blk, char *line, int len)
{
	struct route_conf * const conf = (struct route_conf *)blk->conf;

	char address[WORD_LEN], dev_name[WORD_LEN];
	int prefix;

	/* skip first space */
	while (isspace(*line))
		line++, len--;

	if (sscanf(line, "%[0-9.]/%d %[^ ^\n^\t]", address, &prefix, dev_name) != 3)
		return;

	struct route_entry *ent = calloc(1, sizeof(struct route_entry));
	if (!ent) {
		TRACE_ERROR("Could not allocate memory for route_entry!\n");
		exit(EXIT_FAILURE);
	}

	ent->ip = inet_addr(address);
	ent->mask = htonl((prefix == 0) ? 0 : ((-1) << (32 - prefix)));
	ent->masked_ip = ent->mask & ent->ip;
	ent->prefix = prefix;
	ent->nif = -1;
	strcpy(ent->dev_name, dev_name);

	TAILQ_INSERT_TAIL(&conf->list, ent, link);
	conf->ent[conf->num] = ent;
	conf->num++;
}
/*----------------------------------------------------------------------------*/
static void
FeedNICFwdConfLine(struct conf_block *blk, char *line, int len)
{
	struct nic_forward_conf * const conf = (struct nic_forward_conf *)blk->conf;
	char dev_name_in[WORD_LEN];
	char dev_name_out[WORD_LEN];

	/* skip first space */
	while (isspace(*line))
		line++, len--;

	if (sscanf(line, "%[^ ^\n^\t] %[^ ^\n^\t]", dev_name_in, dev_name_out) != 2)
		return;

	struct nic_forward_entry *ent = calloc(1, sizeof(struct nic_forward_entry));
	if (!ent) {
		TRACE_ERROR("Could not allocate memory for nic forward entry!\n");
		exit(EXIT_FAILURE);
	}

	strcpy(ent->nif_in, dev_name_in);
	strcpy(ent->nif_out, dev_name_out);
	TAILQ_INSERT_TAIL(&conf->list, ent, link);
	conf->ent[conf->num] = ent;
	conf->num++;
}
/*----------------------------------------------------------------------------*/
static void
MosConfAddChild(struct conf_block *blk, struct conf_block *child)
{
	struct mos_conf * const conf = (struct mos_conf *)blk->conf;

	if (strcmp(child->name, NETDEV_BLOCK_NAME) == 0) {
		conf->netdev = child;
		conf->netdev_table = (struct netdev_conf *)child->conf;
	} else if (strcmp(child->name, ARP_BLOCK_NAME) == 0) {
		conf->arp = child;
		conf->arp_table = (struct arp_conf *)child->conf;
	} else if (strcmp(child->name, ROUTE_BLOCK_NAME) == 0) {
		conf->route = child;
		conf->route_table = (struct route_conf *)child->conf;
	} else if (strcmp(child->name, FORWARD_BLOCK_NAME) == 0) {
		conf->nic_forward = child;
		conf->nic_forward_table = (struct nic_forward_conf *)child->conf;
	} else
		return;
}
/*----------------------------------------------------------------------------*/
static int
AppConfIsValid(struct conf_block *blk)
{
	struct app_conf * const conf = (struct app_conf *)blk->conf;

	if (conf->app_argc <= 0)
		return 0;

	return 1;
}
/*----------------------------------------------------------------------------*/
static int
MosConfIsValid(struct conf_block *blk)
{
	return 1;
}
/*----------------------------------------------------------------------------*/
static int
NetdevConfIsValid(struct conf_block *blk)
{
	return 1;
}
/*----------------------------------------------------------------------------*/
static int
ArpConfIsValid(struct conf_block *blk)
{
	return 1;
}
/*----------------------------------------------------------------------------*/
static int
RouteConfIsValid(struct conf_block *blk)
{
	return 1;
}
/*----------------------------------------------------------------------------*/
static int
NICFwdConfIsValid(struct conf_block *blk)
{
	return 1;
}
/*----------------------------------------------------------------------------*/
static void
NetdevConfPrint(struct conf_block *blk)
{
	struct netdev_conf * const conf = (struct netdev_conf *)blk->conf;

	printf(" +===== Netdev configuration (%d entries) =====\n",
			conf->num);

	struct netdev_entry *walk;
	TAILQ_FOREACH(walk, &conf->list, link) {
		printf(" | %s(idx: %d, HADDR: %02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX) maps to CPU 0x%016lX\n",
				walk->dev_name, walk->ifindex,
				walk->haddr[0], walk->haddr[1], walk->haddr[2],
				walk->haddr[3], walk->haddr[4], walk->haddr[5],
				walk->cpu_mask);
	}
	printf(" |\n");
}
/*----------------------------------------------------------------------------*/
static void
ArpConfPrint(struct conf_block *blk)
{
	struct arp_conf * const conf = (struct arp_conf *)blk->conf;

	printf(" +===== Static ARP table configuration (%d entries) =====\n",
			conf->num);

	struct _arp_entry *walk;
	TAILQ_FOREACH(walk, &conf->list, link) {
		printf(" | IP: 0x%08X, NETMASK: 0x%08X, "
			   "HADDR: %02hhX:%02hhX:%02hhX:%02hhX:%02hhX:%02hhX\n",
			   ntohl(walk->ip), ntohl(walk->mask),
			   walk->haddr[0], walk->haddr[1], walk->haddr[2],
			   walk->haddr[3], walk->haddr[4], walk->haddr[5]);
	}
	printf(" |\n");
}
/*----------------------------------------------------------------------------*/
static void
RouteConfPrint(struct conf_block *blk)
{
	struct route_conf * const conf = (struct route_conf *)blk->conf;

	printf(" +===== Routing table configuration (%d entries) =====\n",
			conf->num);

	struct route_entry *walk;
	TAILQ_FOREACH(walk, &conf->list, link) {
		printf(" | IP: 0x%08X, NETMASK: 0x%08X, INTERFACE: %s(idx: %d)\n",
			   ntohl(walk->ip), ntohl(walk->mask), walk->dev_name, walk->nif);
	}
	printf(" |\n");
}
/*----------------------------------------------------------------------------*/
static void
NICFwdConfPrint(struct conf_block *blk)
{
	int i;
	struct nic_forward_conf * const conf = (struct nic_forward_conf *)blk->conf;

	printf(" +===== NIC Forwarding table configuration (%d entries) =====\n",
			conf->num);

	struct nic_forward_entry *walk;
	TAILQ_FOREACH(walk, &conf->list, link) {
		printf(" | NIC Forwarding Entry: %s <---> %s",
		       walk->nif_in, walk->nif_out);
	}
	printf(" |\n");

	printf(" | NIC Forwarding Index Table: |\n");
	
	for (i = 0; i < MAX_FORWARD_ENTRY; i++)
		printf( " | %d --> %d | \n", i, conf->nic_fwd_table[i]);
}
/*----------------------------------------------------------------------------*/
static void
AppConfPrint(struct conf_block *blk)
{
	struct app_conf * const conf = (struct app_conf *)blk->conf;

	printf("===== Application configuration =====\n");
	printf("| type:       %s\n", conf->type);
	printf("| run:        %s\n", conf->run);
	printf("| cpu_mask:   0x%016lX\n", conf->cpu_mask);
	printf("| ip_forward: %s\n", conf->ip_forward ? "forward" : "drop");
	printf("\n");
}
/*----------------------------------------------------------------------------*/
static void
MosConfPrint(struct conf_block *blk)
{
	struct mos_conf * const conf = (struct mos_conf *)blk->conf;

	printf("===== MOS configuration =====\n");
	printf("| num_cores:       %d\n", conf->num_cores);
	printf("| nb_mem_channels: %d\n", conf->nb_mem_channels);
	printf("| max_concurrency: %d\n", conf->max_concurrency);
	printf("| rmem_size:       %d\n", conf->rmem_size);
	printf("| wmem_size:       %d\n", conf->wmem_size);
	printf("| tcp_tw_interval: %d\n", conf->tcp_tw_interval);
	printf("| tcp_timeout:     %d\n", conf->tcp_timeout);
	printf("| multiprocess:    %s\n", conf->multiprocess ? "true" : "false");
	printf("| mos_log:         %s\n", conf->mos_log);
	printf("| stat_print:      %s\n", conf->stat_print);
	printf("| forward:         %s\n", conf->forward ? "forward" : "drop");
	printf("|\n");
	if (conf->netdev)
		conf->netdev->print(conf->netdev);
	if (conf->arp)
		conf->arp->print(conf->arp);
	if (conf->route)
		conf->route->print(conf->route);
	if (conf->nic_forward)
		conf->nic_forward->print(conf->nic_forward);
	printf("\n");
}
/*----------------------------------------------------------------------------*/
static void
InitAppBlock(struct config *config, struct conf_block *blk)
{
	assert(blk);

	blk->name = APP_BLOCK_NAME;

	blk->feed = FeedAppConfLine;
	blk->addchild = NULL;
	blk->isvalid = AppConfIsValid;
	blk->print = AppConfPrint;

	struct app_conf *conf = calloc(1, sizeof(struct app_conf));
	if (conf == NULL) {
		TRACE_ERROR("Could not allocate memory for app_conf!\n");
		exit(EXIT_FAILURE);
	}
	/* set default values */
	conf->cpu_mask = -1;
	conf->ip_forward = 1;
	conf->app_argc = 0;
	blk->conf = conf;

	blk->list = (typeof(blk->list))&config->app_blkh;
}
/*----------------------------------------------------------------------------*/
static void
InitMosBlock(struct config *config, struct conf_block *blk)
{
	assert(blk);

	blk->name = MOS_BLOCK_NAME;

	blk->feed = FeedMosConfLine;
	blk->addchild = MosConfAddChild;
	blk->isvalid = MosConfIsValid;
	blk->print = MosConfPrint;

	struct mos_conf *conf = calloc(1, sizeof(struct mos_conf));
	if (conf == NULL) {
		TRACE_ERROR("Could not allocate memory for mos_conf!\n");
		exit(EXIT_FAILURE);
	}
	/* set default values */
	conf->forward = 1;
	conf->nb_mem_channels = 0;
	conf->max_concurrency = 100000;
	conf->no_ring_buffers = 0;
	conf->rmem_size = 8192;
	conf->wmem_size = 8192;
	conf->tcp_tw_interval = SEC_TO_USEC(TCP_TIMEWAIT) / TIME_TICK;
	conf->tcp_timeout = SEC_TO_USEC(TCP_TIMEOUT) / TIME_TICK;
	conf->cpu_mask = 0;
	blk->conf = conf;

	blk->list = (typeof(blk->list))&config->mos_blkh;
	config->mos = conf;
}
/*----------------------------------------------------------------------------*/
static void
InitNetdevBlock(struct config *config, struct conf_block *blk)
{
	assert(blk);

	blk->name = NETDEV_BLOCK_NAME;

	blk->feed = FeedNetdevConfLine;
	blk->addchild = NULL;
	blk->isvalid = NetdevConfIsValid;
	blk->print = NetdevConfPrint;

	struct netdev_conf *conf = calloc(1, sizeof(struct netdev_conf));
	if (conf == NULL) {
		TRACE_ERROR("Could not allocate memory for netdev_conf!\n");
		exit(EXIT_FAILURE);
	}
	TAILQ_INIT(&conf->list);
	blk->conf = conf;

	blk->list = NULL;
}
/*----------------------------------------------------------------------------*/
static void
FetchARPKernelEntries(struct arp_conf * const config)
{
#define	_PATH_PROCNET_ARP		"/proc/net/arp"
#define	DPDK_PREFIX			"dpdk"
#define DPDK_PREFIX_LEN			4
#define LINE_LEN			200
#define ENTRY_LEN			25

	FILE *fp;
	char ip[ENTRY_LEN];
	char hwa[ENTRY_LEN];
	char mask[ENTRY_LEN];
	char dev[WORD_LEN];
	char line[LINE_LEN];
	int type, flags, num;

	if ((fp = fopen(_PATH_PROCNET_ARP, "r")) == NULL) {
		perror(_PATH_PROCNET_ARP);
		exit(EXIT_FAILURE);
	}

	/* Bypass header -- read until newline */
	if (fgets(line, sizeof(line), fp) != (char *) NULL) {
		strcpy(mask, "-");
		strcpy(dev, "-");
		/* Read the ARP cache entries. */
		for (; fgets(line, sizeof(line), fp);) {

			num = sscanf(line, "%s 0x%x 0x%x %100s %100s %100s\n",
				     ip, &type, &flags, hwa, mask, dev);
			if (num < 6)
				break;
			
			/* if the user specified device differs, skip it */
			if (strncmp(dev, DPDK_PREFIX, DPDK_PREFIX_LEN))
				continue;
			
			/* if the entry has not expired/tagged for removal then... */
			if (flags != 0x00) {
				/* add the new arp entry in MOS database */
				struct _arp_entry *ent = calloc(1, sizeof(struct _arp_entry));
				if (!ent) {
					TRACE_ERROR("Can't allocate memory for arp_entry\n");
					exit(EXIT_FAILURE);
				}
				uint8_t haddr[ETH_ALEN] = {0};
				if (sscanf(hwa, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
					   &haddr[0], &haddr[1], &haddr[2],
					   &haddr[3], &haddr[4], &haddr[5]) != 6) {
					TRACE_ERROR("Error reading the ARP entry\n");
					exit(EXIT_FAILURE);
				}
				ent->ip = inet_addr(ip);
				ent->prefix = 32;
				ent->mask = htonl((ent->prefix == 0) ? 0 : ((-1) << (32 - ent->prefix)));
				ent->masked_ip = ent->mask & ent->ip;
				memcpy(ent->haddr, haddr, ETH_ALEN);
				TAILQ_INSERT_TAIL(&config->list, ent, link);
				config->ent[config->num] = ent;
				config->num++;
			}
		}
	}
	
	fclose(fp);
}
/*----------------------------------------------------------------------------*/
static void
FetchRouteKernelEntries(struct route_conf * const config)
{
#define	_PATH_PROCNET_ROUTE		"/proc/net/route"
#define	DPDK_PREFIX			"dpdk"
#define DPDK_PREFIX_LEN			4
	
	FILE *fp;
	uint32_t gate;
	uint32_t dest;
	uint32_t mask;
	char dev[WORD_LEN];
	char line[LINE_LEN];
	char mtu[ENTRY_LEN];
	char win[ENTRY_LEN];
	char irtt[ENTRY_LEN];
	int flags, num, cnt, use, metric;
	
	if ((fp = fopen(_PATH_PROCNET_ROUTE, "r")) == NULL) {
		perror(_PATH_PROCNET_ARP);
		exit(EXIT_FAILURE);
	}

	/* Bypass header -- read until newline */
	if (fgets(line, sizeof(line), fp) != (char *) NULL) {
		/* Read the route table entries. */
		for (; fgets(line, sizeof(line), fp);) {

			num = sscanf(line, "%s %08X %08X %d %d %d %d %08X %s %s %s\n",
				     dev,
				     &dest,
				     &gate,
				     &flags, &cnt, &use, &metric,
				     &mask,
				     mtu, win, irtt);

			if (num < 11)
				break;
#if 0
			/* if the user specified device differs, skip it */
			if (strncmp(dev, DPDK_PREFIX, DPDK_PREFIX_LEN))
				continue;
#endif
			struct route_entry *ent = calloc(1, sizeof(struct route_entry));
			if (!ent) {
				TRACE_ERROR("Could not allocate memory for route_entry!\n");
				exit(EXIT_FAILURE);
			}
			
			ent->ip = dest;
			ent->prefix = 32 - __builtin_clz(mask);
			ent->mask = mask;
			ent->masked_ip = ent->mask & ent->ip;
			strcpy(ent->dev_name, dev);
			TAILQ_INSERT_TAIL(&config->list, ent, link);
			config->ent[config->num] = ent;
			config->num++;
		}
	}
	
	fclose(fp);
}
/*----------------------------------------------------------------------------*/
static void
InitArpBlock(struct config *config, struct conf_block *blk)
{
	assert(blk);

	blk->name = ARP_BLOCK_NAME;

	blk->feed = FeedArpConfLine;
	blk->addchild = NULL;
	blk->isvalid = ArpConfIsValid;
	blk->print = ArpConfPrint;

	struct arp_conf *conf = calloc(1, sizeof(struct arp_conf));
	if (conf == NULL) {
		TRACE_ERROR("Could not allocate memory for arp_conf!\n");
		exit(EXIT_FAILURE);
	}
	TAILQ_INIT(&conf->list);
	blk->conf = conf;

	blk->list = NULL;
	config->mos->arp = blk;

	/* fetch relevant ARP entries for dpdk? from kernel tables */
	FetchARPKernelEntries(conf);
}
/*----------------------------------------------------------------------------*/
static void
InitRouteBlock(struct config *config, struct conf_block *blk)
{
	assert(blk);

	blk->name = ROUTE_BLOCK_NAME;

	blk->feed = FeedRouteConfLine;
	blk->addchild = NULL;
	blk->isvalid = RouteConfIsValid;
	blk->print = RouteConfPrint;

	struct route_conf *conf = calloc(1, sizeof(struct route_conf));
	if (conf == NULL) {
		TRACE_ERROR("Could not allocate memory for route_conf!\n");
		exit(EXIT_FAILURE);
	}
	TAILQ_INIT(&conf->list);
	blk->conf = conf;

	blk->list = NULL;
	config->mos->route = blk;

	/* fetch relevant route entries for dpdk? from kernel tables */
	FetchRouteKernelEntries(conf);
}
/*----------------------------------------------------------------------------*/
static void
InitNICForwardBlock(struct config *config, struct conf_block *blk)
{
	int i;
	assert(blk);
	blk->name = FORWARD_BLOCK_NAME;

	blk->feed = FeedNICFwdConfLine;
	blk->addchild = NULL;
	blk->isvalid = NICFwdConfIsValid;
	blk->print = NICFwdConfPrint;

	struct nic_forward_conf *conf = calloc(1, sizeof(struct nic_forward_conf));
	if (conf == NULL) {
		TRACE_ERROR("Could not allocate memory for nic_forward_conf!\n");
		exit(EXIT_FAILURE);
	}
	for (i = 0; i < MAX_FORWARD_ENTRY; i++)
		conf->nic_fwd_table[i] = -1;

	TAILQ_INIT(&conf->list);
	blk->conf = conf;

	blk->list = NULL;
	config->mos->nic_forward = blk;
}
/*----------------------------------------------------------------------------*/
void
PrintConf(struct config *conf)
{
	struct conf_block *walk;
	TAILQ_FOREACH(walk, &conf->app_blkh, link) {
		if (walk->print)
			walk->print(walk);
	}
	
	TAILQ_FOREACH(walk, &conf->mos_blkh, link) {
		if (walk->print)
			walk->print(walk);
	}
}
/*----------------------------------------------------------------------------*/
static void
CheckConfValidity(struct config *conf)
{
	struct conf_block *walk;
	TAILQ_FOREACH(walk, &conf->app_blkh, link) {
		if (!walk->isvalid || !walk->isvalid(walk))
			goto __error;
	}

	TAILQ_FOREACH(walk, &conf->mos_blkh, link) {
		struct conf_block *child;

		if (!walk->isvalid || !walk->isvalid(walk))
			goto __error;

		child = ((struct mos_conf *)walk->conf)->netdev;
		if (!child->isvalid || !child->isvalid(child))
			goto __error;

		child = ((struct mos_conf *)walk->conf)->arp;
		if (!child->isvalid || !child->isvalid(child))
			goto __error;

		child = ((struct mos_conf *)walk->conf)->route;
		if (!child->isvalid || !child->isvalid(child))
			goto __error;
	}

	return;

__error:
	printf("!!!!! Configuration validity check failure !!!!!\n");
	if (walk && walk->print)
		walk->print(walk);
	exit(0);
}
/*----------------------------------------------------------------------------*/
static char *
ReadConf(const char *fname)
{
	ssize_t hav_read = 0, rc;
	FILE *fp = fopen(fname, "r");
	if (fp == NULL) {
		TRACE_ERROR("Cannot open the config file %s\n", fname);
		exit(-1);		
	}
	
	/* find out the size of file */
	fseek(fp, 0L, SEEK_END);
	int size = ftell(fp);
	fseek(fp, 0L, SEEK_SET);

	file = calloc(1, size + 1);
	if (file == NULL) {
		TRACE_ERROR("Can't allocate memory for file!\n");
		exit(EXIT_FAILURE);
	}

	file[size] = '\0';

	while (hav_read < size) {
		rc = fread(file, 1, size, fp);
		/* sanity check */
		if (rc <= 0) break;
		hav_read += rc;
	}

	fclose(fp);

	return file;
}
/*----------------------------------------------------------------------------*/
static char *
PreprocessConf(char *raw)
{
	char *line;
	int llen;

	int len = strlen(raw);

	LINE_FOREACH(line, llen, raw, len) {
		int i, iscomment = 0;
		for (i = 0; i < llen; i++) {
			if (!iscomment && line[i] == '#')
				iscomment = 1;
			if (iscomment)
				line[i] = ' ';
		}
	}
	return raw;
}
/*----------------------------------------------------------------------------*/
static void
InitConfig(struct config *config)
{
	int i;
	struct conf_block *blk;

	TAILQ_INIT(&g_free_blkh);
	TAILQ_INIT(&config->app_blkh);
	TAILQ_INIT(&config->mos_blkh);

	for (i = 0; i < MAX_APP_BLOCK; i++) {
		/* Allocate app conf_block */
		blk = calloc(1, sizeof(struct conf_block));
		if (blk == NULL) goto init_config_err;
		InitAppBlock(config, blk);
		TAILQ_INSERT_TAIL(&g_free_blkh, blk, link);

		/* Allocate netdev conf_block */
		blk = calloc(1, sizeof(struct conf_block));
		if (blk == NULL) goto init_config_err;
		InitNetdevBlock(config, blk);
		TAILQ_INSERT_TAIL(&g_free_blkh, blk, link);
	}

	for (i = 0; i < MAX_MOS_BLOCK; i++) {
		/* Allocate mos conf_block */
		blk = calloc(1, sizeof(struct conf_block));
		if (blk == NULL) goto init_config_err;
		InitMosBlock(config, blk);
		TAILQ_INSERT_TAIL(&g_free_blkh, blk, link);

		/* Allocate arp conf_block */
		blk = calloc(1, sizeof(struct conf_block));
		if (blk == NULL) goto init_config_err;
		InitArpBlock(config, blk);
		TAILQ_INSERT_TAIL(&g_free_blkh, blk, link);

		/* Allocate route conf_block */
		blk = calloc(1, sizeof(struct conf_block));
		if (blk == NULL) goto init_config_err;
		InitRouteBlock(config, blk);
		TAILQ_INSERT_TAIL(&g_free_blkh, blk, link);

		/* Allocate nic_forward conf_block */
		blk = calloc (1, sizeof(struct conf_block));
		if (blk == NULL) goto init_config_err;
		InitNICForwardBlock(config, blk);
		TAILQ_INSERT_TAIL(&g_free_blkh, blk, link);
	}
	return;
 init_config_err:
	TRACE_ERROR("Can't allocate memory for blk_entry!\n");
	exit(EXIT_FAILURE);
}
/*----------------------------------------------------------------------------*/
static struct conf_block *
AllocateBlock(char *name, int len)
{
	struct conf_block *walk, *tmp;

	for (walk = TAILQ_FIRST(&g_free_blkh); walk != NULL; walk = tmp) {
		tmp = TAILQ_NEXT(walk, link);
		if (len == strlen(walk->name) && strncmp(walk->name, name, len) == 0) {
			TAILQ_REMOVE(&g_free_blkh, walk, link);
			if (walk->list)
				TAILQ_INSERT_TAIL(walk->list, walk, link);
			return walk;
		}
	}

	return NULL;
}
/*----------------------------------------------------------------------------*/
struct conf_block *
DetectBlock(struct conf_block *blk, char *buf, int len)
{
	int depth = 0;
	char *blkname = NULL, *end = &buf[len];
	int blknamelen;
	struct conf_block *nblk;

	/* skip first space */
	while (buf < end && isspace(*buf))
		buf++;

	if (DetectWord(buf, len, &blkname, &blknamelen) < 0
			|| blkname != buf)
		/* Failed to detect conf_block name */
		return NULL;

	/* fast forward buffer */
	buf += blknamelen;

	/* skip space */
	while (buf < end && isspace(*buf))
		buf++;

	/* buf must be '{' */
	if (buf >= end || *buf != '{')
		return NULL;

	buf++;   /* skip '{' */
	while (buf < end && isspace(*buf))
		buf++; /* skip space */
	depth++; /* Now in first parenthesis */

	/* Now, the `buf` points the first byte inside conf_block */

	for (len = 0; &buf[len] < end; len++) {
		if (buf[len] == '{')
			depth++;
		else if (buf[len] == '}' && --depth == 0)
				break;
	}

	if (depth != 0)
		/* Failed to find the end of parenthesis */
		return NULL;

	if (!(nblk = AllocateBlock(blkname, blknamelen)))
		return NULL;

	if (blk) {
		assert(blk->addchild);
		blk->addchild(blk, nblk);
	}

	nblk->buf = buf;
	nblk->len = len;

	return nblk;
}
/*----------------------------------------------------------------------------*/
static void
ParseBlock(struct conf_block *blk)
{
	char *line;
	int llen;

	LINE_FOREACH(line, llen, blk->buf, blk->len) {
		struct conf_block *nblk;

		if ((nblk = DetectBlock(blk, line, blk->len - (line - blk->buf)))) {
			ParseBlock(nblk);
			/* skip nested conf_block by fast forwarding line */
			line = &nblk->buf[nblk->len] + 1;
			llen = 0;
		} else
			blk->feed(blk, line, llen);
	}
}
/*----------------------------------------------------------------------------*/
void
PatchCONFIG(struct config *config)
{
	int i;
	char *word, *str, *end;
	int wlen;

	g_config.mos->num_cores = num_cpus;
	word = NULL;

	i = 0;
	struct conf_block *bwalk;
	TAILQ_FOREACH(bwalk, &g_config.app_blkh, link) {
		struct app_conf *app_conf = (struct app_conf *)bwalk->conf;
		g_config.mos->forward = g_config.mos->forward && app_conf->ip_forward;
		if (end_app_exists == 0 && !strcmp(app_conf->type, "end"))
			end_app_exists = 1;
		if (mon_app_exists == 0 && !strcmp(app_conf->type, "monitor"))
			mon_app_exists = 1;
		i++;
	}
	/* turn on monitor mode if end app is not set */
	if (!end_app_exists && !mon_app_exists) mon_app_exists = 1;

	/* stat print */
	str = g_config.mos->stat_print;
	end = str + strlen(str);
	while (DetectWord(str, end - str, &word, &wlen) == 0) {
		for (i = 0; i < g_config.mos->netdev_table->num; i++) {
			if (strncmp(g_config.mos->netdev_table->ent[i]->dev_name, word, wlen) == 0) {
				g_config.mos->netdev_table->ent[i]->stat_print = TRUE;
			}
		}
		str = word + wlen;
	}

}
/*----------------------------------------------------------------------------*/
int 
LoadConfigurationUpperHalf(const char *fname)
{
	char *line;
	int llen;

	char *raw = ReadConf(fname);
	char *preprocessed = PreprocessConf(raw);
	int len = strlen(preprocessed);

	InitConfig(&g_config);

	LINE_FOREACH(line, llen, preprocessed, len) {
		struct conf_block *nblk;

		if ((nblk = DetectBlock(NULL, line, len - (line - preprocessed)))) {
			ParseBlock(nblk);
			/* skip parsed conf_block by fast forwarding line */
			line = &nblk->buf[nblk->len] + 1;
			llen = 0;
		}
	}

	CheckConfValidity(&g_config);

	PatchCONFIG(&g_config);

	//PrintConf(&g_config);

	return 0;
}
/*----------------------------------------------------------------------------*/
void
LoadConfigurationLowerHalf(void)
{
	struct route_conf *route_conf = g_config.mos->route_table;
	struct netdev_conf *netdev_conf = g_config.mos->netdev_table;
	struct nic_forward_conf *nicfwd_conf = g_config.mos->nic_forward_table;
	struct route_entry *rwalk;
	struct netdev_entry *nwalk;
	struct nic_forward_entry *fwalk;
	int nif_in = -1;
	int nif_out = -1;

	TAILQ_FOREACH(rwalk, &route_conf->list, link) {
		TAILQ_FOREACH(nwalk, &netdev_conf->list, link) {
			if (!strcmp(nwalk->dev_name, rwalk->dev_name))
				break;
		}
		if (!nwalk)
			continue;
		if (nwalk->ifindex < 0 &&
			(nwalk->ifindex = current_iomodule_func->get_nif(&nwalk->ifr)) < 0) {
			TRACE_ERROR("Interface '%s' not found\n", nwalk->dev_name);
			exit(EXIT_FAILURE);
		}

		rwalk->nif = nwalk->ifindex;
	}

	if (nicfwd_conf != NULL) {
		TAILQ_FOREACH(fwalk, &nicfwd_conf->list, link) {
			TAILQ_FOREACH(nwalk, &netdev_conf->list, link) {
				if (!strcmp(nwalk->dev_name, fwalk->nif_in))
					nif_in = nwalk->ifindex = current_iomodule_func->get_nif(&nwalk->ifr);
				if (!strcmp(nwalk->dev_name, fwalk->nif_out))
					nif_out = nwalk->ifindex = current_iomodule_func->get_nif(&nwalk->ifr);
			}
			
			if (nif_in != -1)
				nicfwd_conf->nic_fwd_table[nif_in] = nif_out;
			if (nif_out != -1)
				nicfwd_conf->nic_fwd_table[nif_out] = nif_in;
			nif_in = nif_out = -1;
		}
	}
}
/*----------------------------------------------------------------------------*/
void
FreeConfigResources()
{
	if (file) {
		free(file);
		file = NULL;
	}
}
/*----------------------------------------------------------------------------*/
