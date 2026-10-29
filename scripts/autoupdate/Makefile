.POSIX:

include config.mk

SRC = autoupdate.c
OBJ = $(SRC:.c=.o)

all: autoupdate

$(OBJ): config.h config.mk

config.h:
	cp config.def.h $@

.c.o:
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $<

autoupdate: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

clean:
	rm -f autoupdate $(OBJ)

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f autoupdate $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/autoupdate

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/autoupdate
