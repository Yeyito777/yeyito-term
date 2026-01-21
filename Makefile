# st - simple terminal
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

SRC = st.c x.c vimnav.c sshind.c
OBJ = $(SRC:.c=.o)

all: st

config.h:
	cp config.def.h config.h

.c.o:
	$(CC) $(STCFLAGS) -c $<

st.o: config.h st.h win.h vimnav.h
x.o: arg.h config.h st.h win.h sshind.h
vimnav.o: st.h vimnav.h
sshind.o: sshind.h

$(OBJ): config.h config.mk

st: $(OBJ)
	$(CC) -o $@ $(OBJ) $(STLDFLAGS)

clean:
	rm -f st $(OBJ) st-$(VERSION).tar.gz
	rm -f a.out
	rm -f tests/*.o tests/test_vimnav

dist: clean
	mkdir -p st-$(VERSION)
	cp -R FAQ LEGACY TODO LICENSE Makefile README config.mk\
		config.def.h st.info st.1 arg.h st.h win.h vimnav.h sshind.h $(SRC)\
		st-$(VERSION)
	tar -cf - st-$(VERSION) | gzip > st-$(VERSION).tar.gz
	rm -rf st-$(VERSION)

install: st
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f st $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/st
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < st.1 > $(DESTDIR)$(MANPREFIX)/man1/st.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/st.1
	tic -sx st.info
	@echo Please see the README file regarding the terminfo entry of st.

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/st
	rm -f $(DESTDIR)$(MANPREFIX)/man1/st.1

# Testing
TEST_SRC = tests/mocks.c tests/test_vimnav.c vimnav.c
TEST_OBJ = tests/mocks.o tests/test_vimnav.o tests/vimnav.o
TESTFLAGS = -I. -g -Wall -Wextra

tests/mocks.o: tests/mocks.c tests/mocks.h st.h
	$(CC) $(TESTFLAGS) -c tests/mocks.c -o tests/mocks.o

tests/test_vimnav.o: tests/test_vimnav.c tests/test.h tests/mocks.h vimnav.h st.h
	$(CC) $(TESTFLAGS) -c tests/test_vimnav.c -o tests/test_vimnav.o

tests/vimnav.o: vimnav.c vimnav.h st.h
	$(CC) $(TESTFLAGS) -c vimnav.c -o tests/vimnav.o

test_vimnav: $(TEST_OBJ)
	$(CC) -o tests/test_vimnav $(TEST_OBJ)

# sshind tests (self-contained with X11 mocks - includes sshind.c directly)
tests/test_sshind.o: tests/test_sshind.c tests/test.h sshind.h sshind.c
	$(CC) $(TESTFLAGS) -c tests/test_sshind.c -o tests/test_sshind.o

test_sshind: tests/test_sshind.o
	$(CC) -o tests/test_sshind tests/test_sshind.o

test: test_vimnav test_sshind
	@echo "Running tests..."
	@./tests/test_vimnav
	@./tests/test_sshind

clean-tests:
	rm -f tests/*.o tests/test_vimnav tests/test_sshind

.PHONY: all clean dist install uninstall test clean-tests
