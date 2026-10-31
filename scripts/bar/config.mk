VERSION = 1.0
PREFIX = /usr/local

CC = ccache gcc
WAYLAND_SCANNER = wayland-scanner

PKGS = wayland-client alsa

CPPFLAGS = -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\" $$(pkg-config --cflags $(PKGS))
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -Wno-unused-parameter -O2
LDFLAGS = -s $$(pkg-config --libs $(PKGS)) -lm
