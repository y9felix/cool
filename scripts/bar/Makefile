.POSIX:

include config.mk

SRC = bar.c wlr-layer-shell-unstable-v1-protocol.c
OBJ = $(SRC:.c=.o)

all: bar

$(OBJ): config.h config.mk font.h wlr-layer-shell-unstable-v1-client-protocol.h

config.h:
	cp config.def.h $@

wlr-layer-shell-unstable-v1-client-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

wlr-layer-shell-unstable-v1-protocol.c: protocols/wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

bar: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f bar $(OBJ) wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-protocol.c

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f bar $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/bar

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/bar

.PHONY: all clean install uninstall
