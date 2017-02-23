#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <sys/uio.h>
#include <ctype.h>
#include "debug.h"
#include "tcp_rb.h"

#define FREE(x) do { free(x); x = NULL; } while (0)
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#endif

#define PRINTF(f, args...) printf("%s:%d: " f, __FILE__, __LINE__, ##args)

/* -------------------------------------------------------------------------- */
/* Private */
/* -------------------------------------------------------------------------- */

static inline boff_t
loff2boff(tcprb_t *rb, loff_t loff)
{
	int woff = loff - rb->head; /* window offset */
	if (woff < 0 || woff > rb->len)
		return -1;

	return (boff_t)((loff + rb->corr) % rb->len);
}
/*--------------------------------------------------------------------------*/
static inline tcpfrag_t *
frags_new(void)
{
	return (tcpfrag_t *)calloc(sizeof(tcpfrag_t), 1);
}
/*--------------------------------------------------------------------------*/
static inline void
frags_del(tcpfrag_t *f)
{
	free(f);
}
/*--------------------------------------------------------------------------*/
static inline void
frags_insert(tcprb_t *rb, tcpfrag_t *f)
{
	struct _tcpfrag_t *walk;

	TAILQ_FOREACH(walk, &rb->frags, link)
		if (walk->head > f->head) {
			TAILQ_INSERT_BEFORE(walk, f, link);
			break;
		}

	if (!walk)
		TAILQ_INSERT_TAIL(&rb->frags, f, link);
}
/*--------------------------------------------------------------------------*/
static inline tcpbufseg_t *
bufseg_new(mem_pool_t mp)
{
	//return (tcpbufseg_t *)malloc(sizeof(tcpbufseg_t));
	return (tcpbufseg_t *)MPAllocateChunk(mp);
}
/*--------------------------------------------------------------------------*/
static inline void
bufseg_del(mem_pool_t mp, tcpbufseg_t *b)
{
	//free(b);
	MPFreeChunk(mp, b);
}
/*--------------------------------------------------------------------------*/
/* For on-demand buffser segment allocation, the buffer segment queue must
 * be always traversed in directin of from head to tail */
static inline tcpbufseg_t *
buf_first(tcprb_t *rb)
{
	tcpbufseg_t *tmp;

	if ((tmp = TAILQ_FIRST(&rb->bufsegs)))
		return tmp;

	if ((rb->lbufsegs == 0) || ((tmp = bufseg_new(rb->mp)) == NULL))
		return NULL;

	tmp->id = 0;
	TAILQ_INSERT_TAIL(&rb->bufsegs, tmp, link);

	return tmp;
}
/*--------------------------------------------------------------------------*/
static inline tcpbufseg_t *
buf_next(tcprb_t *rb, tcpbufseg_t *buf)
{
	tcpbufseg_t *tmp;

	if ((tmp = TAILQ_NEXT(buf, link)))
		return tmp;

	if ((rb->lbufsegs <= buf->id + 1) || ((tmp = bufseg_new(rb->mp)) == NULL))
		return NULL;

 	tmp->id = buf->id + 1;
	TAILQ_INSERT_TAIL(&rb->bufsegs, tmp, link);

	return tmp;
}
/*--------------------------------------------------------------------------*/
static inline tcpbufseg_t *
buf_getbuf(tcprb_t *rb, boff_t off)
{
	tcpbufseg_t *buf;
	int id = off / UNITBUFSIZE;
	assert(id >= 0);

#if 0
	int max = rb->len / UNITBUFSIZE;

	if (max / 2 > id) {
		buf = TAILQ_FIRST(&rb->bufsegs);
		while (id--)
			buf = TAILQ_NEXT(buf, link);
	} else {
		buf = TAILQ_LAST(&rb->bufsegs, blist);
		while (max - ++id)
			buf = TAILQ_PREV(buf, blist, link);
	}
#else
	buf = buf_first(rb);
	while (id--)
		buf = buf_next(rb, buf);
#endif

	assert(buf);

	return buf;
}
/*--------------------------------------------------------------------------*/
#define TAILQ_LOOP_NEXT(head, headname, elm, field) \
	((*(((struct headname *)((head)->tqh_last))->tqh_last)) == (elm) ? \
	 ((head)->tqh_first) : ((elm)->field.tqe_next))

