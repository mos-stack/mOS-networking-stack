#pragma once

#include <assert.h>

#define _TREE_NODE(type, qual) \
struct { \
	qual type *tn_parent;   /* parent */ \
	qual type *tn_first;    /* first child */ \
	qual type *tn_last;     /* last child */ \
	qual type *tn_younger;  /* younger sibling */ \
	qual type *tn_older;    /* older sibling */ \
}
#define TREE_NODE(type) _TREE_NODE(struct type,)

#define _TREE_SCRATCH(type, qual, size) \
union { \
	struct { \
		qual type *stack[(size)]; \
		qual type **sp; \
	}; \
	struct { \
		qual type *queue[(size)]; \
		qual type **head; \
		qual type **tail; \
	}; \
}
#define TREE_SCRATCH(type, size) _TREE_SCRATCH(struct type, , size)

#define TREE_INIT_NODE(elm, field) do { \
	(elm)->field.tn_parent = NULL; \
	(elm)->field.tn_first = NULL; \
	(elm)->field.tn_last = NULL; \
	(elm)->field.tn_younger = NULL; \
	(elm)->field.tn_older = NULL; \
} while (0)

#define TREE_INSERT_CHILD(treeelm, elm, field) do { \
	(elm)->field.tn_parent = (treeelm); \
	if ((treeelm)->field.tn_last != NULL) { \
		(treeelm)->field.tn_last->field.tn_younger = (elm); \
		(elm)->field.tn_older = (treeelm)->field.tn_last; \
	} \
	(treeelm)->field.tn_last = (elm); \
	if ((treeelm)->field.tn_first == NULL) \
		(treeelm)->field.tn_first = (elm); \
} while (0)

#define TREE_INSERT_YOUNGER(treeelm, elm, field) do { \
	(elm)->field.tn_parent = (treeelm)->field.tn_parent; \
	if ((treeelm)->field.tn_younger != NULL) { \
		(elm)->field.tn_younger = (treeelm)->field.tn_younger; \
		(elm)->field.tn_younger->field.tn_older = (elm); \
	} \
	else if ((treeelm)->field.tn_parent != NULL) \
		(treeelm)->field.tn_parent->field.tn_last = (elm); \
	(treeelm)->field.tn_younger = (elm); \
	(elm)->field.tn_older = (treeelm); \
} while (0)

#define TREE_INSERT_OLDER(treeelm, elm, field) do { \
	(elm)->field.tn_parent = (treeelm)->field.tn_parent; \
	if ((treeelm)->field.tn_older != NULL) { \
		(elm)->field.tn_older = (treeelm)->field.tn_older; \
		(elm)->field.tn_older->field.tn_younger = (elm); \
	} \
	else if ((treeelm)->field.tn_parent != NULL) \
		(treeelm)->field.tn_parent->field.tn_first = (elm); \
	(treeelm)->field.tn_older = (elm); \
	(elm)->field.tn_younger = (treeelm); \
} while (0)

#define TREE_DETACH(elm, field) do { \
	if ((elm)->field.tn_parent) { \
		if ((elm)->field.tn_parent->field.tn_first == (elm)) \
			(elm)->field.tn_parent->field.tn_first = (elm)->field.tn_younger; \
		if ((elm)->field.tn_parent->field.tn_last == (elm)) \
			(elm)->field.tn_parent->field.tn_last = (elm)->field.tn_older; \
	} \
	if ((elm)->field.tn_younger) \
		(elm)->field.tn_younger->field.tn_older = (elm)->field.tn_older; \
	if ((elm)->field.tn_older) \
		(elm)->field.tn_older->field.tn_younger = (elm)->field.tn_younger; \
	(elm)->field.tn_older = (elm)->field.tn_younger = NULL; \
	(elm)->field.tn_parent = NULL; \
} while (0)

#define __TREE_DFS_PUSH(scratch, var) \
	(assert(&(scratch)->stack[sizeof((scratch)->stack)] > (scratch)->sp), \
	 *(++((scratch)->sp)) = (var))
