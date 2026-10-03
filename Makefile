# wmyt - YouTube player dockapp for Window Maker

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man1

PKGS    = mpv x11 xext
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -std=c99 -D_GNU_SOURCE $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS))

SRC = src/wmyt.c src/display.c src/font.c src/player.c src/selection.c
OBJ = $(SRC:.c=.o)

all: wmyt

wmyt: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/wmyt.o: src/display.h src/font.h src/player.h src/selection.h
src/display.o: src/display.h
src/font.o: src/font.h
src/player.o: src/player.h
src/selection.o: src/selection.h

install: wmyt
	install -Dm755 wmyt $(DESTDIR)$(BINDIR)/wmyt
	install -Dm644 wmyt.1 $(DESTDIR)$(MANDIR)/wmyt.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/wmyt $(DESTDIR)$(MANDIR)/wmyt.1

clean:
	rm -f wmyt $(OBJ)

.PHONY: all install uninstall clean