static inline int
buf_try_resize(tcprb_t *rb, int len, loff_t data, int datalen)
{
	/* FIXME: resizing is temporally disabled because of on-demand buffer
	 * allocation patch */
	//return 0;

	assert(rb);
	assert(rb->len);
	assert(len % UNITBUFSIZE == 0 || len < 2);

	int segdiff = (len - rb->len) / UNITBUFSIZE;
	if (segdiff == 0)
		return 0;

	boff_t head = loff2boff(rb, data);
	boff_t tail = (data + datalen - 1) % rb->len;

	tcpbufseg_t *headseg = buf_getbuf(rb, head);
	tcpbufseg_t *tailseg = buf_getbuf(rb, tail);

	if (segdiff > 0) {
		/* Expand the buffer */
		rb->len = len;
		boff_t new_head = loff2boff(rb, data);
		rb->corr = (rb->corr + (segdiff * UNITBUFSIZE) - (new_head - head))
				   % rb->len;
		if (rb->corr < 0)
			rb->corr += rb->len;

		if (head > tail && headseg == tailseg) {
			tcpbufseg_t *seg = bufseg_new(rb->mp);
			assert(seg);
			memcpy(&seg->buf[head % UNITBUFSIZE],
				   &headseg->buf[head % UNITBUFSIZE],
				   UNITBUFSIZE - (head % UNITBUFSIZE));
			TAILQ_INSERT_AFTER(&rb->bufsegs, tailseg, seg, link);
			headseg = seg;
			segdiff--;
		}
		while (segdiff--) {
			tcpbufseg_t *seg = bufseg_new(rb->mp);
			assert(seg);
			TAILQ_INSERT_AFTER(&rb->bufsegs, tailseg, seg, link);
		}
	} else /* if (segdiff < 0) */ {
		/* Shrink the buffer */
		tcpbufseg_t *seg;

		segdiff *= -1;
		int shrinkable = (rb->len - datalen) / UNITBUFSIZE;
		int tobeshrank = segdiff;

		rb->len -= (tobeshrank * UNITBUFSIZE);
		if (rb->len) {
			boff_t new_head = loff2boff(rb, data);
			rb->corr = (rb->corr - (tobeshrank * UNITBUFSIZE) -
						(new_head - head)) % rb->len;
			if (rb->corr < 0)
				rb->corr += rb->len;
		}

		if (shrinkable < segdiff) {
			/* Mark some fragments as empty */
			loff_t eh = data + rb->len +
				(UNITBUFSIZE - (loff2boff(rb, data) % UNITBUFSIZE));
			//loff_t et = eh + (segdiff - shrinkable) * UNITBUFSIZE;
			loff_t et = data + datalen;

			struct _tcpfrag_t *f = TAILQ_FIRST(&rb->frags), *new;

			TAILQ_FOREACH(f, &rb->frags, link) {
				if (f->tail <= eh)
					continue;

				if (f->tail > et) {
					new = frags_new();
					new->head = et;
					new->tail = f->tail;
					TAILQ_INSERT_AFTER(&rb->frags, f, new, link);

					f->tail = et;
				}

				if (f->head >= eh && f->tail <= et) {
					f->empty = true;
					continue;
				}
				
				if (f->head < eh) {
					new = frags_new();
					new->head = eh;
					new->tail = f->tail;
					TAILQ_INSERT_AFTER(&rb->frags, f, new, link);

					f->tail = eh;
					continue;
				}
			}

			/* shrink the buffer */
			int skip = rb->len / UNITBUFSIZE;
			for (seg = headseg; seg; ) {
				if (skip-- <= 0) {
					TAILQ_REMOVE(&rb->bufsegs, seg, link);
					bufseg_del(rb->mp, seg);
				}
				seg = TAILQ_LOOP_NEXT(&rb->bufsegs, blist, tailseg, link);
				if (seg == headseg)
					break;
			}
		} else {
			while (tobeshrank &&
					(seg = TAILQ_LOOP_NEXT(&rb->bufsegs, blist, tailseg, link))
					!= headseg) {
				TAILQ_REMOVE(&rb->bufsegs, seg, link);
				bufseg_del(rb->mp, seg);
				tobeshrank--;
			}
			if (tobeshrank) {
				assert(tobeshrank == 1);
				assert((tail % UNITBUFSIZE) < (head % UNITBUFSIZE));
				memcpy(&tailseg->buf[head % UNITBUFSIZE],
						&headseg->buf[head % UNITBUFSIZE],
						UNITBUFSIZE - (head % UNITBUFSIZE));
				TAILQ_REMOVE(&rb->bufsegs, headseg, link);
				bufseg_del(rb->mp, headseg);
				headseg = tailseg;
				tobeshrank = 0;
			}
		}
	}

	return 0;
}
/*--------------------------------------------------------------------------*/
/* buf_read() and buf_write() are highly symmetic, so use macro for function
 * definition to ease code maintenance. */