#define __TREE_DFS_POP(scratch) \
	(*(((scratch)->sp)--))

#define TREE_DFS_FOREACH(var, root, scratch, field) \
	for ((scratch)->stack[0] = NULL, (scratch)->sp = (scratch)->stack, \
		(var) = (root); (var); \
		(var) = (var)->field.tn_first \
		? ((((var)->field.tn_younger && (var) != (root)) \
			? __TREE_DFS_PUSH(scratch, (var)->field.tn_younger) : 0), \
		   (var)->field.tn_first) \
		: ((var)->field.tn_younger && (var) != (root)) \
			? (var)->field.tn_younger : __TREE_DFS_POP(scratch))

#define TREE_DFS_FOREACH_SELECTIVE(var, root, scratch, field, sel) \
	for ((scratch)->stack[0] = NULL, (scratch)->sp = (scratch)->stack, \
		(var) = (root); (sel = true, var); \
		(var) = ((sel) && (var)->field.tn_first) \
		? ((((var)->field.tn_younger && (var) != (root)) \
			? __TREE_DFS_PUSH(scratch, (var)->field.tn_younger) : 0), \
		   (var)->field.tn_first) \
		: ((var)->field.tn_younger && (var) != (root)) \
			? (var)->field.tn_younger : __TREE_DFS_POP(scratch))

#define __TREE_BFS_ENQUEUE(scratch, var) \
	(*(((scratch)->tail)++) = (var))
#define __TREE_BFS_DEQUEUE(scratch) \
	(*(((scratch)->head)++))

#define TREE_BFS_FOREACH(var, root, scratch, field) \
	for ((scratch)->head = (scratch)->tail = (scratch)->queue, \
		(var) = (root); (var); \
		(var) = (var)->field.tn_first \
		? (var)->field.tn_younger \
			? /* enqueue, the younger */ \
			  (__TREE_BFS_ENQUEUE(scratch, (var)->field.tn_first), \
			   (var)->field.tn_younger) \
			: (scratch)->head != (scratch)->tail \
				? /* enqueue, dequeue */ \
				  (__TREE_BFS_ENQUEUE(scratch, (var)->field.tn_first), \
				   __TREE_BFS_DEQUEUE(scratch)) \
				: /* the first */ \
				  (var)->field.tn_first \
		: (var)->field.tn_younger \
			? /* the younger */ \
			  (var)->field.tn_younger \
			: (scratch)->head != (scratch)->tail \
				? /* dequeue */ \
				  __TREE_BFS_DEQUEUE(scratch) \
				: NULL)

#define TREE_BFS_FOREACH_SELECTIVE(var, root, scratch, field, sel) \
	for ((scratch)->head = (scratch)->tail = (scratch)->queue, \
		(var) = (root); (sel = true, var); \
		(var) = ((sel) && (var)->field.tn_first) \
		? (var)->field.tn_younger \
			? /* enqueue, the younger */ \
			  (__TREE_BFS_ENQUEUE(scratch, (var)->field.tn_first), \
			   (var)->field.tn_younger) \
			: (scratch)->head != (scratch)->tail \
				? /* enqueue, dequeue */ \
				  (__TREE_BFS_ENQUEUE(scratch, (var)->field.tn_first), \
				   __TREE_BFS_DEQUEUE(scratch)) \
				: /* the first */ \
				  (var)->field.tn_first \
		: (var)->field.tn_younger \
			? /* the younger */ \
			  (var)->field.tn_younger \
			: (scratch)->head != (scratch)->tail \
				? /* dequeue */ \
				  __TREE_BFS_DEQUEUE(scratch) \
				: NULL)

#define TREE_FIRSTBORN(ent, field) ((ent)->field.tn_first)
#define TREE_LASTBORN(ent, field)  ((ent)->field.tn_last)
#define TREE_YOUNGER(ent, field)   ((ent)->field.tn_younger)
#define TREE_OLDER(ent, field)     ((ent)->field.tn_older)
#define TREE_PARENT(ent, field)    ((ent)->field.tn_parent)

