CC      ?= gcc
CFLAGS  ?= -O3 -flto -march=haswell -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -flto -static -s

# CFLAGS for the runtime (musl, static)
# In Alpine, the system gcc is already musl-based. Override with musl-gcc
# only when building against a separate musl toolchain.
RUNTIME_CC      ?= $(CC)
RUNTIME_CFLAGS  ?= -O3 -flto -march=haswell -Wall -Wextra -Wno-unused-parameter
RUNTIME_LDFLAGS ?= -static -s

OBJS = parse.o server.o

all: fraud-api fraud-api-static

fraud-api: $(OBJS) tree.h
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

fraud-api-static: $(OBJS) tree.h
	$(RUNTIME_CC) $(RUNTIME_CFLAGS) -o $@ $(OBJS) $(RUNTIME_LDFLAGS)

parse.o: parse.c parse.h tree.h
	$(CC) $(CFLAGS) -c parse.c

server.o: server.c parse.h tree.h
	$(CC) $(CFLAGS) -c server.c

clean:
	rm -f fraud-api fraud-api-static *.o

test: fraud-api
	./test_runner

.PHONY: all clean test
