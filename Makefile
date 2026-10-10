CC = cc
PREFIX ?= /opt/local
X11BASE ?= /usr/local
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -I$(X11BASE)/include
LDFLAGS += -L$(X11BASE)/lib -Wl,-rpath,$(X11BASE)/lib
LDLIBS += -lXm -lXt -lX11 -lXpm -lkvm

all: xsysmeter

xsysmeter: xsysmeter.o
	$(CC) $(LDFLAGS) -o $@ xsysmeter.o $(LDLIBS)

xsysmeter.o: xsysmeter.c meter001.xpm
	$(CC) $(CPPFLAGS) $(CFLAGS) -c xsysmeter.c -o $@

install: xsysmeter
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 xsysmeter $(DESTDIR)$(PREFIX)/bin/xsysmeter

clean:
	rm -f xsysmeter xsysmeter.o

.PHONY: all install clean
