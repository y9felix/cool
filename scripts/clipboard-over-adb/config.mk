VERSION = 1.0
PREFIX = /usr/local

CC = cc
CPPFLAGS = -DVERSION=\"$(VERSION)\"
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -O2
LDFLAGS = -s
