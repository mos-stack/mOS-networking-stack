#ifndef __FHASH_H_
#define __FHASH_H_

#include <sys/queue.h>
#include "tcp_stream.h"

#define NUM_BINS (131072)     /* 132 K entries per thread*/
#define TCP_AR_CNT (3)

#define STATIC_TABLE FALSE
#define INVALID_HASH (NUM_BINS + 1)

typedef struct hash_bucket_head {
	tcp_stream *tqh_first;
	tcp_stream **tqh_last;
} hash_bucket_head;

/* hashtable structure */
struct hashtable {
	uint8_t ht_count ;                    // count for # entry

#if STATIC_TABLE
	tcp_stream* ht_array[NUM_BINS][TCP_AR_CNT];
#endif
	hash_bucket_head ht_table[NUM_BINS];
};

/*functions for hashtable*/
struct hashtable *CreateHashtable(void);
void DestroyHashtable(struct hashtable *ht);


int HTInsert(struct hashtable *ht, tcp_stream *, unsigned int *hash);
void* HTRemove(struct hashtable *ht, tcp_stream *);
tcp_stream* HTSearch(struct hashtable *ht, const tcp_stream *, unsigned int *hash);

#endif /* __FHASH_H_ */
