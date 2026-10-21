PREFIX ?= /usr/local
BIN = lowbat

PKGS = wayland-client
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS))

WAYLAND_SCANNER ?= wayland-scanner
WLR_XML ?= /usr/share/wlr-protocols/unstable/wlr-layer-shell-unstable-v1.xml

all: $(BIN)

wlr-layer-shell-unstable-v1-client-protocol.h: $(WLR_XML)
	$(WAYLAND_SCANNER) client-header $< $@

wlr-layer-shell-unstable-v1-protocol.c: $(WLR_XML)
	$(WAYLAND_SCANNER) private-code $< $@

$(BIN): lowbat.c wlr-layer-shell-unstable-v1-protocol.c wlr-layer-shell-unstable-v1-client-protocol.h
	$(CC) $(CFLAGS) $(LDFLAGS) lowbat.c wlr-layer-shell-unstable-v1-protocol.c -o $@ $(LDLIBS)

clean:
	rm -f $(BIN) wlr-layer-shell-unstable-v1-client-protocol.h wlr-layer-shell-unstable-v1-protocol.c

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

.PHONY: all clean install
