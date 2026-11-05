include config.mk

PROTO_XML = protocols/wlr-layer-shell-unstable-v1.xml
PROTO_H = wlr-layer-shell-unstable-v1-client-protocol.h
PROTO_C = wlr-layer-shell-unstable-v1-protocol.c

SRC = osd.c $(PROTO_C)
OBJ = $(SRC:.c=.o)

all: osd

$(OBJ): config.h config.mk font.h $(PROTO_H)

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

osd: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)
	chmod 755 $@

clean:
	rm -f osd $(OBJ) config.h $(PROTO_H) $(PROTO_C)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f osd $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/osd

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/osd

.PHONY: all clean install uninstall
