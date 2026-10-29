.POSIX:

CC = ccache gcc
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -O2 -march=skylake -fuse-ld=mold
LDFLAGS = -s

all: m3u8gen

m3u8gen: m3u8gen.c
	$(CC) $(CFLAGS) -o $@ m3u8gen.c $(LDFLAGS)

clean:
	rm -f m3u8gen
