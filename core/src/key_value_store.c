#include "debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/queue.h>
#include "key_value_store.h"
#include "scalable_event.h"

/*----------------------------------------------------------------------------*/
kvs_t * 
kvs_create(int num_buckets, int num_entries)
{
	int i;
	kvs_t *ht = calloc(1, sizeof(kvs_t));
	if (!ht)
		return NULL;

	ht->num_buckets = num_buckets;
	ht->num_entries = num_entries;

	/* init the tables */
	if (!(ht->kvs_table = calloc(num_buckets, sizeof(kvs_bucket_head)))) {
		free(ht);
		return NULL;
	}
	for (i = 0; i < num_buckets; i++)
		TAILQ_INIT(&ht->kvs_table[i]);

	if (!(ht->kvs_cont = calloc(num_entries, sizeof(struct kvs_entry)))) {
		free(ht->kvs_table);
		free(ht);
		return NULL;
	}
	TAILQ_INIT(&ht->kvs_free);
	for (i = 0; i < num_entries; i++)
		TAILQ_INSERT_TAIL(&ht->kvs_free, &ht->kvs_cont[i], link);

	return ht;
}
/*----------------------------------------------------------------------------*/
void
kvs_destroy(kvs_t *ht)
{
	free(ht->kvs_table);
	free(ht->kvs_cont);
	free(ht);	
}
/*----------------------------------------------------------------------------*/
int 
kvs_insert(kvs_t *ht, _key_t const key, void * const value)
{
#ifdef DBGMSG
	__PREPARE_DBGLOGGING();
#endif
	/* create an entry*/ 
	int idx;

	assert(value);

	assert(ht);
	assert(ht->kvs_count <= ht->num_entries);

	if (kvs_search(ht, key))
		return -1;

	idx = key % ht->num_buckets;
	assert(idx >=0 && idx < ht->num_buckets);

	/* get a container */
	struct kvs_entry *ent;
	if (!(ent = TAILQ_FIRST(&ht->kvs_free)))
		return -1;
	TAILQ_REMOVE(&ht->kvs_free, ent, link);

	/* put the value to the container */
	ent->key = key;
	ent->value = value;

	/* insert the container */
	TAILQ_INSERT_TAIL(&ht->kvs_table[idx], ent, link);
	ht->kvs_count++;

	TRACE_DBG("%lX inserted to 0x%p\n", key, ht);

	return 0;
}
/*----------------------------------------------------------------------------*/
void * 
kvs_remove(kvs_t *ht, _key_t const key)
{
#ifdef DBGMSG
	__PREPARE_DBGLOGGING();
#endif
	struct kvs_entry *walk;
	kvs_bucket_head *head;

	head = &ht->kvs_table[key % ht->num_buckets];
	TAILQ_FOREACH(walk, head, link) {
		if (key == walk->key) {
			TAILQ_REMOVE(head, walk, link);
			TAILQ_INSERT_TAIL(&ht->kvs_free, walk, link);
			ht->kvs_count--;

			TRACE_DBG("%lX removed from 0x%p\n", key, ht);
			return walk->value;
		}
	}

	return NULL;
}	
/*----------------------------------------------------------------------------*/
void *
kvs_search(kvs_t *ht, _key_t const key)
{
#ifdef DBGMSG
	__PREPARE_DBGLOGGING();
#endif
	TRACE_DBG("look for %lX from 0x%p..\n", key, ht);

	struct kvs_entry *walk;
	kvs_bucket_head *head;

	head = &ht->kvs_table[key % ht->num_buckets];
	TAILQ_FOREACH(walk, head, link) {
		if (key == walk->key)
			return walk->value;
	}

	return NULL;
}
/*----------------------------------------------------------------------------*/
