#ifndef __EVENT_CALLBACK_H_
#define __EVENT_CALLBACK_H_

#ifndef NEWEV

#include <stdint.h>
#include <sys/queue.h>
#include "mos_api.h"
/*----------------------------------------------------------------------------*/
typedef struct socket_map * socket_map_t;

/* configuration */
#define NUM_EV_TABLE 		100

/* event representation
 * identification (ID): type: event_t (uint64_t), range: 0x1, 0x2, 0x4, ...
 * index (IDX):         type: int8_t,             range: 0 ~ 63 */

/** maximum number of event (== 64) */
#define MAX_EV 			(8 * sizeof(event_t))
#define BEV_OFFSET 		0
#define UDE_OFFSET 		32
#define NUM_BEV 		12

struct ev_table {
	struct {
		callback_t cb;    /**< callback function pointer */
		int ref;        /**< number of flows which are referring this `cb` */
	} ent[MAX_EV];      /**< table entry */

	event_t map; /**< A bit is 1 if the corresponding `ent` is occupied
				      by any flow. This is to speed up callback registration. */

	TAILQ_ENTRY(ev_table) link; /**< TAILQ link */
};

struct ev_pointer {
	uint64_t cb_map;      /**< map of registered callback */
	uint64_t ft_map;      /**< map of registered fltrs. Automatically updated by
							   `mtcp_register_callback()` or
							   `mtcp_unregister_callback()` */

	struct ev_table *evt; /**< pointer to `struct ev_table` in
							   `struct ev_base` */
};

struct ev_base {
	/** Default event pointer.
	 * This will be copied to every new monitor stream socket.
	 * dflt_evp.evt always points default event table. */
	struct ev_pointer dflt_evp;
	/** List of event tables.
	 * TAILQ_FIRST(`evth`) will return current default event table. */
	TAILQ_HEAD(, ev_table) evth;
};
/*----------------------------------------------------------------------------*/
/* Event's global initialization (only one CPU core should run this) */
void
GlobInitEvent(void);

void
InitEvent(mtcp_manager_t mtcp, int num_evt);

void
InitEvP(struct ev_pointer *evp, struct ev_base *evb);

void
CleanupEvP(struct ev_pointer *evp);

void
InitEvB(mtcp_manager_t mtcp, struct ev_base *evb);

void
CleanupEvB(mtcp_manager_t mtcp, struct ev_base *evb);

extern inline void
HandleCallback(mtcp_manager_t mtcp, uint32_t hook,
		socket_map_t socket, int side, struct pkt_ctx *pctx, event_t events);

#endif

#endif /* __EVENT_CALLBACK_H_ */
