PREFIX = /usr/local

CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror -O2
LDFLAGS = -s
