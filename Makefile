CFLAGS  := -O3 -Wall -Werror -Wno-unused-result
LD      := gcc
LDFLAGS := ${LDFLAGS} -libverbs -lrt -lpthread -lmemcached

APPS    := main

all: ${APPS}

main: hrd.o main.o
	${LD} -o $@ $^ ${LDFLAGS}

PHONY: clean
clean:
	rm -f *.o ${APPS}
