CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS = -lcrypto -lmosquitto -lcjson -lpthread -lm
PREFIX ?= /usr

kaifareader: kaifareader.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: kaifareader
	install -D -m 0755 kaifareader $(DESTDIR)$(PREFIX)/bin/kaifareader

clean:
	rm -f kaifareader

.PHONY: install clean