/* TODO: We do not have tailing anymore. simplify these functions */

#define MEMCPY_FOR_read(a, b, len) memcpy(a, b, len)
#define MEMCPY_FOR_write(a, b, len) memcpy(b, a, len)

#define FUNCDEF_BUF_RW(rw) \
static inline void \
buf_##rw(tcprb_t *rb, uint8_t *buf, int len, loff_t off) \
{ \
	tcpbufseg_t *bufseg = NULL; \
 \
	assert(rb); \
 \
	boff_t from = loff2boff(rb, off); \
	boff_t to = loff2boff(rb, off + len); \
	tcpbufseg_t *bufseg_from = buf_getbuf(rb, from); \
	tcpbufseg_t *bufseg_to = buf_getbuf(rb, to); \
 \
	if (from > to) { \
		off = UNITBUFSIZE - (from % UNITBUFSIZE); \
		MEMCPY_FOR_##rw(&buf[0], &bufseg_from->buf[from % UNITBUFSIZE], off); \
		for (bufseg = buf_next(rb, bufseg_from); \
			 bufseg && (bufseg != bufseg_to); \
			 bufseg = buf_next(rb, bufseg)) { \
			MEMCPY_FOR_##rw(&buf[off], &bufseg->buf[0], UNITBUFSIZE); \
			off += UNITBUFSIZE; \
		} \
		for (bufseg = buf_first(rb); \
			 bufseg && (bufseg != bufseg_to); \
			 bufseg = buf_next(rb, bufseg)) { \
			MEMCPY_FOR_##rw(&buf[off], &bufseg->buf[0], UNITBUFSIZE); \
			off += UNITBUFSIZE; \
		} \
		MEMCPY_FOR_##rw(&buf[off], &bufseg_to->buf[0], to % UNITBUFSIZE); \
	} else if (bufseg_from == bufseg_to) { \
		MEMCPY_FOR_##rw(&buf[0], &bufseg_from->buf[from % UNITBUFSIZE], len); \
	} else { \
		off = UNITBUFSIZE - (from % UNITBUFSIZE); \
		MEMCPY_FOR_##rw(&buf[0], &bufseg_from->buf[from % UNITBUFSIZE], off); \
		for (bufseg = buf_next(rb, bufseg_from); \
			 bufseg && (bufseg != bufseg_to); \
			 bufseg = buf_next(rb, bufseg)) { \
			MEMCPY_FOR_##rw(&buf[off], &bufseg->buf[0], UNITBUFSIZE); \
			off += UNITBUFSIZE; \
		} \
		MEMCPY_FOR_##rw(&buf[off], &bufseg_to->buf[0], to % UNITBUFSIZE); \
	} \
}

FUNCDEF_BUF_RW(read)
FUNCDEF_BUF_RW(write)
/* -------------------------------------------------------------------------- */
/* Public */
/* -------------------------------------------------------------------------- */

