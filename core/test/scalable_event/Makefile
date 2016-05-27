
PROG=scev

.PRECIOUS: $(PROG)

SRC_DIR=../../src
PS_DIR=../../../../io_engine/io_engine-2.0.38.2

CORE_SRC=$(addprefix $(SRC_DIR)/,scalable_event.c key_value_store.c fhash.c util.c)

all: clean $(PROG)

$(PROG): test.c $(CORE_SRC)
	gcc -gdwarf-2 -g3 -Wall $^ -DTEST -DNEWEV -I$(SRC_DIR)/include -I$(PS_DIR)/include -o $@
	./$(PROG) -p 0.5 -d 10

clean:
	rm -f $(PROG)
	rm -f core
