#define __MOS_CORE_

#ifndef NEWEV

/* NOTE TODO:
 * We can improve performance and reduce memory usage by making MOS_NULL
 * hook and MOS_HK_RCV share event structures since no event can be registered
 * to both hooks. */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <string.h>

#include "mtcp.h"
#include "event_callback.h"
#include "debug.h"
#include "mos_api.h"
#include "mtcp_api.h"
/*----------------------------------------------------------------------------*/
enum {
	OP_REG,
	OP_UNREG,
};

/* macros */
#define IS_PO2(x) (!((x - 1) & (x)))
#define NUM_UDE (MAX_EV - UDE_OFFSET)
#define EVENT_FOREACH_START(ev, idx, from, to, map) \
do {int idx; for (idx = (from); idx < (to); idx++) { \
	const event_t ev = 1L << idx; if (!((map) & (ev))) continue; 
#define EVENT_FOREACH_END(ev, map) \
	if (!((map) &= ~ev)) break;}} while (0)

/* Global variables (per CPU core) */
static struct {
	filter_t ft;
	event_t ancestors;
	int8_t parent;
} g_udes[NUM_UDE];
static event_t g_descendants[MAX_EV];
static event_t g_ude_map;
static const event_t g_bev_map = (~(((event_t)-1) << NUM_BEV)) << BEV_OFFSET;
static int8_t g_ude_id[MAX_EV][NUM_UDE];

/* FIXME: ft_map is not managed properly (especially in UnregCb()). */

