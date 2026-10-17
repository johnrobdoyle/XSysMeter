CC = cc
PREFIX ?= /opt/local
X11BASE ?= /usr/local
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
CPPFLAGS += -I$(X11BASE)/include
LDFLAGS += -L$(X11BASE)/lib -Wl,-rpath,$(X11BASE)/lib
LDLIBS += -lXm -lXt -lX11 -lXpm -lkvm -lm

all: xsysmeter

xsysmeter: xsysmeter.o network.o
	$(CC) $(LDFLAGS) -o $@ xsysmeter.o network.o $(LDLIBS)

xsysmeter.o: xsysmeter.c meter001.xpm network.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c xsysmeter.c -o $@

network.o: network.c network.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c network.c -o $@

check:
	$(CC) $(CFLAGS) -DNETWORK_RATE_TEST -I. network.c tests/test_network.c -o test_network -lm
	./test_network

install: xsysmeter
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 xsysmeter $(DESTDIR)$(PREFIX)/bin/xsysmeter

clean:
	rm -f xsysmeter xsysmeter.o network.o test_network

.PHONY: all install clean check
