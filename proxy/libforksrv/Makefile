.PHONY: all clean

CFLAGS = -fPIC -Wall -Wextra
LDFLAGS = -shared -ldl

# libforksrv.so is LD_PRELOADed into the target and starts the forkserver from
# a constructor. libforksrv.a is the deferred (CS_DEFER_FORKSRV) variant: built
# with -DSTATIC so it has no constructor, it is linked into a target that calls
# __cs_start_forkserver() itself at a point of its choosing.
all: libforksrv.so libforksrv.a

libforksrv.so: libforksrv.c
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

libforksrv.a: libforksrv.o
	ar rcs $@ $^

libforksrv.o: libforksrv.c
	$(CC) $(CFLAGS) -DSTATIC -c $^ -o $@

clean:
	rm -f libforksrv.so libforksrv.a libforksrv.o
