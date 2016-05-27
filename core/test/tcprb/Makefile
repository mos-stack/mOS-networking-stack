
.PRECIOUS: test

SRC=../../src
PS_DIR=../../../../io_engine/io_engine-2.0.38.2

all: clean test

test: test.c $(SRC)/tcp_rb.c $(SRC)/include/tcp_rb.h \
	$(SRC)/memory_mgt.c
	gcc -g -Wall $^ -DTEST -I$(SRC)/include -I$(PS_DIR)/include -o $@
	./test

clean:
	rm -f test
	rm -f core
