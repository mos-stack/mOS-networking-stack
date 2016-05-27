#define _LARGEFILE64_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <linux/if_ether.h>
#include <linux/tcp.h>
#include <mos_api.h>
#include <ctype.h>
#include "cpu.h"
#include "http_parsing.h"
#include "debug.h"
#include "applib.h"
/*----------------------------------------------------------------------------*/
/* default configuration file path */
#define MOS_CONFIG_FILE		     "config/mos.conf"
/* max length per line in firewall config file */
#define CONF_MAX_LINE_LEN        1024
/* number of array elements */
#define NELEMS(x)           (sizeof(x) / sizeof(x[0])) 
/* macro to skip spaces */
#define SKIP_SPACES(x) while (*x && isspace((int)*x)) x++;
/* macro to skip characters */
#define SKIP_CHAR(x) while((*x) && !isspace(*x)) x++;
/* macro to skip digit characters */
#define SKIP_DIGIT(x) while((*x) && isdigit(*x)) x++;
/* macro to do netmasking with ip address */
#define IP_NETMASK(x, y) x & (0xFFFFFFFF >> (32 - y));
/* macro to dump error and exit */
#define EXIT_WITH_ERROR(f, m...) {                                 \
		fprintf(stderr, "[%10s:%4d] errno: %u" f, __FUNCTION__, __LINE__, errno, ##m); \
	exit(EXIT_FAILURE);                                            \
}
/* boolean for function return value */
#define SUCCESS 1
#define FAILURE 0
/*----------------------------------------------------------------------------*/
/* firewall rule action */
typedef enum {FRA_INVALID, FRA_ACCEPT, FRA_DROP} FRAction;
#define FR_ACCEPT "ACCEPT"
#define FR_DROP   "DROP"
#define FR_SPORT  "sport:"
#define FR_DPORT  "dport:"
/* firewall rule structure */
#define MAX_RULES 1024
#define MAX_IP_ADDR_LEN 19      /* in CIDR format */
/* all fields are in network byte order */
typedef struct FirewallRule {
	in_addr_t fr_srcIP;         /* source IP */
	int       fr_srcIPmask;     /* source IP netmask */
	in_addr_t fr_dstIP;         /* destination IP */
	int       fr_dstIPmask;     /* destination IP netmask */
	in_port_t fr_srcPort;       /* source port */
	in_port_t fr_dstPort;       /* destination port */
	FRAction  fr_action;        /* action */
	uint32_t  fr_count;         /* packet count */
} FirewallRule;
static FirewallRule g_FWRules[MAX_RULES];
/*----------------------------------------------------------------------------*/
struct thread_context
{
	mctx_t mctx;         /* per-thread mos context */
	int mon_listener;    /* listening socket for flow monitoring */
};
/*----------------------------------------------------------------------------*/
/* Print the entire firewall rule and status table */
static void
DumpFWRuleTable(mctx_t mctx, int sock, int side,
		uint64_t events, filter_arg_t *arg)
{
	int i;
  	FirewallRule *fwr;
	char cip_str[MAX_IP_ADDR_LEN];
	char sip_str[MAX_IP_ADDR_LEN];
	struct timeval tv_1sec = { /* 1 second */
		.tv_sec = 1,
		.tv_usec = 0
	};

	printf("-----------------------------------------------------------------------\n");
	printf("Firewall rule table\n");
	printf("idx   flows   target   client             server             port\n");

	for (i = 0;  i < MAX_RULES; i++) {
		fwr = &g_FWRules[i];

		/* we've searched till the end */
		if (fwr->fr_action == FRA_INVALID)
			break;

		/* print out each rule */
		if (!inet_ntop(AF_INET, &(fwr->fr_srcIP), cip_str, INET_ADDRSTRLEN) ||
			!inet_ntop(AF_INET, &(fwr->fr_dstIP), sip_str, INET_ADDRSTRLEN))
			EXIT_WITH_ERROR("inet_ntop() error\n");

		if (fwr->fr_srcIPmask != 32)
			sprintf(cip_str, "%s/%d", cip_str, fwr->fr_srcIPmask);
		if (fwr->fr_dstIPmask != 32)
			sprintf(sip_str, "%s/%d", sip_str, fwr->fr_dstIPmask);
		printf("%-6u%-8u%-9s%-19s%-19s",
			   (i + 1), fwr->fr_count,
			   (fwr->fr_action == FRA_DROP)? FR_DROP : FR_ACCEPT,
			   cip_str, sip_str);
		if (fwr->fr_srcPort)
			printf("sport:%-6d", ntohs(fwr->fr_srcPort));
		if (fwr->fr_dstPort)
			printf("dport:%-6d", ntohs(fwr->fr_dstPort));
		printf("\n");
	}
	printf("-----------------------------------------------------------------------\n");
	
	/* Set a timer for next printing */
	if (mtcp_settimer(mctx, sock, &tv_1sec, DumpFWRuleTable))
		EXIT_WITH_ERROR("mtcp_settimer() error\n");
}
/*----------------------------------------------------------------------------*/
static inline char*
ExtractPort(char* buf, in_port_t* sport, in_port_t* dport)
{
	in_port_t* p = NULL;
	char* temp = (char*)buf;
	char* check;	
	int port;
	char s = 0;             /* swap character */

	SKIP_CHAR(temp);	    /* skip characters */
	s = *temp; *temp = 0;	/* replace the end character with null */

	/* check if the port format is correct */
	if (!strncmp(buf, FR_SPORT, sizeof(FR_SPORT) - 1)) {
		p = sport;
		buf += (sizeof(FR_SPORT) - 1);
	}
	else if (!strncmp(buf, FR_DPORT, sizeof(FR_DPORT) - 1)) {
		p = dport;
		buf += (sizeof(FR_DPORT) - 1);
	}
	else
		EXIT_WITH_ERROR("Invalid rule in port setup [%s]\n", buf);

	check = buf;
	SKIP_DIGIT(check);
	if (check != temp)
		EXIT_WITH_ERROR("Invalid port format [%s]\n", buf);

	/* convert to port number */
	port = atoi(buf);
	if (port < 0 || port > 65536)
		EXIT_WITH_ERROR("Invalid port [%d]\n", port);
	(*p) = htons(port);

	(*temp) = s;	/* recover the original character */
	buf = temp;	    /* move buf pointer to next string */
	SKIP_SPACES(buf);

	return buf;
}
/*----------------------------------------------------------------------------*/
static inline char*
ExtractIPAddress(char* buf, in_addr_t* addr, int* addrmask)
{
	struct in_addr addr_conv;
	char* temp = (char*)buf;
	char* check;	
	int netmask = 32;
	char s = 0;        /* swap character */

	/* skip characters which are not '/' */
	while ((*temp) && !isspace(*temp) && (*temp) != '/') temp++;

	s = *temp; *temp = 0;
	if (inet_aton(buf, &addr_conv) == 0)
		EXIT_WITH_ERROR("Invalid IP address [%s]\n", buf);
	(*addr) = addr_conv.s_addr;
	(*temp) = s;

	/* if the rule contains netmask */
	if ((*temp) == '/') {
		buf = temp + 1;
		SKIP_CHAR(temp);
		s = *temp; *temp = 0;

		/* check if the format is correct */
		check = buf;
		SKIP_DIGIT(check);
		if (check != temp)
			EXIT_WITH_ERROR("Invalid netmask format [%s]\n", buf);

		/* convert to netmask number */
		netmask = atoi(buf);
		if (netmask < 0 || netmask > 32)
			EXIT_WITH_ERROR("Invalid netmask [%s]\n", buf);
		(*addr) = IP_NETMASK((*addr), netmask);
		(*temp) = s;
	}

	/* move buf pointer to next string */
	buf = temp;
	SKIP_SPACES(buf);

	(*addrmask) = netmask;

	return buf;
}
/*----------------------------------------------------------------------------*/
static void
ParseConfigFile(char* configF)
{
  	FirewallRule *fwr;
	FILE *fp;
	char line_buf[CONF_MAX_LINE_LEN] = {0};
	char *line, *p;
	int i = 0;

	/* config file path should not be null */
	assert(configF != NULL);

	/* open firewall rule file */
	if ((fp = fopen(configF, "r")) == NULL)
		EXIT_WITH_ERROR("Firewall rule file %s is not found.\n", configF);

	/* read each line */
	while ((line = fgets(line_buf, CONF_MAX_LINE_LEN, fp)) != NULL) {

		/* each line represents a rule */
		fwr = &g_FWRules[i];
		if (line[CONF_MAX_LINE_LEN - 1])
			EXIT_WITH_ERROR("%s has a line longer than %d\n",
						configF, CONF_MAX_LINE_LEN);

		SKIP_SPACES(line); /* remove spaces */
		if (*line == '\0' || *line == '#')
			continue;
		if ((p = strchr(line, '#'))) /* skip comments in the line */
			*p = '\0';		
		while (isspace(line[strlen(line) - 1])) /* remove spaces */
			line[strlen(line) - 1] = '\0';
		
		/* read firewall rule action */
		p = line;
		if (!strncmp(p, FR_ACCEPT, sizeof(FR_ACCEPT) - 1)) {
			fwr->fr_action = FRA_ACCEPT;
			p += (sizeof(FR_ACCEPT) - 1);
		}
		else if (!strncmp(p, FR_DROP, sizeof(FR_DROP) - 1)) {
			fwr->fr_action = FRA_DROP;
			p += (sizeof(FR_DROP) - 1);
		}
		else
			EXIT_WITH_ERROR("Unknown rule action [%s].\n", line);

		if (!isspace(*p)) /* invalid if no space exists after action */
			EXIT_WITH_ERROR("Invalid format [%s].\n", line);
		SKIP_SPACES(p);

		/* read client ip address */
		if (*p)
			p = ExtractIPAddress(p, &fwr->fr_srcIP, &(fwr->fr_srcIPmask));
		else
			EXIT_WITH_ERROR("Invalid format [%s].\n", line);

		/* read server ip address */
		if (*p)
			p = ExtractIPAddress(p, &fwr->fr_dstIP, &(fwr->fr_dstIPmask));
		else
			EXIT_WITH_ERROR("Invalid format [%s].\n", line);

		/* read port filter information */
		while (*p)
			p = ExtractPort(p, &(fwr->fr_srcPort), &(fwr->fr_dstPort));

		fwr->fr_count = 0;
		if ((i++) >= MAX_RULES)
			EXIT_WITH_ERROR("Exceeded max number of rules (%d)\n", MAX_RULES);
	}

	fclose(fp);
}
/*----------------------------------------------------------------------------*/
static inline int
MatchAddr(in_addr_t ip, in_addr_t fw_ip, int netmask)
{	
	ip = IP_NETMASK(ip, netmask);

	/* 0 means '*' */
	return (fw_ip == 0 || ip == fw_ip);
}
/*----------------------------------------------------------------------------*/
static inline int
MatchPort(in_port_t port, in_port_t fw_port)
{
	/* 0 means '*' */
	return (fw_port == 0 || port == fw_port);
}
/*----------------------------------------------------------------------------*/
static int
FWRLookup(in_addr_t sip, in_addr_t dip, in_port_t sp, in_port_t dp)
{
	int i;
	FirewallRule *p = g_FWRules;

	for (i = 0;  i < MAX_RULES; i++) {
		if (p[i].fr_action == FRA_INVALID) {
			/* We've searched till the end. By default, allow any flow */
			return (FRA_ACCEPT);
		}

		if (MatchAddr(sip, p[i].fr_srcIP, p[i].fr_srcIPmask) &&
			MatchAddr(dip, p[i].fr_dstIP, p[i].fr_dstIPmask) &&
			MatchPort(sp, p[i].fr_srcPort) &&
			MatchPort(dp, p[i].fr_dstPort)) {
			p[i].fr_count++;
			return p[i].fr_action;
		}
	}

	assert(0); /* can't reach here */
	return  (FRA_ACCEPT);
}
/*----------------------------------------------------------------------------*/
static void
ApplyActionPerFlow(mctx_t mctx, int msock, int side,
		   		     uint64_t events, filter_arg_t *arg)

