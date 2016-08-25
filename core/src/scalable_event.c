#ifdef NEWEV

/* ----------------------------------------------------------------------------
 * Please see 4 Jul. 2015 meeting slides for conceptual explanation.
 * 'Subtree' has special meaning and its 'ID' is very important concept for
 * understanding this code.
 *
 * Please contact Donghwi Kim for further questions,
 * via e-mail: dhkim09a@gmail.com */
#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tree.h>
#include <key_value_store.h>
#include "scalable_event.h"

#include "mtcp.h"
#include "mos_api.h"
#include "mtcp_api.h"

#define NEWHASH
#ifdef NEWHASH
#include "mtcp_util.h"
#define XXHSEED 18446744073709551557ULL
#endif
/*----------------------------------------------------------------------------*/
#define POWER_OF_TWO(x) (!((x - 1) & (x)))

#define MAX_EVENTS  30240
#define KVS_BUCKETS 1024
#define KVS_ENTRIES 102400
#define MAX_DEPTH   MAX_EVENTS
#define NEWID(id, ev, cb) (id ^ hash64(ev, cb))
#define IS_UDE(e) ((e)->ev >= (1 << NUM_BEV))
#define IS_FLOATING_EVENT(e) (!(e)->ft && IS_UDE(e))

struct _tree_node_t *g_evroot;
event_t g_evid;

static __thread stree_t *g_cur_stree = NULL;
static __thread tree_node_t *g_cur_ev = NULL;

static __thread struct {
	struct _tree_node_t *stack[MAX_EVENTS];
	struct _tree_node_t **sp;
} g_evstack;

#define free(x) do { \
	assert(x); \
	free(x); \
} while (0)
/*----------------------------------------------------------------------------*/
/** Create a new tree node
 * @return the new tree node
 * */
inline tree_node_t *
tree_node_new()
{
	return calloc(1, sizeof(tree_node_t));
}
/*----------------------------------------------------------------------------*/
/** Delete a tree node */
inline void
tree_node_del(tree_node_t *node)
{
	free(node);
}
/*----------------------------------------------------------------------------*/
/** Delete a tree node and all of its descendants
 * @param [in] node: target node
 * */
inline void
tree_del_recursive(tree_node_t *node)
{
	struct _tree_node_t *walk = NULL, *del = NULL;
	TREE_SCRATCH(_tree_node_t, MAX_DEPTH) stack;

	TREE_DFS_FOREACH(walk, node, &stack, link) {
		if (del)
			tree_node_del(del);
		del = walk;
	}
	if (del)
		tree_node_del(del);
}
/*----------------------------------------------------------------------------*/
/** Search a tree node by event ID
 * @param [in] root: root node
 * @param [in] ev: event ID
 * @return target node
 *
 * The search is in DFS mannar
 * */
inline tree_node_t *
tree_search(tree_node_t *root, event_t ev)
{
	struct _tree_node_t *walk = NULL;
	TREE_SCRATCH(_tree_node_t, MAX_DEPTH) stack;

	TREE_DFS_FOREACH(walk, root, &stack, link)
		if (walk->ev == ev) {
			return walk;
		}
	return NULL;
}
/*----------------------------------------------------------------------------*/
/** Create a new subtree
 * @param [in] id: subtree ID
 * @param [in] root: root of the subtree
 * @return new subtree
 *
 * Allocate and initialize subtree structure, stree_t:
 *   1. Allocate the structure.
 *   2. Initialize reference counter and fill ID field.
 *   3. Set 'root' pointer which points the root node of a tree.
 *      The tree should contain all required tree nodes.
 *   4. Set built-in event tree node pointers.
 * */
