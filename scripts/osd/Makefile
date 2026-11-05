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
	chmod 644 $@

$(PROTO_H): $(PROTO_XML)
	$(WAYLAND_SCANNER) client-header $< $@
	chmod 644 $@

$(PROTO_C): $(PROTO_XML)
	$(WAYLAND_SCANNER) private-code $< $@
	chmod 644 $@

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<
	chmod 644 $@

lowbat: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)
	chmod 755 $@

clean:
	rm -f lowbat $(OBJ) config.h $(PROTO_H) $(PROTO_C)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f lowbat $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/lowbat
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/lowbat

.PHONY: all clean install uninstall
