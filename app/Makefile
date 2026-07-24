PROG1 = sds3sync
OBJS1 = sdsync.o sigv4.o s3put.o paramstore_axis.o
PROGS = $(PROG1)

PKGS = gio-2.0 glib-2.0 axparameter libcurl openssl
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64
LDLIBS += $(shell pkg-config --libs $(PKGS))

all: $(PROGS)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

# --- Host build ------------------------------------------------------
# Links the same sdsync.c/s3put.c/sigv4.c against the real host's glib/
# libcurl/openssl -- no axparameter (Axis doesn't ship it for x86_64;
# see paramstore_host.c for the substitute). For integration tests only;
# the on-camera .eap build above is completely separate and untouched by
# any of this. Distinct object suffixes (.host.o / .asan.o) so these
# never collide with the aarch64 .o files built in the same directory.
HOST_PROG = sds3sync-host
HOST_OBJS = sdsync.host.o sigv4.host.o s3put.host.o paramstore_host.host.o
HOST_PKGS = glib-2.0 libcurl openssl
HOST_CFLAGS = $(shell pkg-config --cflags $(HOST_PKGS)) -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64
HOST_LDLIBS = $(shell pkg-config --libs $(HOST_PKGS))

%.host.o: %.c
	$(CC) $(HOST_CFLAGS) -c $< -o $@

host: $(HOST_PROG)

$(HOST_PROG): $(HOST_OBJS)
	$(CC) $(HOST_CFLAGS) $(LDFLAGS) $^ $(HOST_LDLIBS) -o $@

# ASan/LeakSanitizer variant of the host build: short, deterministic runs
# to localize leaks precisely, complementing the statistical RSS-trend
# soak (see tests/integration/).
HOST_ASAN_PROG = sds3sync-host-asan
HOST_ASAN_OBJS = sdsync.asan.o sigv4.asan.o s3put.asan.o paramstore_host.asan.o
ASAN_FLAGS = -fsanitize=address,leak -g -O0

%.asan.o: %.c
	$(CC) $(HOST_CFLAGS) $(ASAN_FLAGS) -c $< -o $@

host-asan: $(HOST_ASAN_PROG)

$(HOST_ASAN_PROG): $(HOST_ASAN_OBJS)
	$(CC) $(HOST_CFLAGS) $(ASAN_FLAGS) $(LDFLAGS) $^ $(HOST_LDLIBS) -o $@

clean:
	rm -f $(PROGS) $(HOST_PROG) $(HOST_ASAN_PROG) *.o *.eap* pa*.conf
