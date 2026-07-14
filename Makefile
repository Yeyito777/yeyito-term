# st - simple terminal
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

DIST_SRC = st.c x.c vimnav.c sshind.c notif.c persist.c cmdline.c search.c

ifeq ($(UNAME_S),Darwin)
SRC = st.c vimnav.c persist.c cmdline.c search.c
OBJC_SRC = macos/backend.m macos/renderer.m macos/pty.m
OBJ = $(SRC:.c=.o) $(OBJC_SRC:.m=.o)
else
SRC = st.c x.c vimnav.c sshind.c notif.c persist.c cmdline.c search.c
OBJ = $(SRC:.c=.o)
endif
APP = .build/st.app

all: st install-hint

config.h:
	cp config.def.h config.h

.c.o:
	$(CC) $(STCFLAGS) -c $<

.m.o:
	$(CC) $(STOBJCFLAGS) -c $< -o $@

st.o: config.h st.h win.h vimnav.h persist.h
x.o: arg.h config.h st.h win.h sshind.h notif.h persist.h cmdline.h search.h render/gpu.c
macos/backend.o: macos/backend.m macos/native.h macos/renderer.h macos/pty.h macos/keysyms.h config.h st.h win.h
macos/renderer.o: macos/renderer.m macos/renderer.h
macos/pty.o: macos/pty.m macos/pty.h
vimnav.o: st.h vimnav.h
sshind.o: sshind.h
notif.o: sshind.h notif.h
persist.o: st.h persist.h
cmdline.o: cmdline.h cmdline_layout.h vimnav.h
search.o: search.h st.h vimnav.h

$(OBJ): config.h config.mk

st: $(OBJ)
	$(CC) -o $@ $(OBJ) $(STLDFLAGS)

app: st macos/Info.plist macos/st.icns
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources/bin
	cp -f st $(APP)/Contents/MacOS/st
	chmod 755 $(APP)/Contents/MacOS/st
	cp -f macos/Info.plist $(APP)/Contents/Info.plist
	cp -f macos/st.icns $(APP)/Contents/Resources/st.icns
	cp -f scripts/st-notify scripts/st-save-cmd $(APP)/Contents/Resources/bin/
	chmod 755 $(APP)/Contents/Resources/bin/st-notify $(APP)/Contents/Resources/bin/st-save-cmd
	codesign --force --deep --sign - $(APP)

install-app: app
	mkdir -p $(HOME)/Applications
	@if [ -d $(HOME)/Applications/st.app ]; then \
		/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -u $(HOME)/Applications/st.app; \
	fi
	rm -rf $(HOME)/Applications/st.app
	ditto $(APP) $(HOME)/Applications/st.app
	touch $(HOME)/Applications/st.app
	/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f $(HOME)/Applications/st.app
	mdimport $(HOME)/Applications/st.app
	rm -rf $(APP)
	@echo "Installed $(HOME)/Applications/st.app"

uninstall-app:
	@if [ -d $(HOME)/Applications/st.app ]; then \
		/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -u $(HOME)/Applications/st.app; \
	fi
	rm -rf $(HOME)/Applications/st.app

install-hint:
	@$(if $(filter Darwin,$(UNAME_S)),echo "Build complete. Install the native app with: make install-app",echo "Build complete. Install changes with: sudo make install")