inline loff_t
seq2loff(tcprb_t *rb, uint32_t seq, uint32_t isn)
{
	loff_t off = seq - isn;

	while (off < rb->head)
		off += 0x100000000;

	return off;
}
/*--------------------------------------------------------------------------*/
inline tcprb_t *
tcprb_new(mem_pool_t mp, int len, unsigned buf_mgmt)
{
	tcprb_t *rb;

	if (len % UNITBUFSIZE || len < 2)
		return NULL;

	if (!(rb = calloc(sizeof(tcprb_t), 1)))
		return NULL;

	TAILQ_INIT(&rb->bufsegs);
	rb->lbufsegs = ((len - 1) / UNITBUFSIZE) + 1;

#if 0
	int i;
	for (i = 0; i < rb->lbufsegs; i++) {
		tcpbufseg_t *bufseg = bufseg_new(mp);
		if (!bufseg) {
			TAILQ_FOREACH(bufseg, &rb->bufsegs, link)
				bufseg_del(mp, bufseg);
			FREE(rb);
			return NULL;
		}
		TAILQ_INSERT_TAIL(&rb->bufsegs, bufseg, link);
	}
#endif

	rb->buf_mgmt = buf_mgmt;
	rb->mp = mp;
	rb->len = rb->metalen = len;
	TAILQ_INIT(&rb->frags);

	return rb;
}
/*--------------------------------------------------------------------------*/
inline int
tcprb_del(tcprb_t *rb)
{
	struct _tcpbufseg_t *bwalk, *bnext;
	struct _tcpfrag_t *fwalk, *fnext;

	for (bwalk = TAILQ_FIRST(&rb->bufsegs); bwalk; bwalk = bnext) {
		bnext = TAILQ_NEXT(bwalk, link);
		bufseg_del(rb->mp, bwalk);
	}

	for (fwalk = TAILQ_FIRST(&rb->frags); fwalk; fwalk = fnext) {
		fnext = TAILQ_NEXT(fwalk, link);
		frags_del(fwalk);
	}

	FREE(rb);

	return 0;
}
/*--------------------------------------------------------------------------*/
inline int
tcprb_setpile(tcprb_t *rb, loff_t new)
{
	if (!rb || new > (rb->head + rb->len) || new < rb->head)
		return -1;

	tcpfrag_t *cf = TAILQ_FIRST(&rb->frags); /* contiguous buffer seg. */

	if (!cf || (cf->head != rb->head)) {
		/* No contiguous buffer seg. */
		assert(rb->pile == rb->head);
		return -1;
	}

	if (new > cf->tail)
		return -1;

	rb->pile = new;

	return 0;
}
/*--------------------------------------------------------------------------*/
inline int
tcprb_cflen(tcprb_t *rb)
{
	tcpfrag_t *cf = TAILQ_FIRST(&rb->frags); /* contiguous buffer seg. */

	if (!cf || (cf->head != rb->head))
		/* No contiguous buffer seg. to taverse */
		return 0;

	int cflen = cf->tail - rb->pile; /* length of cf */

	assert(cflen >= 0);

	return cflen;
}
/*--------------------------------------------------------------------------*/
inline int
tcprb_ffhead(tcprb_t *rb, int len)
{
	if (!rb || len < 0)
		return 0;
	else if (len == 0)
		return 0;

	tcpfrag_t *cf = TAILQ_FIRST(&rb->frags); /* contiguous buffer seg. */

	if (!cf || (cf->head != rb->head))
		/* No contiguous buffer seg. to taverse */
		return 0;

	int cflen = cf->tail - cf->head; /* length of cf */
	assert(cflen > 0);

	int ff = MIN(len, cflen);
	ff = MIN(ff, rb->pile - rb->head); /* head cannot go further than pile */

	if (cflen == ff) {
		/* Fast forward entire contiguous segment */
		TAILQ_REMOVE(&rb->frags, cf, link);
		frags_del(cf);
	} else {
		cf->head += ff;
	}

	rb->head += ff;

	return ff;
}
/*--------------------------------------------------------------------------*/
static inline int
tcprb_get_datalen(tcprb_t *rb)
{
	tcpfrag_t *lastfrag = TAILQ_LAST(&rb->frags, flist);
	return lastfrag ? (int)(lastfrag->tail - rb->head) : 0;
}
/*--------------------------------------------------------------------------*/
inline int
tcprb_resize_meta(tcprb_t *rb, int len)
{
#ifdef DISABLE_DYN_RESIZE
	assert(len == 0);

	struct _tcpfrag_t *fwalk, *fnext;

	rb->metalen = 0;

	for (fwalk = TAILQ_FIRST(&rb->frags); fwalk; fwalk = fnext) {
		fnext = TAILQ_NEXT(fwalk, link);
		TAILQ_REMOVE(&rb->frags, fwalk, link);
		frags_del(fwalk);
	}

	return 0;
#else
	int diff, ff, datalen;

	if ((diff = len - rb->metalen) > 0) {
		rb->metalen = len;
	} else if (diff < 0) {
		ff = diff - (rb->len - tcprb_get_datalen(rb));
		tcprb_ffhead(rb, ff);
		datalen = tcprb_get_datalen(rb);
		rb->metalen = MAX(datalen, len);
	}

	return rb->metalen;
#endif
}
/*--------------------------------------------------------------------------*/
inline int
tcprb_setpolicy(tcprb_t *rb, uint8_t policy)
{
	if (policy >= MOS_OVERLAP_CNT)
		return -1;
	
	rb->overlap = policy;
	return 0;
}
/*--------------------------------------------------------------------------*/
inline int
tcprb_resize(tcprb_t *rb, int len)
{
#ifdef DISABLE_DYN_RESIZE
	assert(len == 0);

	struct _tcpbufseg_t *bwalk, *bnext;
	struct _tcpfrag_t *fwalk, *fnext;

	rb->metalen = 0;
	rb->len = 0;

	for (bwalk = TAILQ_FIRST(&rb->bufsegs); bwalk; bwalk = bnext) {
		bnext = TAILQ_NEXT(bwalk, link);
		TAILQ_REMOVE(&rb->bufsegs, bwalk, link);
		bufseg_del(rb->mp, bwalk);
	}

	for (fwalk = TAILQ_FIRST(&rb->frags); fwalk; fwalk = fnext) {
		fnext = TAILQ_NEXT(fwalk, link);
		TAILQ_REMOVE(&rb->frags, fwalk, link);
		frags_del(fwalk);
	}

	return 0;
#else
	int diff, ff;

	if (len % UNITBUFSIZE)
		return -1;
	else if (len == rb->len)
		return 0;

	if ((diff = rb->len - len) > 0 && /* shrinking */
		(ff = diff - (rb->len - tcprb_get_datalen(rb))))
		tcprb_ffhead(rb, ff);

	return buf_try_resize(rb, len, rb->head,
						  tcprb_get_datalen(rb));
#endif
}
/*--------------------------------------------------------------------------*/
inline int
tcprb_ppeek(tcprb_t *rb, uint8_t *buf, int len, loff_t off)
{
	struct _tcpfrag_t *f;

	if (!rb || rb->buf_mgmt != BUFMGMT_FULL || !buf || len < 0)
		return -1;
	else if (len == 0)
		return 0;

	TAILQ_FOREACH(f, &rb->frags, link)
		if (off >= f->head && off < f->tail) {
			if (f->empty)
				f = NULL;
			break;
		}

	if (!f) /* No proper fragment found */
		return -1;

	int plen = MIN(len, f->tail - off);

	buf_read(rb, buf, plen, off);

	return plen;
}
/*--------------------------------------------------------------------------*/
inline int
tcprb_pwrite(tcprb_t *rb, uint8_t *buf, int len, loff_t off)
{
	int ff, olen;
	loff_t efhead = -1;  /* head of empty fragment */
	int eflen = 0;           /* length of empty fragment */

	/* pwrite() will not overwrite old data with new data.
	 * However, */

	if (!rb || !buf || len < 0 ||
		/* out of window range */
		off < rb->head || off >= rb->pile + rb->metalen)
		return -1;
	else if (len == 0)
		return 0;
	else if (off + len < rb->pile) /* already written */
		return len;

	/* move head */
	olen = len;
	if ((ff = (off + len) - (rb->head + MIN(rb->len, rb->metalen))) > 0)
		len -= ff - tcprb_ffhead(rb, ff);

	if (rb->metalen > rb->len)
		eflen = MIN(olen - len, rb->metalen - rb->len);
	if (eflen)
		efhead = off + len;

	/* write data */
	struct _tcpfrag_t *f = TAILQ_FIRST(&rb->frags), *fnext;
	int uoff = 0; /* offset of `buf` */

	/* head: position of first byte of the fragment
	 * tail: head + length of the fragment */
	while (uoff < len) {
		bool skip = false;
		int wrlen = 0; /* length to be written */

		while (f) {
			struct _tcpfrag_t *ef, *nef;
			fnext = TAILQ_NEXT(f, link);

			if (f->empty) {
				/* skip empty fragment */
				f = fnext;
				continue;
			}

			if (f->head <= off + uoff) {
				if (f->tail > off + uoff) {
					skip = true;
					wrlen = f->tail - (off + uoff);
					break;
				} else if (f->tail == off + uoff) {
					skip = false;

					/* shrink empty fragment */
					for (ef = fnext;
						 ef && ef->empty && ef->head < f->tail + len - uoff;
						 ef = nef) {
						nef = TAILQ_NEXT(ef, link);

						if (ef->tail <= f->tail + len - uoff) {
							TAILQ_REMOVE(&rb->frags, ef, link);
						} else {
							ef->head = f->tail + len - uoff;
							/* break is not necessary, but for early escape */
							break;
						}
					}
					fnext = TAILQ_NEXT(f, link);

					wrlen = fnext ? MIN(fnext->head - (off + uoff), len - uoff)
								  : len - uoff;
					f->tail += wrlen;
					if (fnext && (f->tail == fnext->head)) {
						/* merge 'f' and 'fnext' */
						f->tail = fnext->tail;
						TAILQ_REMOVE(&rb->frags, fnext, link);
						frags_del(fnext);
					}
					break;
				}
			} else if (f->head <= off + len) {
				skip = false;
				wrlen = MIN(f->head - (off + uoff), len - uoff);
				f->head -= wrlen;

				/* shrink empty fragment */
				for (ef = TAILQ_PREV(f, flist, link);
					 ef && ef->empty && ef->tail < f->head;
					 ef = nef) {
					nef = TAILQ_PREV(ef, flist, link);

					if (ef->head <= f->head) {
						TAILQ_REMOVE(&rb->frags, ef, link);
					} else {
						ef->tail = f->head;
						/* break is not necessary, but for early escape */
						break;
					}
				}

				break;
			} else /* if (f->head > off + len) */ {
				/* early escape */
				f = NULL;
				break;
			}

			f = fnext;
		}

		if (!f) {
			struct _tcpfrag_t *new;

			/* create new fragment and insert it into the list */
			new = frags_new();

			new->head = off;
			new->tail = off + len;
			wrlen = len;

			frags_insert(rb, new);
		}

		/* copy data */
		if ((rb->overlap == MOS_OVERLAP_POLICY_LAST || !skip)
			&& rb->buf_mgmt)
			buf_write(rb, &buf[uoff], wrlen, off + uoff);
		uoff += wrlen;
	}

	/* Insert empty fragment if necessary */
	if (eflen) {
		assert(rb->metalen > rb->len);

		struct _tcpfrag_t *new;

		/* create new fragment and insert it into the list */
		new = frags_new();

		new->empty = true;
		new->head = efhead;
		new->tail = efhead + eflen;

		frags_insert(rb, new);
	}

	return len;
}
/*--------------------------------------------------------------------------*/
#define PRT_CL_RST		"\x1b[0m"
#define PRT_FG_BLK		"\x1b[30m"
#define PRT_FG_RED		"\x1b[31m"
#define PRT_FG_GRN		"\x1b[32m"
#define PRT_FG_YEL		"\x1b[33m"
#define PRT_FG_BLU		"\x1b[34m"
#define PRT_FG_PUR		"\x1b[35m"
#define PRT_FG_CYN		"\x1b[36m"
#define PRT_FG_GRY		"\x1b[37m"
#define PRT_BG_BLK		"\x1b[40m"
#define PRT_BG_RED		"\x1b[41m"
#define PRT_BG_GRN		"\x1b[42m"
#define PRT_BG_YEL		"\x1b[43m"
#define PRT_BG_BLU		"\x1b[44m"
#define PRT_BG_PUR		"\x1b[45m"
#define PRT_BG_CYN		"\x1b[46m"
#define PRT_BG_GRY		"\x1b[47m"
/*--------------------------------------------------------------------------*/
inline void
tcprb_printfrags(struct _tcprb_t *rb)
{
	struct _tcpfrag_t *walk;

	printf("frags: ");
	TAILQ_FOREACH(walk, &rb->frags, link) {
		printf("[%lu - %lu]:'", walk->head, walk->tail - 1);
#if 1
		if (walk->empty)
			printf("EMPTY");
		else {
			loff_t i;
			for (i = walk->head; i < walk->tail; i++) {
				tcpbufseg_t *bufseg;
				boff_t j = loff2boff(rb, i);
				bufseg = buf_getbuf(rb, j);
				char c = bufseg->buf[j % UNITBUFSIZE];
				if (isgraph(c))
					printf("%c", c);
				else {
					printf(PRT_FG_BLU);
					if (c == ' ')
						printf(" ");
					else if (c == '\0')
						printf("0");
					else if (c == '\r')
						printf("R");
					else if (c == '\n')
						printf("N");
					else if (c == '\t')
						printf("T");
					else
						printf("?");
					printf(PRT_CL_RST);
				}
			}
		}
#endif
		printf("' ");
	}
	printf("\n");
}
inline void
tcprb_printbufsegs(tcprb_t *rb)
{
	struct _tcpbufseg_t *walk;

	printf("bufsegs: ");
	TAILQ_FOREACH(walk, &rb->bufsegs, link) {
		printf("[%d]:'", walk->id);
#if 1
		int j;
		for (j = 0; j < UNITBUFSIZE; j++) {
			char c = walk->buf[j];
			if (isgraph(c))
				printf("%c", c);
			else {
				printf(PRT_FG_BLU);
				if (c == ' ')
					printf(" ");
				else if (c == '\0')
					printf("0");
				else if (c == '\r')
					printf("R");
				else if (c == '\n')
					printf("N");
				else if (c == '\t')
					printf("T");
				else
					printf("?");
				printf(PRT_CL_RST);
			}
		}
#endif
		printf("' ");
	}
	printf("\n");
}
/*--------------------------------------------------------------------------*/
inline void
tcprb_printrb(struct _tcprb_t *rb)
{
	printf("  rb: len: %d, meta: %d, "
			"(head: %ld <= pile: %ld <= tail: %ld)\n  ",
			rb->len, rb->metalen, rb->head, rb->pile, rb->head + rb->metalen);
	tcprb_printfrags(rb);
	tcprb_printbufsegs(rb);
	printf("-------------------------------------------------\n");
}
/*--------------------------------------------------------------------------*/
inline void
tcp_rb_overlapchk(mtcp_manager_t mtcp, struct pkt_ctx *pctx,
		    struct tcp_stream *recvside_stream)
{
#define DOESOVERLAP(a1, a2, b1, b2)				\
	((a1 != b2) && (a2 != b1) && ((a1 > b2) != (a2 > b1)))
	
	/* Check whether this packet is retransmitted or not. */
	tcprb_t *rb;
	struct socket_map *walk;
	if (pctx->p.payloadlen > 0 && recvside_stream->rcvvar != NULL
	    && (rb = recvside_stream->rcvvar->rcvbuf) != NULL) {
		struct _tcpfrag_t *f;
		loff_t off = seq2loff(rb, pctx->p.seq, recvside_stream->rcvvar->irs + 1);
		TAILQ_FOREACH(f, &rb->frags, link)
			if (DOESOVERLAP(f->head, f->tail, off, off + pctx->p.payloadlen)) {
				/*
				 * call it immediately (before pkt payload is attached
				 * to the tcp ring buffer)
				 */
				SOCKQ_FOREACH_START(walk, &recvside_stream->msocks) {
					HandleCallback(mtcp, MOS_HK_RCV, walk,
						       recvside_stream->side,
						       pctx, MOS_ON_REXMIT);
				} SOCKQ_FOREACH_END;
				/* recvside_stream->cb_events |= MOS_ON_REXMIT; */
				TRACE_DBG("RETX!\n");
				break;
			}
	}
}
/*--------------------------------------------------------------------------*/
