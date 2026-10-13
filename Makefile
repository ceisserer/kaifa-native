CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib

LIB_LDLIBS = -lcrypto -lm
LDLIBS = $(LIB_LDLIBS) -lmosquitto -lcjson -lpthread

SOVERSION = 1
VERSION = 1.0.0
SONAME = libkaifa.so.$(SOVERSION)

# MQTT bridge (default), libkaifa is linked in statically
kaifareader: kaifa2mqtt.c kaifa.o kaifa.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ kaifa2mqtt.c kaifa.o $(LDLIBS)

kaifa.o: kaifa.c kaifa.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -fvisibility=hidden -c -o $@ kaifa.c

# shared library, only needs libcrypto
lib: libkaifa.so.$(VERSION) libkaifa.a kaifa.pc

libkaifa.so.$(VERSION): kaifa.o
	$(CC) $(CFLAGS) $(LDFLAGS) -shared -Wl,-soname,$(SONAME) -o $@ $< $(LIB_LDLIBS)
	ln -sf $@ $(SONAME)
	ln -sf $(SONAME) libkaifa.so

libkaifa.a: kaifa.o
	$(AR) rcs $@ $<

kaifa.pc: kaifa.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@VERSION@|$(VERSION)|' $< > $@

example: examples/kaifa_example

examples/kaifa_example: examples/kaifa_example.c lib
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -I. -o $@ $< -L. -lkaifa

all: kaifareader lib

install: kaifareader
	install -D -m 0755 kaifareader $(DESTDIR)$(PREFIX)/bin/kaifareader

install-lib: lib
	install -D -m 0755 libkaifa.so.$(VERSION) $(DESTDIR)$(LIBDIR)/libkaifa.so.$(VERSION)
	ln -sf libkaifa.so.$(VERSION) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libkaifa.so
	install -D -m 0644 libkaifa.a $(DESTDIR)$(LIBDIR)/libkaifa.a
	install -D -m 0644 kaifa.h $(DESTDIR)$(PREFIX)/include/kaifa.h
	install -D -m 0644 kaifa.pc $(DESTDIR)$(LIBDIR)/pkgconfig/kaifa.pc

clean:
	rm -f kaifareader kaifa.o libkaifa.so* libkaifa.a kaifa.pc examples/kaifa_example

.PHONY: all lib example install install-lib clean
