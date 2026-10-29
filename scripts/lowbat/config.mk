VERSION = 1.0
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

CC = ccache gcc
WAYLAND_SCANNER = wayland-scanner

CPPFLAGS = -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\"
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -Wno-unused-parameter -O2 \
	-march=skylake -fuse-ld=mold
LDFLAGS = -s -lwayland-client