inline stree_t *
stree_create(uint64_t id, fevq_t *fevq, tree_node_t *root)
{
	int i;
	tree_node_t *bev;
	stree_t *nstree = malloc(sizeof(stree_t));
	if (!nstree)
		return NULL;

	nstree->ref_cnt = 0;
	nstree->id = id;
	nstree->root = root;
	for (i = 0; i < NUM_BEV; i++)
		nstree->bevs[i] = NULL;

	for (bev = TREE_FIRSTBORN(root, link); bev; bev = TREE_YOUNGER(bev, link)) {
		int idx = 0;
		while (bev->ev >> (idx + 1))
			idx++;
		nstree->bevs[idx] = bev;
	}

	nstree->floating_evq = *fevq;

	return nstree;
}
/*----------------------------------------------------------------------------*/
/** Destroy the subtree
 * @param [in] stree: the target subtree */
inline void
stree_destroy(kvs_t *store, stree_t *stree)
{
	tree_node_t *w;
	TAILQ_FOREACH(w, &stree->floating_evq, flink)
		kvs_remove(store, w->key);
	tree_del_recursive(stree->root);
	free(stree);
}
/*----------------------------------------------------------------------------*/
/** Increase reference counter of the subtree
 * @param [in] stree: the target subtree */
inline void
stree_inc_ref(stree_t *stree)
{
	stree->ref_cnt++;
}
/*----------------------------------------------------------------------------*/
/** Decrease reference counter of the subtree, and remove it from the key-
 * value-store if needed
 * @param [in] store: the key-value-store
 * @param [in] stree: the target subtree 
 *
 * In addition to decreasing the reference counter, remove the subtree from 
 * key-value-store if decreased reference counter is zero.
 * */
inline void
stree_dec_ref(kvs_t *store, stree_t *stree)
{
	if (stree && !--stree->ref_cnt) {
		kvs_remove(store, stree->id);
		stree_destroy(store, stree);
	}
}
/*----------------------------------------------------------------------------*/
inline void
stree_reg_floating_event(kvs_t *store, uint64_t id,
						 fevq_t *fevq, tree_node_t *ev)
{
	struct {
		event_t ev;
		uint64_t sid;
	} instance;

	instance.ev = ev->ev;
	instance.sid = id;

	ev->key = XXH64(&instance, sizeof(instance), XXHSEED);

	kvs_insert(store, ev->key, ev);

	TAILQ_INSERT_TAIL(fevq, ev, flink);
}
/*----------------------------------------------------------------------------*/
/** Hash the event ID and callback pair
 * @param [in] ev: the event ID
 * @param [in] cb: the callback function pointer
 * @return hash value
 *
 * TODO: Replace 32-bit Jenkins hash with better one.
 * */
