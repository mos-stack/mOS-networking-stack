#ifndef __SCALABLE_EVENT_H__
#define __SCALABLE_EVENT_H__
/*----------------------------------------------------------------------------*/
#ifdef NEWEV

#include <tree.h>
#include <key_value_store.h>
#include <mos_api.h>
/*----------------------------------------------------------------------------*/
#ifndef NDEBUG
#define FLOOR(x) (((x) > 0) ? (x) : 0)
#define gprintf(f, args...) do {	  \
		printf("%-15s:%-4d:%-10.10s: " f,		 \
		       &__FILE__[FLOOR(sizeof(__FILE__) - 16)],	 \
		       __LINE__, __func__, ##args);		 \
	} while (0)
#else /* NDEBUG */
#define gprintf(args...) ((void) 0)
#endif

#ifdef ENABLE_DEBUG_EVENT
#define RAISE_DEBUG_EVENT(mtcp, stream, format, args...)	\
	do {								\
		mtcp_set_debug_string(mtcp, "[%s:%d] "format, __func__, __LINE__, ##args); \
		struct socket_map *walk;				\
		SOCKQ_FOREACH_START(walk, &cur_stream->msocks) {	\
			HandleCallback(mtcp, MOS_NULL, walk, (stream)->side, \
				       NULL, MOS_ON_DEBUG_MESSAGE);	\
		} SOCKQ_FOREACH_END;					\
		mtcp_set_debug_string(mtcp, NULL);			\
	} while (0)
#else
#define RAISE_DEBUG_EVENT(args...) do {} while (0)
#endif

#ifdef NUM_BEV
#undef NUM_BEV
#endif
#define NUM_BEV 12
/*----------------------------------------------------------------------------*/

typedef struct _tree_node_t {
	filter_t          ft;   // filter function
	callback_t        cb;   // callback function
	event_t           ev;   // event id
	struct filter_arg arg;  // filter argument
	//  _key_t key;
	uint32_t is_in_raiseq:1;
	
	TREE_NODE(_tree_node_t) link;   // link in the tree
	TREE_NODE(_tree_node_t) invk;   // inverse link: used for invoking callbacks
	//  TAILQ_ENTRY(_tree_node_t) flink;
} tree_node_t;

typedef struct _stree_t {
	int ref_cnt;
	uint64_t id;
	tree_node_t *root;
	tree_node_t *bevs[NUM_BEV];
} stree_t;
/*----------------------------------------------------------------------------*/
extern inline void
stree_inc_ref(stree_t *stree);
extern inline void
stree_dec_ref(kvs_t *store, stree_t *stree);

void
GlobInitEvent(void);

void
InitEvent(mtcp_manager_t mtcp);

extern inline void
HandleCallback(mtcp_manager_t mtcp, uint32_t hook, 
	       socket_map_t socket, int side, struct pkt_ctx *pctx, event_t events);
/*----------------------------------------------------------------------------*/
#endif
#endif /* __SCALABLE_EVENT_H__ */
