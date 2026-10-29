VERSION = 1.0
PREFIX = /usr/local

CC = ccache gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -DVERSION=\"$(VERSION)\"
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -O2 \
	-march=skylake -fuse-ld=mold
LDFLAGS = -s
