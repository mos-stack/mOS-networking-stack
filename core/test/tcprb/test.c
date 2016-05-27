#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "tcp_rb.h"

#define FREE(x) do { free(x); x = NULL; } while (0)
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define PRINTF(f, args...) printf("%s:%d: " f, __FILE__, __LINE__, ##args)

#define CALL(arg) printf("%s returned %d\n", #arg, arg)

int main(int argc, char **argv)
{
	uint8_t wbuf[1024] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'};
	uint8_t rbuf[1024];

	tcprb_t *rb = tcprb_new(10);

	CALL(tcprb_pwrite(rb, wbuf, 2, 0));
	CALL(tcprb_ppeek(rb, rbuf, 2, 0));
	PRINTF("Read result: %c%c\n", rbuf[0], rbuf[1]);
	tcprb_printrb(rb);

	CALL(tcprb_pwrite(rb, wbuf, 2, 3));
	tcprb_printrb(rb);

	CALL(tcprb_pwrite(rb, wbuf, 2, 2));
	tcprb_printrb(rb);

	CALL(tcprb_pwrite(rb, wbuf, 2, 9));
	tcprb_printrb(rb);

	CALL(tcprb_setpile(rb, 2));
	tcprb_printrb(rb);

	CALL(tcprb_pwrite(rb, wbuf, 2, 11));
	tcprb_printrb(rb);

	CALL(tcprb_pwrite(rb, wbuf, 3, 6));
	tcprb_printrb(rb);

	CALL(tcprb_pwrite(rb, wbuf, 8, 4));
	tcprb_printrb(rb);

	CALL(tcprb_ppeek(rb, rbuf, 10, 2));
	PRINTF("Read result: %c%c%c%c%c%c%c%c%c%c\n",
			rbuf[0], rbuf[1], rbuf[2], rbuf[3], rbuf[4],
			rbuf[5], rbuf[6], rbuf[7], rbuf[8], rbuf[9]);

	CALL(tcprb_setpile(rb, 12));
	CALL(tcprb_pwrite(rb, wbuf, 1, 21));
	tcprb_printrb(rb);

	return 0;
}
