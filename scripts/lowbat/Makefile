.POSIX:

include config.mk

PROTO_XML = protocols/wlr-layer-shell-unstable-v1.xml
PROTO_H = wlr-layer-shell-unstable-v1-client-protocol.h
PROTO_C = wlr-layer-shell-unstable-v1-protocol.c

SRC = lowbat.c $(PROTO_C)
OBJ = $(SRC:.c=.o)

all: lowbat

$(OBJ): config.h config.mk $(PROTO_H)

config.h:
	cp config.def.h $@

$(PROTO_H): $(PROTO_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO_C): $(PROTO_XML)
	$(WAYLAND_SCANNER) private-code $< $@

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

lowbat: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f lowbat $(OBJ) config.h $(PROTO_H) $(PROTO_C)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f lowbat $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/lowbat
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	cp -f lowbat.1 $(DESTDIR)$(MANPREFIX)/man1/lowbat.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/lowbat.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/lowbat
	rm -f $(DESTDIR)$(MANPREFIX)/man1/lowbat.1

.PHONY: all clean install uninstall
