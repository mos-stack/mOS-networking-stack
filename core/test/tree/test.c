#include <stdio.h>
#include <stdlib.h>
#include "tree.h"

struct tree {
	int id;
	TREE_NODE(tree) link;
};

int
main (int argc, char **argv)
{
	struct tree nodes[10];
	TREE_SCRATCH(tree, 10) scratch;
	int i;

	struct tree *walk;

	for (i = 0; i < 10; i++) {
		nodes[i].id = i;
		TREE_INIT_NODE(&nodes[i], link);
	}

	TREE_INSERT_CHILD(&nodes[0], &nodes[1], link);
	TREE_INSERT_CHILD(&nodes[1], &nodes[2], link);
	TREE_INSERT_CHILD(&nodes[1], &nodes[3], link);
	TREE_INSERT_CHILD(&nodes[2], &nodes[4], link);
	TREE_INSERT_CHILD(&nodes[3], &nodes[5], link);

	TREE_DFS_FOREACH(walk, &nodes[0], &scratch, link)
		printf("DFS: %d\n", walk->id);

	TREE_BFS_FOREACH(walk, &nodes[0], &scratch, link)
		printf("BFS: %d\n", walk->id);

	return 0;
}
