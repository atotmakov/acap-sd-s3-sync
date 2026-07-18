PROG1 = sds3sync
OBJS1 = sdsync.o sigv4.o s3put.o
PROGS = $(PROG1)

PKGS = gio-2.0 glib-2.0 axevent axparameter libcurl openssl
CFLAGS += $(shell pkg-config --cflags $(PKGS)) -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64
LDLIBS += $(shell pkg-config --libs $(PKGS))

all: $(PROGS)

$(PROG1): $(OBJS1)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f $(PROGS) *.o *.eap* pa*.conf