{
	/* this function is called at the first SYN */
	struct pkt_info p;
	int opt;
	FRAction action;

	if (mtcp_getlastpkt(mctx, msock, side, &p) < 0)
		EXIT_WITH_ERROR("Failed to get packet context!\n");

	/* look up the firewall rules */
	action = FWRLookup(p.iph->saddr, p.iph->daddr, 
					    p.tcph->source, p.tcph->dest);

	if (action == FRA_DROP) {
		mtcp_setlastpkt(mctx, msock, side, 0, NULL, 0, MOS_DROP);
	} else {
		assert(action == FRA_ACCEPT);
		/* no need to monitor this flow any more */
		opt = MOS_SIDE_BOTH;
		if (mtcp_setsockopt(mctx, msock, SOL_MONSOCKET, 
				    MOS_STOP_MON, &opt, sizeof(opt)) < 0)
			EXIT_WITH_ERROR("Failed to stop monitoring conn with sockid: %d\n",
					msock);
	}
}
/*----------------------------------------------------------------------------*/
static bool
CatchInitSYN(mctx_t mctx, int sockid, 
			int side, uint64_t events, filter_arg_t *arg)
{
	struct pkt_info p;

	if (mtcp_getlastpkt(mctx, sockid, side, &p) < 0)
		EXIT_WITH_ERROR("Failed to get packet context!!!\n");

	return (p.tcph->syn && !p.tcph->ack);
}
/*----------------------------------------------------------------------------*/
static void
CreateAndInitThreadContext(struct thread_context* ctx, 
						      int core, event_t  udeForSYN)
{	
	struct timeval tv_1sec = { /* 1 second */
		.tv_sec = 1,
		.tv_usec = 0
	};

	ctx->mctx = mtcp_create_context(core);

	/* create socket  */
	ctx->mon_listener = mtcp_socket(ctx->mctx, AF_INET,
					MOS_SOCK_MONITOR_STREAM, 0);
	if (ctx->mon_listener < 0)
		EXIT_WITH_ERROR("Failed to create monitor listening socket!\n");
	
	/* register callback */
	if (mtcp_register_callback(ctx->mctx, ctx->mon_listener,
							   udeForSYN,
							   MOS_HK_SND,
				   			   ApplyActionPerFlow) == -1)
		EXIT_WITH_ERROR("Failed to register callback func!\n");

	/* CPU 0 is in charge of printing stats */
	if (ctx->mctx->cpu == 0 &&
		mtcp_settimer(ctx->mctx, ctx->mon_listener,
					  &tv_1sec, DumpFWRuleTable))
		EXIT_WITH_ERROR("Failed to register timer callback func!\n");
					  
}
/*----------------------------------------------------------------------------*/
static void
WaitAndCleanupThreadContext(struct thread_context* ctx)
{
	/* wait for the TCP thread to finish */
	mtcp_app_join(ctx->mctx);
		
	/* close the monitoring socket */
	mtcp_close(ctx->mctx, ctx->mon_listener);

	/* tear down */
	mtcp_destroy_context(ctx->mctx);	
}
/*----------------------------------------------------------------------------*/
int
main(int argc, char **argv)
{
	int ret, i;
	char *fname = MOS_CONFIG_FILE; /* path to the default mos config file */
	struct mtcp_conf mcfg;
	char simple_firewall_file[1024] = "config/simple_firewall.conf";
	struct thread_context ctx[MAX_CPUS] = {{0}}; /* init all fields to 0 */
	event_t initSYNEvent;
	int num_cpus;
	int opt, rc;

	/* get the total # of cpu cores */
	num_cpus = GetNumCPUs();

	while ((opt = getopt(argc, argv, "c:f:n:")) != -1) {
		switch (opt) {
			case 'c':
				fname = optarg;
				break;
		        case 'f':
				strcpy(simple_firewall_file, optarg);
				break;
			case 'n':
				if ((rc=atoi(optarg)) > num_cpus) {
					EXIT_WITH_ERROR("Available number of CPU cores is %d "
							"while requested cores is %d\n",
							num_cpus, rc);
				}
				num_cpus = rc;
				break;
			default:
				printf("Usage: %s [-c mos_config_file] "
				       "[-f simple_firewall_config_file]\n", 
				       argv[0]);
				return 0;
		}
	}

	/* parse mos configuration file */
	ret = mtcp_init(fname);
	if (ret)
		EXIT_WITH_ERROR("Failed to initialize mtcp.\n");

	/* set the core limit */
	mtcp_getconf(&mcfg);
	mcfg.num_cores = num_cpus;
	mtcp_setconf(&mcfg);

	/* parse simple firewall-specfic startup file */
	ParseConfigFile(simple_firewall_file);

	/* populate local mos-specific mcfg struct for later usage */
	mtcp_getconf(&mcfg);

	/* event for the initial SYN packet */
	initSYNEvent = mtcp_define_event(MOS_ON_PKT_IN, CatchInitSYN, NULL);
	if (initSYNEvent == MOS_NULL_EVENT)
		EXIT_WITH_ERROR("mtcp_define_event() failed!");

	/* initialize monitor threads */	
	for (i = 0; i < mcfg.num_cores; i++)
		CreateAndInitThreadContext(&ctx[i], i, initSYNEvent);

	/* wait until all threads finish */	
	for (i = 0; i < mcfg.num_cores; i++) {
		WaitAndCleanupThreadContext(&ctx[i]);
	  	TRACE_INFO("Message test thread %d joined.\n", i);	  
	}	
	
	mtcp_destroy();

	return EXIT_SUCCESS;
}
/*----------------------------------------------------------------------------*/
