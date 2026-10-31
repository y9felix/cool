.POSIX:

include config.mk

PROTO_XML = protocols/wlr-layer-shell-unstable-v1.xml
PROTO_H = wlr-layer-shell-unstable-v1-client-protocol.h
PROTO_C = wlr-layer-shell-unstable-v1-protocol.c

SRC = bar.c $(PROTO_C)
OBJ = $(SRC:.c=.o)

all: bar

$(OBJ): config.h config.mk $(PROTO_H)

config.h:
	cp config.def.h $@

$(PROTO_H): $(PROTO_XML)
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO_C): $(PROTO_XML)
	$(WAYLAND_SCANNER) private-code $< $@

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

bar: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f bar $(OBJ) $(PROTO_H) $(PROTO_C)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f bar $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/bar

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/bar

.PHONY: all clean install uninstall
