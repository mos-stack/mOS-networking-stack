#include <stdio.h>
#include <stdlib.h>

#include "mtcp.h"
#include "event_callback.h"
#include "mos_api.h"
#include "memory_mgt.h"

struct mtcp_thread_context ctx;
struct mtcp_manager mtcp;

extern void
InitEvent(mtcp_manager_t mtcp, int num_evt);
extern void
InitEvP(struct ev_pointer *evp, struct ev_base *evb);
extern void
CleanupEvP(struct ev_pointer *evp);
extern void
InitEvB(mtcp_manager_t mtcp, struct ev_base *evb);
extern void
CleanupEvB(mtcp_manager_t mtcp, struct ev_base *evb);
extern inline int
ModCb(mtcp_manager_t mtcp, int op, struct ev_pointer *evp, struct ev_base *evb,
		event_t events, void *cb);

enum {
	OP_REG,
	OP_UNREG,
};

#define EV(i) (1L << i)
#define PRINTF(args...) \
do {printf(args); fflush(stdout);} while (0)

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

int
intevent(event_t ev)
{
	int e;
	for (e = 0; e < 64; e++)
		if ((1L << e) == ev)
			return e;
	return -1;
}

static void
cb(mctx_t mctx, int sock, int side, event_t ev)
{
	PRINTF("CB: %d\n", intevent(ev));
}

static void
cb2(mctx_t mctx, int sock, int side, event_t ev)
{
	PRINTF("CB2: %d\n", intevent(ev));
}

static int
ft(mctx_t mctx, int sock, int side, event_t ev)
{
	PRINTF("FT: %d\n", intevent(ev));

	return 1;
}

