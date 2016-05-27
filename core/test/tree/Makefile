
PROG=tree

.PRECIOUS: $(PROG)

SRC=../../src
PS_DIR=../../../../io_engine/io_engine-2.0.38.2

all: clean $(PROG)

$(PROG): test.c
	gcc -g -Wall $^ -DTEST -I$(SRC)/include -I$(PS_DIR)/include -o $@
	./$(PROG)

clean:
	rm -f $(PROG)
	rm -f core
