
.PRECIOUS: evcb

SRC=../../src
PS_DIR=../../../../io_engine/io_engine-2.0.38.2

all: clean evcb

evcb: test.c $(SRC)/event_callback.c $(SRC)/include/event_callback.h \
	$(SRC)/memory_mgt.c
	gcc -g -Wall $^ -DTEST -I$(SRC)/include -I$(PS_DIR)/include -o $@
	./evcb

clean:
	rm -f evcb
	rm -f core
