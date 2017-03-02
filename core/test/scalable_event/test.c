#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <time.h>

#include "mtcp.h"
#include "mos_api.h"
#include "memory_mgt.h"
#include "scalable_event.h"

#define PROFILE_ON
#include "profile.h"

event_t g_first_ev = MOS_NULL_EVENT, g_last_ev = MOS_NULL_EVENT;
int g_depth = 0;
float g_prob = 0;
int g_threshold = 0;

int ft_counter = 0, cb_counter = 0;
struct mtcp_manager mtcp;

extern inline int
ModCb(kvs_t *store, stree_t **pstree, event_t ev, callback_t cb);

extern void
HandleCb(mctx_t mctx, int sock, int side, stree_t *stree, event_t events);

extern int
RaiseEv(kvs_t *store, event_t event);

mtcp_manager_t
GetMTCPManager(mctx_t mctx)
{
	return &mtcp;
}

u_int
sfbpf_filter(const struct sfbpf_insn *pc, const u_char *p, u_int wirelen,
		u_int buflen)
{
	return 0;
}

static void
cb(mctx_t mctx, int sock, int side, event_t ev, filter_arg_t *arg)
{
	//printf("cb: %ld\n", ev);
	cb_counter++;
}

static bool
ft(mctx_t mctx, int sock, int side, event_t ev, filter_arg_t *arg)
{
	//printf("ft: %ld\n", ev);
	return true;
}

static bool
ft_prob(mctx_t mctx, int sockid, int side, uint64_t events, filter_arg_t *arg)
{
	static __thread unsigned int seed = 0;
	if (seed == 0)
		seed = time(NULL) + getpid();
	bool ret = rand_r(&seed) < g_threshold;
	//printf("ft: %ld returns %s\n", events, ret ? "true" : "false");
	ft_counter++;
	return ret;
}

static int
power(base, exponent)
{
	int i, ret = 1;

	assert(exponent >= 0);

	for (i = 0; i < exponent; i++)
		ret *= base;

	return ret;
}

event_t g_ac_events[5000];
event_t g_r_events[5000];

static bool
ft_ac_match(mctx_t mctx, int sock, int side, uint64_t ev, filter_arg_t *arg)
{
	event_t ev_matched;
	static __thread unsigned int seed = 0;
	if (seed == 0)
		seed = time(NULL) + getpid();
	ev_matched = g_ac_events[rand_r(&seed) % *((int *)arg->arg)];

	RaiseEv(mtcp.ev_store, ev_matched);

	return true;
}

