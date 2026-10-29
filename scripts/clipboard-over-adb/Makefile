.POSIX:

include config.mk

SRC = clip-sync.c
OBJ = $(SRC:.c=.o)

all: clip-sync

$(OBJ): config.h config.mk

config.h:
	cp config.def.h $@

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

clip-sync: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f clip-sync $(OBJ)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f clip-sync $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/clip-sync

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/clip-sync
