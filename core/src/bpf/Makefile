CC=gcc
INC=../include/bpf/
CFLAGS=-g -O3 -I$(INC) -DTEST_BPF
LDFLAGS=
TARGET=main

all: sf_bpf_filter.c sfbpf-int.c sf_gencode.c sf_grammar.c \
	sf_nametoaddr.c sf_optimize.c sf_scanner.c main.c
	$(CC) $(CFLAGS) $(LDFLAGS) $? -o $(TARGET)
clean:
	rm -rf *.o $(TARGET) *~