int main(int argc, char **argv)
{
	stree_t *stree[10] = {NULL};

	GlobInitEvent();
	InitEvent(&mtcp);

	ModCb(mtcp.ev_store, &stree[0], 1, cb);
	ModCb(mtcp.ev_store, &stree[1], 1, cb);
	ModCb(mtcp.ev_store, &stree[0], 2, cb);

	printf("HandleCb(0)\n");
	HandleCb(NULL, 0, 0, stree[0], 3);
	printf("HandleCb(1)\n");
	HandleCb(NULL, 0, 0, stree[1], 3);

	event_t ude1 = mtcp_define_event(1, ft, NULL);
	assert(ude1);
	ModCb(mtcp.ev_store, &stree[2], ude1, cb);

	printf("HandleCb(2)\n");
	HandleCb(NULL, 0, 0, stree[2], 3);

	event_t ude2 = mtcp_define_event(ude1, ft, NULL);
	assert(ude2);
	ModCb(mtcp.ev_store, &stree[1], ude2, cb);

	printf("HandleCb(1)\n");
	HandleCb(NULL, 0, 0, stree[1], 1);

	ModCb(mtcp.ev_store, &stree[1], ude2, NULL);

	printf("HandleCb(1)\n");
	HandleCb(NULL, 0, 0, stree[1], 1);

	/* Random tree test */
#define BEV4 4
	event_t ev;
	int opt, i;
	float numcb = 1, numft = 0, p;

	while ((opt = getopt(argc, argv, "d:p:")) > 0) {
		if (opt == 'd')
			g_depth = atoi(optarg);
		else if (opt == 'p')
			sscanf(optarg, "%f", &g_prob);
		else {
			fprintf(stderr, "Unknown option '%c'\n", opt);
			exit(0);
		}
	}

	g_threshold = (int)((float)RAND_MAX * g_prob);

	if (g_depth < 0)
		return -1;

	PROFILE_INIT();
	PROFILE_VAR(define_event);
	PROFILE_VAR(register_event);
	PROFILE_VAR(trigger_event);

	PROFILE_START();

	PROFILE_FROM(define_event);

	g_first_ev = mtcp_define_event(BEV4, ft_prob, NULL);
	//printf("%d -> %ld\n", BEV4, g_first_ev);
	ev = mtcp_define_event(BEV4, ft_prob, NULL);
	//printf("%d -> %ld\n", BEV4, ev);
	g_last_ev = g_first_ev + power(2, g_depth + 1) - 3;

	for (ev = g_first_ev; g_depth != 1 && ev != MOS_NULL_EVENT; ev++) {
		event_t nev;
		/* Call define_event() twice since it is full binary tree */
		nev = mtcp_define_event(ev, ft_prob, NULL);
		//printf("%ld -> %ld\n", ev, nev);
		nev = mtcp_define_event(ev, ft_prob, NULL);
		//printf("%ld -> %ld\n", ev, nev);
		if (nev == g_last_ev)
			break;
		else if (nev == MOS_NULL_EVENT) {
			fprintf(stderr, "Failed to define child event of event %ld\n", ev);
			exit(0);
		}
	}
	if (ev == MOS_NULL_EVENT) {
		fprintf(stderr, "Failed to define child event of event %ld\n", ev);
		exit(0);
	}

	PROFILE_TO(define_event);

	for (p = g_prob, i = 0; i < g_depth; p *= g_prob, i++) {
		numft = 2*numcb;
		numcb += p * power(2, i+1);
	}

	printf("\x1b[33m"
		   "Tree depth: %d, Defined total events: %d\n"
		   "Theoritical average function call: %.2f callbacks, %.2f filters"
		   "\x1b[0m\n",
		   g_depth, power(2, g_depth) + 1, numcb, numft);

	/* Test random tree */

	PROFILE_FROM(register_event);

	ModCb(mtcp.ev_store, &stree[3], BEV4, cb);
	for (ev = g_first_ev; ev != g_last_ev; ev++)
		ModCb(mtcp.ev_store, &stree[3], ev, cb);

	PROFILE_TO(register_event);

#if 0
	mtcp_register_callback(mctx, msock, BEV4, MOS_PRE_RCV, cb_void);

	for (ev = g_first_ev; ev != g_last_ev; ev++)
		mtcp_register_callback(mctx, msock, ev, MOS_PRE_RCV, cb_void);
#endif

	PROFILE_FROM(trigger_event);
#define LCOUNT 1000000
	int j;
	for (j=0; j<LCOUNT;j++)
		HandleCb(NULL, 0, 0, stree[3], BEV4);

	PROFILE_TO(trigger_event);

	PROFILE_END();

	PROFILE_PRINT(stdout);
	printf("Avg CB %f Avg FT %f\n", cb_counter*1.000000/LCOUNT, ft_counter*1.0/LCOUNT);

#ifdef DO_SNORT_SIM
	/*--------------------------------------------------------------------*/
	/* selective event invocation test                                    */
	/*--------------------------------------------------------------------*/

	/* 1. Snort simulation */
	{
		printf("SNORT SIMULATION TEST\n");
		stree_t *s = NULL;
		const event_t BEV8 = 8;
		const int REPEAT = 10;
		const int RULES = 4000;
		const int RULE_CONDS = 4;
		filter_arg_t arg = {&RULES, sizeof(RULES)};
		int percent = 0;

		int i, j;

		const event_t EV_AC_MATCH = mtcp_define_event(BEV8, ft_ac_match, &arg);

		for (i = 0; i < RULES; i++) {
			event_t e = g_ac_events[i] = mtcp_alloc_event(EV_AC_MATCH);

			for (j = 0; j < RULE_CONDS; j++)
				e = mtcp_define_event(e, ft, NULL);

			g_r_events[i] = e;

			if ((i * 100) / RULES > percent) {
				percent = (i * 100) / RULES;
				printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
				printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
				printf(" %d%% (%d / %d)", percent, i, RULES);
			}
		}
		printf("\n");

		percent = 0;
		for (i = 0; i < RULES; i++) {
			ModCb(mtcp.ev_store, &s, g_r_events[i], cb);

			if ((i * 100) / RULES > percent) {
				percent = (i * 100) / RULES;
				printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
				printf("\b\b\b\b\b\b\b\b\b\b\b\b\b\b\b");
				printf(" %d%% (%d / %d)", percent, i, RULES);
			}
		}
		printf("\n");

		cb_counter = 0;
		for (i = 0; i < REPEAT; i++)
			HandleCb(NULL, 0, 0, s, BEV8);
		printf("%d / %d callbacks are called; %s\n",
				cb_counter, REPEAT, cb_counter == REPEAT ? "SUCCESS" : "FAILURE");
	}
#endif

	return 0;
}
