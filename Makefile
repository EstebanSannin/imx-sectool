# imx-sectool -- plain C, libc only
PROG    := imx-sectool
PREFIX  ?= /usr
BINDIR  ?= $(PREFIX)/bin

# U-Boot command names, installed as symlinks only on request (INSTALL_ALIASES=1)
ALIASES := fuse hab_status

SRCS    := src/main.c src/soc.c src/nvmem.c src/mmio.c \
           src/cmd_fuse.c src/cmd_hab.c src/cmd_boot.c src/cmd_security.c \
           src/fusemap.c
OBJS    := $(SRCS:.c=.o)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wpedantic -Wdeclaration-after-statement

$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/sectool.h
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: check clean install
check: $(PROG)
	./test/run.sh ./$(PROG)

clean:
	rm -f $(PROG) $(OBJS)
	rm -rf test/out

install: $(PROG)
	install -D -m0755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
ifeq ($(INSTALL_ALIASES),1)
	for a in $(ALIASES); do ln -sf $(PROG) $(DESTDIR)$(BINDIR)/$$a; done
endif
