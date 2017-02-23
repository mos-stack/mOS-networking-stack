#pragma once

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>
#include <memory_mgt.h>
#include "mos_api.h"

/* Abstract ring buffer as an infinite but windowed buffer */
/* Keep in mind about inc/dec buffer */
/* FYI, I hate current tcp ring buffer implementation with memmove() */

#define UNITBUFSIZE 1024
#if UNITBUFSIZE < 2
#error UNITBUFSIZE cannot be smaller than 2
#endif

#define BUFMGMT_FULL    2
#define BUFMGMT_FRAGS   1
#define BUFMGMT_OFF     0

#define DISABLE_DYN_RESIZE

#if 0
typedef enum {
	MOS_OVERLAP_POLICY_FIRST=0,
	MOS_OVERLAP_POLICY_LAST,
	MOS_OVERLAP_CNT
} MOS_OVERLAP_POLICY;
#endif

enum tcprb_mode {
	__RB_NO_FRAG = 1,
	__RB_NO_BUF  = 2,
};

typedef int      boff_t; /* buffer offset space */
typedef int64_t  loff_t; /* logical offset space */

typedef struct _tcpbufseg_t {
	uint8_t buf[UNITBUFSIZE];

	int id;
	TAILQ_ENTRY(_tcpbufseg_t) link;
} tcpbufseg_t;

typedef struct _tcpfrag_t {
	bool empty;  /* true if this fragment does not have actual data */

	loff_t head; /* head of this fragment */
	loff_t tail; /* tail of this fragment */

	TAILQ_ENTRY(_tcpfrag_t) link;
} tcpfrag_t;

typedef struct _tcprb_t {
	mem_pool_t mp;

	unsigned mode:4,
		buf_mgmt:2,
		overlap:2;
	int corr;

	int metalen;

	TAILQ_HEAD(blist, _tcpbufseg_t) bufsegs;
	int lbufsegs;
	int len;

	loff_t head; /* head of this window (inf space) */
	loff_t pile; /* maximum head. tcprb_ffhead() cannot move hseq further
					than cseq. (sequence space) */

	TAILQ_HEAD(flist, _tcpfrag_t) frags;

	TAILQ_ENTRY(_tcprb_t) link;
} tcprb_t;

extern inline tcprb_t *
tcprb_new(mem_pool_t mp, int len, unsigned buf_mgmt);

extern inline int
tcprb_del(tcprb_t *rb);

extern inline int
tcprb_setpile(tcprb_t *rb, loff_t new);

extern inline int
tcprb_cflen(tcprb_t *rb);

extern inline int
tcprb_resize_meta(tcprb_t *rb, int len);

extern inline int
tcprb_resize(tcprb_t *rb, int len);

extern inline int
tcprb_ffhead(tcprb_t *rb, int len);

extern inline int
tcprb_ppeek(tcprb_t *rb, uint8_t *buf, int len, loff_t off);

extern inline int
tcprb_pwrite(tcprb_t *rb, uint8_t *buf, int len, loff_t off);

extern inline void
tcprb_printfrags(struct _tcprb_t *rb);

extern inline void
tcprb_printrb(struct _tcprb_t *rb);

extern inline loff_t
seq2loff(tcprb_t *rb, uint32_t seq, uint32_t isn);

extern inline void
tcp_rb_overlapchk(mtcp_manager_t mtcp, struct pkt_ctx *pctx,
		  struct tcp_stream *recvside_stream);

extern inline int
tcprb_setpolicy(tcprb_t *rb, uint8_t policy);