#define NEW_RAW_SOCK(s) \
struct socket_map (s); struct mon_listener __ml_##s; \
(s).socktype = MOS_SOCK_MONITOR_RAW; \
(s).monitor_listener = &__ml_##s; \
InitEvB(&mtcp, &__ml_##s.post_tcp_evb)

#define NEW_PSV_SOCK(s) \
struct socket_map (s); struct mon_listener __ml_##s; \
(s).socktype = MOS_SOCK_MONITOR_STREAM; \
(s).monitor_listener = &__ml_##s; \
InitEvB(&mtcp, &__ml_##s.post_tcp_evb)

#define NEW_ATV_SOCK(s, p) \
struct socket_map (s); struct mon_stream __ms_##s; \
__ms_##s.monitor_listener = (p).monitor_listener; \
(s).socktype = MOS_SOCK_MONITOR_STREAM_ACTIVE; \
(s).monitor_stream = &__ms_##s; \
InitEvP(&__ms_##s.post_tcp_evp, &(p).monitor_listener->post_tcp_evb)

#define MOD_CB(op, s, ev, cb) \
((s).socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE) \
 ? ModCb(&mtcp, op, &(s).monitor_stream->post_tcp_evp, \
		 &(s).monitor_stream->monitor_listener->post_tcp_evb, (ev), (cb)) \
 : ModCb(&mtcp, op, &(s).monitor_listener->post_tcp_evb.dflt_evp, \
		 &(s).monitor_listener->post_tcp_evb, (ev), (cb))

#define HANDLE_CB(s, ev) \
	HandleCallback(&mtcp, MOS_POST_SND, &(s), 0, NULL, (ev));

#define GET_EVT(s) \
(((s).socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE) \
 ? (s).monitor_stream->post_tcp_evp.evt \
 : (s).monitor_listener->post_tcp_evb.dflt_evp.evt)

int main(int argc, char **argv)
{
	ctx.cpu = 0;
	mtcp.ctx = &ctx;

	GlobInitEvent();
	InitEvent(&mtcp, 100);

	PRINTF("========== RAW socket test ==========\n");

	/* new raw socket */
	NEW_RAW_SOCK(raw0);
	event_t ude0 = mtcp_define_ude(EV(0), ft);

	PRINTF("test 0: expected result\n");
	ft(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, ude0);

	PRINTF("test 0: result\n");
	MOD_CB(OP_REG, raw0, ude0, cb);
	HANDLE_CB(raw0, EV(0));

	PRINTF("\n");

	/* new raw socket */
	NEW_RAW_SOCK(raw);

	/* test 1 */
	PRINTF("test 1: expected result\n");
	cb(NULL, 0, 0, EV(0));

	PRINTF("test 1: result\n");
	MOD_CB(OP_REG, raw, EV(0), cb);
	HANDLE_CB(raw, EV(0));

	PRINTF("\n");

	/* test 2 */
	PRINTF("test 2: expected result\n");
	cb(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 2: result\n");
	MOD_CB(OP_REG, raw, EV(1), cb);
	HANDLE_CB(raw, EV(0) | EV(1));

	PRINTF("\n");

	/* test 3 */
	PRINTF("test 3: expected result\n");
	cb(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 3: result\n");
	event_t ude1 = mtcp_define_ude(EV(0), ft);
	HANDLE_CB(raw, EV(0) | EV(1));

	PRINTF("\n");

	/* test 4 */
	PRINTF("test 4: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 4: result\n");
	MOD_CB(OP_REG, raw, ude1, cb);
	HANDLE_CB(raw, EV(0) | EV(1));

	PRINTF("\n");

	/* test 5 */
	PRINTF("test 5: expected result\n");
	ft(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 5: result\n");
	MOD_CB(OP_UNREG, raw, EV(0), NULL);
	HANDLE_CB(raw, EV(0) | EV(1));

	PRINTF("\n");

	/* test 6 */
	PRINTF("test 6: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 6: result\n");
	MOD_CB(OP_REG, raw, EV(0), cb);
	HANDLE_CB(raw, EV(0) | EV(1));

	PRINTF("\n");

	/* test 7 */
	PRINTF("test 7: expected result\n");
	cb(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 7: result\n");
	MOD_CB(OP_UNREG, raw, ude1, NULL);
	HANDLE_CB(raw, EV(0) | EV(1));

	PRINTF("\n");

	event_t ude2 = mtcp_define_ude(ude1, ft);

	/* test 8 */
	PRINTF("test 8: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, ude2);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 8: result\n");
	MOD_CB(OP_REG, raw, ude2, cb);
	HANDLE_CB(raw, EV(0) | EV(1));

	PRINTF("\n");

	/* test 9 */
	PRINTF("test 9: expected result\n");
	cb(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 9: result\n");
	MOD_CB(OP_UNREG, raw, ude2, NULL);
	HANDLE_CB(raw, EV(0) | EV(1));

	PRINTF("\n");

	PRINTF("========== STREAM socket test ==========\n");

	/* new listen socket */
	NEW_PSV_SOCK(lsnr);

	/* test 1 */
	PRINTF("test 1: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 1: result\n");
	MOD_CB(OP_REG, lsnr, EV(0), cb);
	MOD_CB(OP_REG, lsnr, EV(1), cb);
	MOD_CB(OP_REG, lsnr, ude1, cb);
	NEW_ATV_SOCK(strm1, lsnr); /**< new stream socket */
	HANDLE_CB(strm1, EV(0) | EV(1));

	PRINTF("\n");

	/* test 2 */
	PRINTF("test 2: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, ude1);
	ft(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, ude2);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 2: result\n");
	MOD_CB(OP_REG, strm1, ude2, cb);
	HANDLE_CB(strm1, EV(0) | EV(1));

	PRINTF("\n");

	/* test 3 */
	PRINTF("test 3: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 3: result\n");
	NEW_ATV_SOCK(strm2, lsnr); /**< new stream socket */
	HANDLE_CB(strm2, EV(0) | EV(1));

	PRINTF("\n");

	/* test 4 */
	PRINTF("test 4: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, ude2);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 4: result\n");
	MOD_CB(OP_UNREG, strm1, ude1, NULL);
	HANDLE_CB(strm1, EV(0) | EV(1));

	PRINTF("\n");

	/* test 5 */
	PRINTF("test 5: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	cb2(NULL, 0, 0, ude1);
	ft(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, ude2);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 5: result\n");
	MOD_CB(OP_REG, strm1, ude1, cb2);
	HANDLE_CB(strm1, EV(0) | EV(1));

	PRINTF("\n");

	/* test 6 */
	PRINTF("test 6: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 6: result\n");
	HANDLE_CB(strm2, EV(0) | EV(1));

	PRINTF("\n");

	/* test 7 */
	PRINTF("test 7: %p != %p == %p\n",
			GET_EVT(strm1), GET_EVT(strm2), GET_EVT(lsnr));

	PRINTF("\n");

	/* test 8 */
	PRINTF("test 8: expected result\n");
	cb(NULL, 0, 0, EV(0));
	ft(NULL, 0, 0, EV(0));
	cb(NULL, 0, 0, ude1);
	ft(NULL, 0, 0, ude1);
	cb(NULL, 0, 0, ude2);
	cb(NULL, 0, 0, EV(1));

	PRINTF("test 8: result\n");
	MOD_CB(OP_UNREG, strm1, ude1, NULL);
	MOD_CB(OP_REG, strm1, ude1, cb);
	HANDLE_CB(strm1, EV(0) | EV(1));

	PRINTF("\n");
	return 0;
}