/*----------------------------------------------------------------------------*/
void
GlobInitEvent(void)
{
	int i;
	for (i = 0; i < NUM_UDE; i++) {
		g_udes[i].ft = NULL;
		g_udes[i].ancestors = 0;
		g_udes[i].parent = -1;
	}

	for (i = 0; i < MAX_EV; i++)
		g_descendants[i] = 0;

	memset(g_ude_id, -1, MAX_EV * NUM_UDE);
	g_ude_map = 0;
}
/*----------------------------------------------------------------------------*/
void
InitEvent(mtcp_manager_t mtcp, int num_evt)
{
	if (!(mtcp->evt_pool = MPCreate(sizeof(struct ev_table),
					sizeof(struct ev_table) * num_evt, 0))) {
		TRACE_ERROR("Failed to allocate ev_table pool\n");
		exit(0);
	}
}
/*----------------------------------------------------------------------------*/
void
InitEvP(struct ev_pointer *evp, struct ev_base *evb)
{
	*evp = evb->dflt_evp;
	event_t map = evp->cb_map;

	EVENT_FOREACH_START(ev, i, BEV_OFFSET, BEV_OFFSET + NUM_BEV, map) {
		evp->evt->ent[i].ref++;
	} EVENT_FOREACH_END(ev, map);

	if (!map)
		return;

	EVENT_FOREACH_START(ev, i, UDE_OFFSET, MAX_EV, map) {
		evp->evt->ent[i].ref++;
	} EVENT_FOREACH_END(ev, map);
}
/*----------------------------------------------------------------------------*/
void
CleanupEvP(struct ev_pointer *evp)
{
	EVENT_FOREACH_START(ev, i, BEV_OFFSET, BEV_OFFSET + NUM_BEV, evp->cb_map) {
		evp->evt->ent[i].ref--;
	} EVENT_FOREACH_END(ev, evp->cb_map);

	if (!evp->cb_map)
		return;

	EVENT_FOREACH_START(ev, i, UDE_OFFSET, MAX_EV, evp->cb_map) {
		evp->evt->ent[i].ref--;
	} EVENT_FOREACH_END(ev, evp->cb_map);
}
/*----------------------------------------------------------------------------*/
void
InitEvB(mtcp_manager_t mtcp, struct ev_base *evb)
{
	TAILQ_INIT(&evb->evth);
	struct ev_table *dflt_evt = MPAllocateChunk(mtcp->evt_pool);
	memset(dflt_evt, 0, sizeof(struct ev_table));

	TAILQ_INSERT_HEAD(&evb->evth, dflt_evt, link);
	evb->dflt_evp.cb_map = 0;
	evb->dflt_evp.ft_map = 0;
	evb->dflt_evp.evt = dflt_evt;
}
/*----------------------------------------------------------------------------*/
void
CleanupEvB(mtcp_manager_t mtcp, struct ev_base *evb)
{
	struct ev_table *walk, *tmp;
	for (walk = TAILQ_FIRST(&evb->evth); walk != NULL; walk = tmp) {
		tmp = TAILQ_NEXT(walk, link);

		MPFreeChunk(mtcp->evt_pool, walk);
	}
}
/*----------------------------------------------------------------------------*/
static inline void
RegCbWCpy(struct ev_pointer *evp, struct ev_table *new_evt,
		event_t events, void *cb)
{
	/* NOTE: We may apply binary search which is O(log(N)) later, while current
	 * linear search is O(N). */
	event_t evcpy = 0, ev_total;
	event_t ev_inc_ref = 0, ev_dec_ref = 0;
	struct ev_table *cur_evt = evp->evt;

	event_t overlap = events & new_evt->map;

	assert(evp->evt != new_evt);
	assert(!(evp->cb_map & events));
	assert((evp->cb_map & cur_evt->map) == evp->cb_map);

	/* event table will be changed to new_evt */
	ev_total = events | evp->cb_map;
	evcpy = evp->cb_map & ~new_evt->map;
	evp->evt = new_evt;

	ev_inc_ref = events | evp->cb_map;
	ev_dec_ref = evp->cb_map;

	new_evt->map |= ev_total;
	evp->cb_map |= events;

	/* For built-in events */
	EVENT_FOREACH_START(ev, i, BEV_OFFSET, BEV_OFFSET + NUM_BEV, ev_total) {
		if (events & ev) {
			assert((ev & overlap) ? new_evt->ent[i].cb == cb
								  : new_evt->ent[i].ref == 0);
			if (!(ev & overlap))
				new_evt->ent[i].cb = cb;
		} else if (evcpy & ev) {
			assert(new_evt && new_evt != cur_evt);
			new_evt->ent[i].cb = cur_evt->ent[i].cb;
		}

		/* ev_dec_ref is subset of ev_inc_ref */
		if (ev_inc_ref & ev) {
			new_evt->ent[i].ref++;
			if (!(new_evt->map & ev))
				new_evt->map |= ev;
			if (ev_dec_ref & ev) {
				if (--cur_evt->ent[i].ref)
					cur_evt->map &= ~ev;
			}
		}
	} EVENT_FOREACH_END(ev, ev_total);

	if (!ev_total)
		return;

	/* For UDEs */
	EVENT_FOREACH_START(ev, i, UDE_OFFSET, MAX_EV, ev_total) {
		if (events & ev) {
			assert((ev & overlap) ? new_evt->ent[i].cb == cb
								  : new_evt->ent[i].ref == 0);
			if (!(ev & overlap))
				new_evt->ent[i].cb = cb;
			/* update ft_map */
			assert(g_udes[i - UDE_OFFSET].ft);
			assert(g_udes[i - UDE_OFFSET].ancestors);
			evp->ft_map |= g_udes[i - UDE_OFFSET].ancestors;
		} else if (evcpy & ev) {
			assert(new_evt && new_evt != cur_evt);
			new_evt->ent[i].cb = cur_evt->ent[i].cb;
		}

		/* ev_dec_ref is subset of ev_inc_ref */
		if (ev_inc_ref & ev) {
			new_evt->ent[i].ref++;
			if (!(new_evt->map & ev))
				new_evt->map |= ev;
			if (ev_dec_ref & ev) {
				if (--cur_evt->ent[i].ref)
					cur_evt->map &= ~ev;
			}
		}
	} EVENT_FOREACH_END(ev, ev_total);
}
/*----------------------------------------------------------------------------*/
static inline void
RegCbWoCpy(struct ev_pointer *evp, event_t events, void *cb)
{
	/* NOTE: We may apply binary search which is O(log(N)) later, while current
	 * linear search is O(N). */
	event_t ev_inc_ref = 0;
	struct ev_table *cur_evt = evp->evt;

	event_t overlap = events & cur_evt->map;

	assert(!(evp->cb_map & events));
	assert((evp->cb_map & cur_evt->map) == evp->cb_map);

	ev_inc_ref = events;

	cur_evt->map |= events;
	evp->cb_map |= events;

	/* For built-in events */
	EVENT_FOREACH_START(ev, i, BEV_OFFSET, BEV_OFFSET + NUM_BEV, events) {
		if (events & ev) {
			assert((ev & overlap) ? cur_evt->ent[i].cb == cb
								  : cur_evt->ent[i].ref == 0);
			if (!(ev & overlap))
				cur_evt->ent[i].cb = cb;
		}

		/* ev_dec_ref is subset of ev_inc_ref */
		if (ev_inc_ref & ev) {
			cur_evt->ent[i].ref++;
			if (!(cur_evt->map & ev))
				cur_evt->map |= ev;
		}
	} EVENT_FOREACH_END(ev, events);

	if (!events)
		return;

	/* For UDEs */
	EVENT_FOREACH_START(ev, i, UDE_OFFSET, MAX_EV, events) {
		if (events & ev) {
			assert((ev & overlap) ? cur_evt->ent[i].cb == cb
								  : cur_evt->ent[i].ref == 0);
			if (!(ev & overlap))
				cur_evt->ent[i].cb = cb;
			/* update ft_map */
			assert(g_udes[i - UDE_OFFSET].ft);
			assert(g_udes[i - UDE_OFFSET].ancestors);
			evp->ft_map |= g_udes[i - UDE_OFFSET].ancestors;
		}
		
		/* ev_dec_ref is subset of ev_inc_ref */
		if (ev_inc_ref & ev) {
			cur_evt->ent[i].ref++;
			if (!(cur_evt->map & ev))
				cur_evt->map |= ev;
		}
	} EVENT_FOREACH_END(ev, events);
}
/*----------------------------------------------------------------------------*/
static inline void
UnregCb(struct ev_pointer *evp, event_t events)
{
	assert(evp);

	struct ev_table *evt = evp->evt;
	evp->cb_map &= ~events;

	/* Unregister unnecessary UDEs */
	if (events & g_ude_map) {
		event_t evs = events & g_ude_map;
		EVENT_FOREACH_START(ev, i, UDE_OFFSET, MAX_EV, evs) {
			int walk = i;
			while (1) {
				const event_t evid = 1L << walk;
				if (/* no registered callback */
					!(evid & evp->cb_map)
					/* no child events */
					&& !(g_descendants[walk] & evp->cb_map)) {
					/* this UDE filter is useless */
					evp->ft_map &= ~(1L << g_udes[walk - UDE_OFFSET].parent);
					/* No need to see this event in rest of EVENT_FOREACH */
					evs &= ~evid;
					if ((walk = g_udes[walk - UDE_OFFSET].parent) < UDE_OFFSET)
						break;
				} else
					break;
			}
		} EVENT_FOREACH_END(ev, evs);
	}

	/* Placing reference counter for each event table entry, instead of each
	 * event table, and decrement them for every callback unregistration may
	 * look inefficient. However, actually, it does NOT. If reference counter
	 * is for each event table, then we need to call FindReusableEvT() for
	 * every callback unregistration to find reusable event table.
	 * FindReusableEvT() is heavier than per-event reference counter update.
	 * And that way also wastes memory. */

	EVENT_FOREACH_START(ev, i, BEV_OFFSET, BEV_OFFSET + NUM_BEV, events) {
		if (--evt->ent[i].ref == 0)
			evt->map &= ~ev;
	} EVENT_FOREACH_END(ev, events);

	if (!events)
		return;

	EVENT_FOREACH_START(ev, i, UDE_OFFSET, MAX_EV, events) {
		if (--evt->ent[i].ref == 0)
			evt->map &= ~ev;
	} EVENT_FOREACH_END(ev, events);
}
/*----------------------------------------------------------------------------*/
static inline struct ev_table *
FindReusableEvT(struct ev_pointer *evp, struct ev_base *evb,
		event_t events, void *cb)
{
	struct ev_table *cur_evt = evp->evt;
	struct ev_table *walk;

	assert((evp->cb_map & cur_evt->map) == evp->cb_map);

	TAILQ_FOREACH(walk, &evb->evth, link) {
		event_t overlap = evp->cb_map & walk->map;
		assert((events & overlap) == 0);
		event_t ev_total = events | overlap;

		EVENT_FOREACH_START(ev, i, BEV_OFFSET, BEV_OFFSET + NUM_BEV, ev_total) {
			if (ev & events) {
				if (walk->ent[i].cb != cb)
					goto __continue;
			} else /* if (ev & overlap) */ {
				if (walk->ent[i].cb != cur_evt->ent[i].cb)
					goto __continue;
			}
		} EVENT_FOREACH_END(ev, ev_total);

		if (!ev_total)
			return walk;

		EVENT_FOREACH_START(ev, i, UDE_OFFSET, MAX_EV, ev_total) {
			if (ev & events) {
				if (walk->ent[i].cb != cb)
					goto __continue;
			} else /* if (ev & overlap) */ {
				if (walk->ent[i].cb != cur_evt->ent[i].cb)
					goto __continue;
			}
		} EVENT_FOREACH_END(ev, ev_total);

		if (!ev_total)
			return walk;
__continue:
		continue;
	}

	return NULL;
}
/*----------------------------------------------------------------------------*/
inline int
ModCb(mtcp_manager_t mtcp, int op, struct ev_pointer *evp, struct ev_base *evb,
		event_t events, void *cb)
{
	struct ev_table *evt = evp->evt;

	assert(evt);

	if (op == OP_REG) {
		/* NOTE: we do not register new callback if correponding 'map' is
		 * occupied */
		if (events & evp->cb_map) {
			/* callback overwrite error */
			errno = EINVAL;
			return -1;
		} else if (events & evt->map) {
			/* event registration conflict */
			struct ev_table *nevt;
			if (!(nevt = FindReusableEvT(evp, evb, events, cb))) {
				nevt = MPAllocateChunk(mtcp->evt_pool);
				assert(nevt);
				TAILQ_INSERT_HEAD(&evb->evth, nevt, link);
			}

			/* register callback */
			if (nevt != evt)
				RegCbWCpy(evp, nevt, events, cb);
			else
				RegCbWoCpy(evp, events, cb);

		} else {
			/* reuse default event table */
			RegCbWoCpy(evp, events, cb);
		}
	} else /* if (op == OP_UNREG) */ {
		if ((events & evp->cb_map) != events) {
			/* unregister unexisting callback error */
			errno = EINVAL;
			return -1;
		} else {
			/* unregister callback */
			UnregCb(evp, events);
		}
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
static inline int
ModifyCallback(mctx_t mctx, int op, int sockid, event_t events,
		                       int hook_point, void* callback)
{
	socket_map_t socket;
	struct ev_pointer *evp;
	struct ev_base *evb;

	assert(op == OP_REG || op == OP_UNREG);

	if ((events & (g_bev_map | g_ude_map)) != events) {
		errno = EINVAL;
		return -1;
	}

	if ((op == OP_REG) && !callback)
		return -1;

	mtcp_manager_t mtcp = GetMTCPManager(mctx);
	if (!mtcp)
		return -1;

	socket = &mtcp->msmap[sockid];

	if (socket->socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE) {
		if (hook_point == MOS_NULL) {
			evp = &socket->monitor_stream->dontcare_evp;
			evb = &socket->monitor_stream->monitor_listener->dontcare_evb;
		} else if (hook_point == MOS_HK_RCV) {
			evp = &socket->monitor_stream->pre_tcp_evp;
			evb = &socket->monitor_stream->monitor_listener->pre_tcp_evb;
		} else if (hook_point == MOS_HK_SND) {
			evp = &socket->monitor_stream->post_tcp_evp;
			evb = &socket->monitor_stream->monitor_listener->post_tcp_evb;
		} else
			return -1;

	} else if (socket->socktype == MOS_SOCK_MONITOR_STREAM
			|| socket->socktype == MOS_SOCK_MONITOR_RAW) {
		if (hook_point == MOS_NULL)
			evb = &socket->monitor_listener->dontcare_evb;
		else if (hook_point == MOS_HK_RCV)
			evb = &socket->monitor_listener->pre_tcp_evb;
		else if (hook_point == MOS_HK_SND)
			evb = &socket->monitor_listener->post_tcp_evb;
		else
			return -1;

		evp = &evb->dflt_evp;
	} else {
		errno = EINVAL;
		return -1;
	}

	return ModCb(mtcp, op, evp, evb, events, callback);
}
/*----------------------------------------------------------------------------*/
int
mtcp_register_callback(mctx_t mctx, int sockid, event_t events,
		                       int hook_point, callback_t callback)
{
	if (!callback) {
		errno = EINVAL;
		return -1;
	}

	return ModifyCallback(mctx, OP_REG, sockid, events, hook_point, callback);
}
/*----------------------------------------------------------------------------*/
int
mtcp_unregister_callback(mctx_t mctx, int sockid, event_t events,
		                       int hook_point)
{
	return ModifyCallback(mctx, OP_UNREG, sockid, events, hook_point, NULL);
}
/*----------------------------------------------------------------------------*/
event_t
mtcp_define_event(event_t event, filter_t filter)
{
	int i, j;
	int evid;

	if (!IS_PO2(event))
		return 0;

	if (!filter)
		return 0;

	for (i = 0; i < MAX_EV; i++)
		if (event == 1L << i) {
			evid = i;
			break;
		}
	if (i == MAX_EV)
		return 0;

	for (i = 0; i < NUM_UDE; i++) {
		const event_t ude = 1L << (i + UDE_OFFSET);
		if (g_ude_map & ude)
			continue;

		for (j = 0; j < NUM_UDE; j++)
			if (g_ude_id[evid][j] == -1) {
				g_ude_id[evid][j] = i + UDE_OFFSET;
				break;
			}
		if (j == NUM_UDE)
			return 0;

		/* Now we have valid UDE */

		/* update ancestor's descendants map */
		event_t ancestors = event |
			((evid >= UDE_OFFSET) ? g_udes[evid - UDE_OFFSET].ancestors : 0);
		EVENT_FOREACH_START(ev, j, BEV_OFFSET, MAX_EV, ancestors) {
			g_descendants[j] |= ude;
		} EVENT_FOREACH_END(ev, ancestors);

		/* update my ancestor map */
		if (event & g_ude_map)
			g_udes[i].ancestors = event | g_udes[evid - UDE_OFFSET].ancestors;
		else
			g_udes[i].ancestors = event;

		g_udes[i].parent = evid;
		g_udes[i].ft = filter;
		g_ude_map |= ude;
		
		return ude;
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
int
mtcp_remove_ude(event_t event)
{
	/* FIXME: this function is not implemented yet.
	 * What should we do if we remove UDE while running? */
	if (!IS_PO2(event))
		return -1;

	if (!(g_ude_map & event))
		return -1;

	return -1;
}
/*----------------------------------------------------------------------------*/
#define PUSH(sp, ev_idx, ft_idx, data) \
do { \
	sp->ev_idx = ev_idx; \
	sp->ft_idx = ft_idx; \
	sp->data.u64 = data.u64; \
	sp++; \
} while (0)
#define POP(sp, ev_idx, ft_idx, data) \
do { \
	sp--; \
	ev_idx = sp->ev_idx; \
	ft_idx = sp->ft_idx; \
	data.u64 = sp->data.u64; \
} while (0)
/*----------------------------------------------------------------------------*/
/**
 * TODO: 
 * - Donghwi, please change POST_TCP names to POST_SND &
 * PRE_TCP names to POST_RCV.
 *
 * - Please make sure that the order of event invocations is:
 * MOS_ON_CONN_START --> .. MOS_ON_* .. --> MOS_ON_CONN_END
 */
inline void
HandleCallback(mtcp_manager_t mtcp, uint32_t hook, 
		socket_map_t socket, int side, struct pkt_ctx *pctx, event_t events)
{
	struct sfbpf_program fcode;
	int8_t ude_id;
	uint64_t cb_map, ft_map;

	int8_t ev_idx, ft_idx;
	event_data_t data;

	if (!socket)
		return;

	if (!events)
		return;
	assert(events);

	/* if client side monitoring is disabled, then skip */
	if (side == MOS_SIDE_CLI && socket->monitor_stream->client_mon == 0)
		return;
	/* if server side monitoring is disabled, then skip */
	else if (side == MOS_SIDE_SVR && socket->monitor_stream->server_mon == 0)
		return;

#define MSTRM(sock) (sock)->monitor_stream
#define MLSNR(sock) (sock)->monitor_listener
	/* We use `?:` notation instead of `if/else` to make `evp` as const */
	struct ev_pointer * const evp =
		(socket->socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE) ?
		    ((hook == MOS_HK_RCV)  ? &MSTRM(socket)->pre_tcp_evp :
			 (hook == MOS_HK_SND) ? &MSTRM(socket)->post_tcp_evp :
									  &MSTRM(socket)->dontcare_evp)
		: (socket->socktype == MOS_SOCK_MONITOR_STREAM)
		  || (socket->socktype == MOS_SOCK_MONITOR_RAW) ?
		    ((hook == MOS_HK_RCV)  ? &MLSNR(socket)->pre_tcp_evb.dflt_evp :
			 (hook == MOS_HK_SND) ? &MLSNR(socket)->post_tcp_evb.dflt_evp :
									  &MLSNR(socket)->dontcare_evb.dflt_evp) :
		 NULL;

	if (!evp || !((cb_map = events & evp->cb_map) || (g_ude_map & evp->cb_map)))
		return;

	/* mtcp_bind_monitor_filter()
     * - BPF filter is evaluated only for RAW socket and PASSIVE socket (= orphan filter)
	 * - stream syn filter is moved to and evaluated on socket creation */
    if (socket->socktype == MOS_SOCK_MONITOR_STREAM) {
		fcode = MLSNR(socket)->stream_orphan_fcode;
		/* if not match with filter, return */
		if (ISSET_BPFFILTER(fcode) && pctx && EVAL_BPFFILTER(fcode, 
		    (uint8_t *)pctx->p.iph - sizeof(struct ethhdr), 
	        pctx->p.ip_len + sizeof(struct ethhdr)) == 0)
			return;
	}
    if (socket->socktype == MOS_SOCK_MONITOR_RAW) {
		fcode = MLSNR(socket)->raw_pkt_fcode;
		/* if not match with filter, return */
		if (ISSET_BPFFILTER(fcode) && pctx && EVAL_BPFFILTER(fcode, 
		    (uint8_t *)pctx->p.iph - sizeof(struct ethhdr), 
	        pctx->p.ip_len + sizeof(struct ethhdr)) == 0)
			return;
	}

	ft_map = events & evp->ft_map;

	event_t bev_map = cb_map | ft_map;
	struct ev_table * const evt = evp->evt;

	struct {
		int8_t ev_idx;
		int8_t ft_idx;
		event_data_t data;
	} stack[NUM_UDE + 1], *sp = stack;

	mtcp->pctx = pctx; /* for mtcp_getlastpkt() */
	mctx_t const mctx = g_ctx[mtcp->ctx->cpu];

	EVENT_FOREACH_START(bev, bidx, BEV_OFFSET, BEV_OFFSET + NUM_BEV, bev_map) {
		ev_idx = bidx;
		ft_idx = 0;
		data.u64 = 0;
		const event_t descendants = g_descendants[ev_idx];
		if (descendants) {
			cb_map |= descendants & evp->cb_map;
			ft_map |= descendants & evp->ft_map;
		}

		while (1) {
			const uint64_t ev = (1L << ev_idx);

			if (cb_map & ev) {
				/* call callback */
				evt->ent[ev_idx].cb(mctx, socket->id, side, ev, data);

				if (!(cb_map &= ~ev))
					return;
			}

			while (1) {
				event_data_t tmpdata;
				if (ft_idx >= NUM_UDE
						|| (ude_id = g_ude_id[ev_idx][ft_idx]) < 0) {
					/* done with this event */
					if (sp == stack /* stack is empty */) {
						/* go to next built-in event */
						goto __continue;
					} else {
						POP(sp, ev_idx, ft_idx, data);
						ft_idx++;
					}
					break;
				}

				assert(ude_id >= UDE_OFFSET && ude_id < MAX_EV);

				if (((1L << ude_id) & (cb_map | ft_map)) &&
					(tmpdata.u64 = g_udes[ude_id - UDE_OFFSET].ft(mctx, socket->id, side, ev, data))) {
					/* DFS jump */
					PUSH(sp, ev_idx, ft_idx, data);
					ev_idx = ude_id;
					ft_idx = 0;
					data.u64 = tmpdata.u64;
					break;
				}

				ft_idx++;
			}
		}
	}
__continue:
	EVENT_FOREACH_END(bev, bev_map);
}
/*----------------------------------------------------------------------------*/

#endif