inline static uint64_t
hash64(event_t ev, callback_t cb)
{
#ifdef NEWHASH
	struct {
		event_t ev;
		callback_t cb;
	} instance;

	instance.ev = ev;
	instance.cb = cb;

	return XXH64(&instance, sizeof(event_t) + sizeof(callback_t), XXHSEED);
#else
	uint64_t hash = 0;
	int i;

	for (i = 0; i < sizeof(ev); ++i) {
		hash += ((uint8_t *)&ev)[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}
	for (i = 0; i < sizeof(cb); ++i) {
		hash += ((uint8_t *)&cb)[i];
		hash += (hash << 10);
		hash ^= (hash >> 6);
	}
	hash += (hash << 3);
	hash ^= (hash >> 11);
	hash += (hash << 15);

	return hash;
#endif
}
/*----------------------------------------------------------------------------*/
/** Register a callback
 * @param [in] store: the key-value-store for subtrees
 * @param [in, out] pstree: the target subtree pointer
 * @param [in] ev: the target event
 * @param [in] cb: the callback to be registered
 * @return 0 for success, -1 for failure
 *
 * For callback registeration, there can be some cases.
 *   1. A new tree what we want is already there, so we can use it by simply
 *      updating reference counters.
 *   2. A new tree what we want is not there, so we are going to create a
 *      new subtree. The new subtree will not only have new callback, but also
 *      inherit previous subtree.
 *      2.1. The reference counter of previous subtree is 1, so we can avoid
 *           new tree allocation by reusing tree nodes of previous subtree.
 *      2.2. The reference counter of previous subtree is not 1, so we should
 *           copy previous subtree's tree nodes.
 *   3. Previously, there was no subtree (in other words, this is the first
 *      callback registeration) so we are going to make a new subtree which
 *      has only one callback
 * */
inline int
RegCb(kvs_t *store, stree_t **pstree, event_t ev, callback_t cb)
{
#ifdef DBGMSG
	__PREPARE_DBGLOGGING();
#endif
	TRACE_DBG("/>>>>>> start <<<<<<\\\n");
	stree_t *stree, *nstree;
	TREE_SCRATCH(_tree_node_t, MAX_DEPTH) stack;
	tree_node_t *w, *w2, *w3;
	uint64_t id = 0, nid;
	fevq_t fevq;

	TAILQ_INIT(&fevq);

	if ((stree = *pstree)) {
		/* XXX: tree_search() is heavy operation, but it is used for only error
		 * checking. Is there any way to avoid this?
		 * --> We can have a key-value-store for this, but is it too much? */
#if 1
		if (tree_search(stree->root, ev))
			/* Illegal trial of overwriting a previously registerd callback */
			return -1;
#endif

		id = stree->id;
	}

	/* calculate new subtree ID */
	nid = NEWID(id, ev, cb);

	TRACE_DBG("nid: 0x%lX, id: 0x%lx, ev: %ld, cb: %p\n",
			nid, id, ev, cb);

	if ((nstree = kvs_search(store, nid))) {
		/* case 1. */
		TRACE_DBG("Case 1\n");
		/* update reference counters */
		if (stree)
			stree_dec_ref(store, stree);

		nstree->ref_cnt++;
		*pstree = nstree;
	} else if (stree) {
		/* case 2. */
		TRACE_DBG("Case 2\n");
		tree_node_t *ntree = NULL, *sptr;

		w = tree_search(g_evroot, ev);
		assert(w);

		/* create spine: Create a new tree which consists of a tree node with
		 * new callback and the tree node's ancestors. */
		do {
			tree_node_t *ntn;
			if (!(ntn = tree_node_new())) {
				if (ntree)
					tree_del_recursive(ntree);
				return -1;
			}
			ntn->ft = w->ft;
			ntn->cb = cb;
			ntn->ev = w->ev;
			ntn->arg = w->arg;
			if (cb)
				cb = NULL;

			if (IS_FLOATING_EVENT(ntn))
				stree_reg_floating_event(store, nid, &fevq, ntn);

			TREE_INIT_NODE(ntn, link);
			TREE_INIT_NODE(ntn, invk);
			if (ntree) {
				TREE_INSERT_CHILD(ntn, ntree, link);
				if (!IS_FLOATING_EVENT(ntree))
					TREE_INSERT_CHILD(ntn, ntree, invk);
			}
			ntree = ntn;
		} while ((w = TREE_PARENT(w, link)));

		/* attach flesh: Inherit previous subtree */
		sptr = ntree;
		if (stree->ref_cnt == 1) {
			/* case 2.1. */
			TRACE_DBG("Case 2.1\n");
			w = stree->root;
			while (w) {
				tree_node_t *next_walk = NULL,
							*next_walk2 = NULL,
							*next_sptr = TREE_FIRSTBORN(sptr, link);

				for (w2 = TREE_FIRSTBORN(w, link);
					 w2 != NULL;
					 w2 = next_walk2) {
					next_walk2 = TREE_YOUNGER(w2, link);
					if (sptr && w2->ev == next_sptr->ev) {

						assert(next_walk == NULL);

						next_walk = w2;

						assert(next_sptr->ev == next_walk->ev);
						if (next_sptr->cb != next_walk->cb)
							next_sptr->cb = next_walk->cb;
					} else {
						TREE_DETACH(w2, link);
						TREE_DFS_FOREACH(w3, w2, &stack, link)
							if (IS_FLOATING_EVENT(w3)) {
								kvs_remove(store, w3->key);
								stree_reg_floating_event(store, nid, &fevq, w3);
							}
						if (!IS_FLOATING_EVENT(w2))
							TREE_DETACH(w2, invk);
						if (next_walk) {
							TREE_INSERT_CHILD(sptr, w2, link);
							if (!IS_FLOATING_EVENT(w2))
								TREE_INSERT_CHILD(sptr, w2, invk);
						} else {
							tree_node_t *f = TREE_FIRSTBORN(sptr, link);
							TREE_INSERT_OLDER(f, w2, link);
							if (!IS_FLOATING_EVENT(w2)) {
								if ((f = TREE_FIRSTBORN(sptr, invk)))
									TREE_INSERT_OLDER(f, w2, invk);
								else
									TREE_INSERT_CHILD(sptr, w2, invk);
							}
						}
					}
				}
				w = next_walk;
				sptr = next_sptr;
				next_walk = NULL;
			}
		} else {
			/* case 2.2. */
			TRACE_DBG("Case 2.2\n");
			TREE_DFS_FOREACH(w, stree->root, &stack, link) {
				tree_node_t *ntn, *ptn;

				if (sptr && w->ev == sptr->ev) {
					if (w->cb != sptr->cb)
						sptr->cb = w->cb;
					sptr = TREE_FIRSTBORN(sptr, link);
					continue;
				}

				if (!(ntn = tree_node_new())) {
					if (ntree)
						tree_del_recursive(ntree);
					return -1;
				}
				ntn->ft = w->ft;
				ntn->cb = w->cb;
				ntn->ev = w->ev;
				ntn->arg = w->arg;

				if (IS_FLOATING_EVENT(ntn))
					stree_reg_floating_event(store, nid, &fevq, ntn);

				TREE_INIT_NODE(ntn, link);
				TREE_INIT_NODE(ntn, invk);

				if (TREE_PARENT(w, link)) {
					ptn = tree_search(ntree, TREE_PARENT(w, link)->ev);

					assert(ptn);

					TREE_INSERT_CHILD(ptn, ntn, link);
					if (!IS_FLOATING_EVENT(ntn))
						TREE_INSERT_CHILD(ptn, ntn, invk);
				} else
					assert(0);
			}
		}

		nstree = stree_create(nid, &fevq, ntree);

		assert(nstree);

		nstree->ref_cnt = 1;

		kvs_insert(store, nid, nstree);

		/* update reference counters */
		stree_dec_ref(store, stree);

		*pstree = nstree;
	} else /* if (!stree) */ {
		/* case 3. */
		TRACE_DBG("Case 3\n");
		tree_node_t *ntree = NULL;

		w = tree_search(g_evroot, ev);
		assert(w);

		/* create spine: Create a new tree which consists of a tree node with
		 * new callback and the tree node's ancestors. */
		do {
			tree_node_t *ntn;
			if (!(ntn = tree_node_new())) {
				if (ntree)
					tree_del_recursive(ntree);
				return -1;
			}
			ntn->ft = w->ft;
			ntn->cb = cb;
			ntn->ev = w->ev;
			ntn->arg = w->arg;
			if (cb)
				cb = NULL;

			if (IS_FLOATING_EVENT(ntn))
				stree_reg_floating_event(store, nid, &fevq, ntn);

			TREE_INIT_NODE(ntn, link);
			TREE_INIT_NODE(ntn, invk);
			if (ntree) {
				TREE_INSERT_CHILD(ntn, ntree, link);
				if (!IS_FLOATING_EVENT(ntree))
					TREE_INSERT_CHILD(ntn, ntree, invk);
			}
			ntree = ntn;
		} while ((w = TREE_PARENT(w, link)));

		nstree = stree_create(nid, &fevq, ntree);

		assert(nstree);

		nstree->ref_cnt = 1;

		kvs_insert(store, nid, nstree);

		*pstree = nstree;
	}

	TRACE_DBG("\\>>>>>>  end  <<<<<</\n");
	return 0;
}
/*----------------------------------------------------------------------------*/
/** Unregister a callback
 * @param [in] store: the key-value-store for subtrees
 * @param [in, out] pstree: the target subtree pointer
 * @param [in] ev: the target event
 * @return 0 for success, -1 for failure
 *
 * For callback unregisteration, there can be some cases.
 *   1. No registered callback will be left after this unregistration.
 *   2. A new tree what we want is already there, so we can use it by simply
 *      updating reference counters.
 *   3. A new tree what we want is not there, so we create a new subtree by
 *      copying a part of previous subtree
 * */
inline int
UnregCb(kvs_t *store, stree_t **pstree, event_t ev)
{
#ifdef DBGMSG
	__PREPARE_DBGLOGGING();
#endif
	TRACE_DBG("/>>>>>> start <<<<<<\\\n");
	stree_t *stree, *nstree;
	TREE_SCRATCH(_tree_node_t, MAX_DEPTH) stack;
	tree_node_t *w, *target;
	fevq_t fevq;
	uint64_t id = 0, nid;
	callback_t cb;

	if (!(stree = *pstree) || !(target = tree_search(stree->root, ev)) ||
		!(cb = target->cb))
		/* Illegal trial of unregistering a callback which is never registered
		 * before. */
		return -1;

	id = stree->id;

	/* Calculate new subtree ID */
	nid = NEWID(id, ev, cb);

	TRACE_DBG("nid: 0x%lX, id: 0x%lx, ev: %ld, cb: %p\n",
			nid, id, ev, cb);

	if (nid == 0) {
		/* case 1. */
		TRACE_DBG("Case 1\n");
		/* update reference counters */
		if (stree)
			stree_dec_ref(store, stree);

		*pstree = NULL;
	} else if ((nstree = kvs_search(store, nid))) {
		/* case 2. */
		TRACE_DBG("Case 2\n");
		/* update reference counters */
		if (stree)
			stree_dec_ref(store, stree);

		nstree->ref_cnt++;
		*pstree = nstree;
	} else if (stree) {
		/* case 3. */
		TRACE_DBG("Case 3\n");
		bool proceed;
		tree_node_t *ntree = NULL, *sptr;

		sptr = TREE_PARENT(target, link);
#define TREE_IS_ONLY_CHILD(root, x, field) \
		(TREE_PARENT((x), field) ? \
		 TREE_PARENT((x), field)->field.tn_first == (x) && \
		 TREE_PARENT((x), field)->field.tn_last == (x) : \
		 (x) == (root) && (x)->field.tn_younger == NULL)
		while (sptr && !sptr->cb && TREE_IS_ONLY_CHILD(stree->root, sptr, link))
			sptr = TREE_PARENT(sptr, link);

		assert(sptr);
		/* 'sptr == NULL' means the tree will lose the only callback.
		 * This case should be handled by 'Case 1' */

		TREE_DFS_FOREACH_SELECTIVE(w, stree->root, &stack, link, proceed) {
			tree_node_t *ntn, *ptn;

			if (w == sptr)
				proceed = false;

			if (!(ntn = tree_node_new())) {
				/* free incomplete ntree */
				if (ntree)
					tree_del_recursive(ntree);
				return -1;
			}
			ntn->ft = w->ft;
			ntn->cb = w->cb;
			ntn->ev = w->ev;
			ntn->arg = w->arg;

			if (IS_FLOATING_EVENT(ntn))
				stree_reg_floating_event(store, nid, &fevq, ntn);

			TREE_INIT_NODE(ntn, link);
			TREE_INIT_NODE(ntn, invk);

			if (TREE_PARENT(w, link)) {
				assert(ntree);
				ptn = tree_search(ntree, TREE_PARENT(w, link)->ev);

				assert(ptn);

				TREE_INSERT_CHILD(ptn, ntn, link);
				if (!IS_FLOATING_EVENT(ntn))
					TREE_INSERT_CHILD(ptn, ntn, invk);
			} else
				/* this is the root node */
				ntree = ntn;
		}

		nstree = stree_create(nid, &fevq, ntree);

		assert(nstree);

		nstree->ref_cnt = 1;

		kvs_insert(store, nid, nstree);

		/* update reference counters */
		stree_dec_ref(store, stree);

		*pstree = nstree;
	} else /* if (!stree) */
		return -1;

	TRACE_DBG("\\>>>>>>  end  <<<<<</\n");
	return 0;
}
/*----------------------------------------------------------------------------*/
inline int
ModCb(kvs_t *store, stree_t **pstree, event_t ev, callback_t cb)
{
	assert(*pstree || cb);

	/* Event ID starts from 1 */
	if (ev == 0)
		return -1;

	if (cb)
		return RegCb(store, pstree, ev, cb);
	else
		return UnregCb(store, pstree, ev);
}
/*----------------------------------------------------------------------------*/
void
GlobInitEvent(void)
{
	int i;

	if (!(g_evroot = tree_node_new())) {
		perror("FATAL: Failed to allocate essentials for the system\n");
		exit(0);
	}
	g_evroot->ev = 0;
	g_evroot->ft = NULL;
	g_evroot->cb = NULL;
	TREE_INIT_NODE(g_evroot, link);
	TREE_INIT_NODE(g_evroot, invk);
	for (i = 0; i < NUM_BEV; i++) {
		tree_node_t *ntn;
		if (!(ntn = tree_node_new())) {
			perror("FATAL: Failed to allocate essentials for the system\n");
			exit(0);
		}
		ntn->ev = 1 << i;
		ntn->ft = NULL;
		ntn->cb = NULL;
		TREE_INIT_NODE(ntn, link);
		TREE_INIT_NODE(ntn, invk);
		TREE_INSERT_CHILD(g_evroot, ntn, link);
		TREE_INSERT_CHILD(g_evroot, ntn, invk);
	}
	g_evid = 1 <<  NUM_BEV;
}
/*----------------------------------------------------------------------------*/
void
InitEvent(mtcp_manager_t mtcp)
{
	mtcp->ev_store = kvs_create(KVS_BUCKETS, KVS_ENTRIES);
}
/*----------------------------------------------------------------------------*/
event_t
mtcp_alloc_event(event_t event)
{
	tree_node_t *parent, *new;
	if (!(parent = tree_search(g_evroot, event)))
		return MOS_NULL_EVENT;

	if ((new = tree_node_new()) == NULL)
		return MOS_NULL_EVENT;

	new->ev = g_evid++;
	new->ft = NULL;
	new->cb = NULL;
	new->arg.arg = NULL;
	new->arg.len = 0;

	TREE_INIT_NODE(new, link);
	TREE_INIT_NODE(new, invk);

	TREE_INSERT_CHILD(parent, new, link);

	return new->ev;
}
/*----------------------------------------------------------------------------*/
event_t
mtcp_define_event(event_t event, filter_t filter, struct filter_arg *arg)
{
#ifdef DBGMSG
	__PREPARE_DBGLOGGING();
#endif
	tree_node_t *parent, *new, *walk;

	if (!filter)
		return MOS_NULL_EVENT;
	if (arg && arg->len > 0 && !arg->arg)
		return MOS_NULL_EVENT;
	if (!(parent = tree_search(g_evroot, event)))
		return MOS_NULL_EVENT;
	for (walk = TREE_FIRSTBORN(parent, link);
		 walk;
		 walk = TREE_YOUNGER(walk, link))
		if ((walk->ft == filter) &&
			((walk->arg.arg == NULL && (!arg || !arg->arg || arg->len <= 0)) ||
			 (walk->arg.len == arg->len &&
			  memcmp(walk->arg.arg, arg->arg, arg->len) == 0)))
			return walk->ev;

	if ((new = tree_node_new()) == NULL)
		return MOS_NULL_EVENT;

	new->ev = g_evid++;
	new->ft = filter;
	new->cb = NULL;
	if (!arg || !arg->arg || arg->len <= 0) {
		new->arg.arg = NULL;
		new->arg.len = 0;
	} else {
		new->arg.arg = malloc(arg->len);
		memcpy(new->arg.arg, arg->arg, arg->len);
		new->arg.len = arg->len;
	}

	TREE_INIT_NODE(new, link);
	TREE_INIT_NODE(new, invk);

	TRACE_DBG("parent: %ld\n", parent->ev);
	TREE_INSERT_CHILD(parent, new, link);
	TREE_INSERT_CHILD(parent, new, invk);

	return new->ev;
}
/*----------------------------------------------------------------------------*/
#define EVENT_STACK_PUSH(s, var) \
	(assert(&(s)->stack[sizeof((s)->stack) / sizeof(void *)] > (s)->sp), \
	 *(++((s)->sp)) = (var))
#define EVENT_STACK_POP(s) \
	(*(((s)->sp)--))
#define EVENT_STACK_CLR(s) \
	((s)->stack[0] = NULL, (s)->sp = (s)->stack)

inline int
RaiseEv(kvs_t *store, event_t event)
{
	tree_node_t *ev = NULL;
#if 1
	struct {
		event_t ev;
		uint64_t sid;
	} instance;

	instance.ev = event;
	instance.sid = g_cur_stree->id;

	_key_t key = XXH64(&instance, sizeof(instance), XXHSEED);
	ev = (tree_node_t *)kvs_search(store, key);
#else
	tree_node_t *walk;
	for (walk = TREE_FIRSTBORN(g_cur_ev, link);
		 walk;
		 walk = TREE_YOUNGER(walk, link)) {
		if (walk->ev == event && IS_FLOATING_EVENT(walk)) {
			ev = walk;
			break;
		}
	}
#endif

	if (!ev)
		return -1;

	if (!ev->is_in_raiseq) {
		ev->is_in_raiseq = true;
		EVENT_STACK_PUSH(&g_evstack, ev);
	}

	return 0;
}
int
mtcp_raise_event(mctx_t mctx, event_t event)
{
	mtcp_manager_t mtcp = GetMTCPManager(mctx);
	if (!mtcp)
		return -1;

	return RaiseEv(mtcp->ev_store, event);
}
/*----------------------------------------------------------------------------*/
#define PRINT_EV(event, field) \
	printf("[%s](node: %p) ev: %ld, older: %ld, younger: %ld, child: %ld, " \
			"last: %ld, parent: %ld, ft: %p, cb: %p\n", \
		  #field, (event), (event)->ev, \
		  (event)->field.tn_older ? (event)->field.tn_older->ev : -1, \
		  (event)->field.tn_younger ? (event)->field.tn_younger->ev : -1, \
		  (event)->field.tn_first ? (event)->field.tn_first->ev : -1, \
		  (event)->field.tn_last ? (event)->field.tn_last->ev : -1, \
		  (event)->field.tn_parent ? (event)->field.tn_parent->ev : -1, \
		  (event)->ft, (event)->cb)
inline void
HandleCb(mctx_t const mctx, int sock, int side,
		 stree_t * const stree, event_t events)
{
	int i;
	bool proceed;
	tree_node_t *walk, *bev, *ev;
	TREE_SCRATCH(_tree_node_t, MAX_DEPTH) queue;
	assert(stree);

	g_cur_stree = stree;

	for (i = 0; i < NUM_BEV; i++) {
		if (!((1L << i) & events))
			continue;

		if (!(bev = stree->bevs[i]))
			continue;

		EVENT_STACK_CLR(&g_evstack);
		tree_node_t **ptr = g_evstack.sp;
		TREE_DFS_FOREACH_SELECTIVE(walk, bev, &queue, invk, proceed) {
			//PRINT_EV(walk, link);
			//PRINT_EV(walk, invk);

			g_cur_ev = walk;

			if (walk != bev && walk->ft &&
				walk->ft(mctx, sock, side, walk->ev, &walk->arg) == false) {
				ptr = g_evstack.sp;
				proceed = false;
				continue;
			}

			if (walk->cb)
				walk->cb(mctx, sock, side, walk->ev, &walk->arg);

			while (ptr < g_evstack.sp) {
				ptr++;
				TREE_INSERT_CHILD(walk, *ptr, invk);
			}
		}
		g_cur_ev = NULL;

		while ((ev = EVENT_STACK_POP(&g_evstack))) {
			ev->is_in_raiseq = false;
			TREE_DETACH(ev, invk);
		}

		if (!(events &= ~(1L << i))) {
			g_cur_stree = NULL;
			return;
		}
	}

	g_cur_stree = NULL;
}
/*----------------------------------------------------------------------------*/
int
mtcp_register_callback(mctx_t mctx, int sockid, event_t event,
		                       int hook_point, callback_t callback)
{
	socket_map_t socket;
	stree_t **pstree;

	if (event == MOS_NULL_EVENT ||
		event < (1L << NUM_BEV) ? !POWER_OF_TWO(event) : 0)
		return -1;

	mtcp_manager_t mtcp = GetMTCPManager(mctx);
	if (!mtcp)
		return -1;

	socket = &mtcp->msmap[sockid];

	if (socket->socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE) {
		if (hook_point == MOS_NULL)
			pstree = &socket->monitor_stream->stree_dontcare;
		else if (hook_point == MOS_HK_RCV)
			pstree = &socket->monitor_stream->stree_pre_rcv;
		else if (hook_point == MOS_HK_SND)
			pstree = &socket->monitor_stream->stree_post_snd;
		else
			return -1;

	} else if (socket->socktype == MOS_SOCK_MONITOR_STREAM
			|| socket->socktype == MOS_SOCK_MONITOR_RAW) {
		if (hook_point == MOS_NULL)
			pstree = &socket->monitor_listener->stree_dontcare;
		else if (hook_point == MOS_HK_RCV)
			pstree = &socket->monitor_listener->stree_pre_rcv;
		else if (hook_point == MOS_HK_SND)
			pstree = &socket->monitor_listener->stree_post_snd;
		else
			return -1;
	} else {
		errno = EINVAL;
		return -1;
	}

	return ModCb(mtcp->ev_store, pstree, event, callback);
}
/*----------------------------------------------------------------------------*/
int
mtcp_unregister_callback(mctx_t mctx, int sockid, event_t event,
		                       int hook_point)
{
	return mtcp_register_callback(mctx, sockid, event, hook_point, NULL);
}
/*----------------------------------------------------------------------------*/
inline void
HandleCallback(mtcp_manager_t mtcp, uint32_t hook, 
		socket_map_t socket, int side, struct pkt_ctx *pctx, event_t events)
{
	struct sfbpf_program fcode;

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
	stree_t * const stree =
		(socket->socktype == MOS_SOCK_MONITOR_STREAM_ACTIVE) ?
		    ((hook == MOS_HK_RCV) ? MSTRM(socket)->stree_pre_rcv :
			 (hook == MOS_HK_SND) ? MSTRM(socket)->stree_post_snd :
									  MSTRM(socket)->stree_dontcare)
		: (socket->socktype == MOS_SOCK_MONITOR_STREAM)
		  || (socket->socktype == MOS_SOCK_MONITOR_RAW) ?
		    ((hook == MOS_HK_RCV) ? MLSNR(socket)->stree_pre_rcv :
			 (hook == MOS_HK_SND) ? MLSNR(socket)->stree_post_snd :
									  MLSNR(socket)->stree_dontcare) :
		 NULL;

	if (!stree)
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

	mctx_t const mctx = g_ctx[mtcp->ctx->cpu];
	mtcp->pctx = pctx; /* for mtcp_getcurpkt() */

	HandleCb(mctx, socket->id, side, stree, events);
}
/*----------------------------------------------------------------------------*/
#endif