clean:
	rm -f st $(OBJ) st-$(VERSION).tar.gz
	rm -f x.o sshind.o notif.o macos/backend.o macos/renderer.o
	rm -f a.out
	rm -f tests/*.o tests/test_vimnav
	rm -rf .build

dist: clean
	mkdir -p st-$(VERSION)
	mkdir -p st-$(VERSION)/render
	mkdir -p st-$(VERSION)/macos st-$(VERSION)/scripts
	cp -R CLAUDE.md Makefile README.md TODO.md config.mk\
		config.def.h st.info st.1 arg.h st.h win.h vimnav.h sshind.h notif.h persist.h cmdline.h cmdline_layout.h search.h $(DIST_SRC)\
		st-$(VERSION)
	cp -R render/gpu.c render/README.md st-$(VERSION)/render
	cp -R macos/README.md macos/Info.plist macos/backend.m macos/keysyms.h macos/native.h\
		macos/renderer.h macos/renderer.m macos/pty.h macos/pty.m macos/st-icon.png macos/st.icns\
		st-$(VERSION)/macos
	cp -R scripts/st-notify scripts/st-save-cmd st-$(VERSION)/scripts
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
	cp -f scripts/st-notify $(DESTDIR)$(PREFIX)/bin/st-notify
	chmod 755 $(DESTDIR)$(PREFIX)/bin/st-notify
	cp -f scripts/st-save-cmd $(DESTDIR)$(PREFIX)/bin/st-save-cmd
	chmod 755 $(DESTDIR)$(PREFIX)/bin/st-save-cmd
	@echo Please see the README file regarding the terminfo entry of st.

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/st
	rm -f $(DESTDIR)$(PREFIX)/bin/st-notify
	rm -f $(DESTDIR)$(PREFIX)/bin/st-save-cmd
	rm -f $(DESTDIR)$(MANPREFIX)/man1/st.1

# Testing
TEST_SRC = tests/mocks.c tests/test_vimnav.c vimnav.c
TEST_OBJ = tests/mocks.o tests/test_vimnav.o tests/vimnav.o
TESTFLAGS = -I. -g -Wall -Wextra -D_XOPEN_SOURCE=600

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

# scrollback tests
tests/test_scrollback.o: tests/test_scrollback.c tests/test.h tests/mocks.h st.h
	$(CC) $(TESTFLAGS) -c tests/test_scrollback.c -o tests/test_scrollback.o

test_scrollback: tests/mocks.o tests/test_scrollback.o tests/vimnav.o
	$(CC) -o tests/test_scrollback tests/mocks.o tests/test_scrollback.o tests/vimnav.o

# cwd tests (self-contained - tests OSC 779 parsing logic)
tests/test_cwd.o: tests/test_cwd.c tests/test.h
	$(CC) $(TESTFLAGS) -c tests/test_cwd.c -o tests/test_cwd.o

test_cwd: tests/test_cwd.o
	$(CC) -o tests/test_cwd tests/test_cwd.o

# notif tests (self-contained with X11 mocks - includes notif.c directly)
tests/test_notif.o: tests/test_notif.c tests/test.h sshind.h notif.h notif.c
	$(CC) $(TESTFLAGS) -c tests/test_notif.c -o tests/test_notif.o

test_notif: tests/test_notif.o
	$(CC) -o tests/test_notif tests/test_notif.o

# persist tests (separate compilation — test provides Term + mocks, persist.c links in)
tests/test_persist.o: tests/test_persist.c tests/test.h st.h persist.h
	$(CC) $(TESTFLAGS) -c tests/test_persist.c -o tests/test_persist.o

tests/persist.o: persist.c st.h persist.h
	$(CC) $(TESTFLAGS) -c persist.c -o tests/persist.o

test_persist: tests/test_persist.o tests/persist.o
	$(CC) -o tests/test_persist tests/test_persist.o tests/persist.o

## search tests (includes search.c directly, provides own Term + mocks)
tests/test_search.o: tests/test_search.c tests/test.h st.h search.h vimnav.h
	$(CC) $(TESTFLAGS) -c tests/test_search.c -o tests/test_search.o

test_search: tests/test_search.o
	$(CC) -o tests/test_search tests/test_search.o

# cmdline geometry tests (pure helper, no X11 dependency)
tests/test_cmdline_layout.o: tests/test_cmdline_layout.c tests/test.h cmdline_layout.h
	$(CC) $(TESTFLAGS) -c tests/test_cmdline_layout.c -o tests/test_cmdline_layout.o

test_cmdline_layout: tests/test_cmdline_layout.o
	$(CC) -o tests/test_cmdline_layout tests/test_cmdline_layout.o

ifeq ($(UNAME_S),Darwin)
tests/test_macos_pty.o: tests/test_macos_pty.m tests/test.h macos/pty.h
	$(CC) $(STOBJCFLAGS) -c tests/test_macos_pty.m -o tests/test_macos_pty.o

test_macos_pty: tests/test_macos_pty.o macos/pty.o
	$(CC) -o tests/test_macos_pty tests/test_macos_pty.o macos/pty.o -framework Foundation
endif

test_gpu_regressions: st
	@./tests/test_gpu_regressions.sh

test: test_vimnav test_sshind test_scrollback test_cwd test_notif test_persist test_search test_cmdline_layout
ifeq ($(UNAME_S),Darwin)
test: test_macos_pty
endif
	@echo "Running tests..."
	@./tests/test_vimnav
	@./tests/test_sshind
	@./tests/test_scrollback
	@./tests/test_cwd
	@./tests/test_notif
	@./tests/test_persist
	@./tests/test_search
	@./tests/test_cmdline_layout
ifeq ($(UNAME_S),Darwin)
	@./tests/test_macos_pty
endif

clean-tests:
	rm -f tests/*.o tests/test_vimnav tests/test_sshind tests/test_scrollback tests/test_cwd tests/test_notif tests/test_persist tests/test_search tests/test_cmdline_layout tests/test_macos_pty

.PHONY: all app install-app uninstall-app install-hint clean dist install uninstall test test_gpu_regressions test_macos_pty clean-tests
