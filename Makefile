# st - simple terminal
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

DIST_SRC = st.c x.c graphics.c graphics.h video.c video.h clipboard5522.c clipboard5522.h vimnav.c sshind.c notif.c persist.c cmdline.c search.c

ifeq ($(UNAME_S),Darwin)
SRC = st.c graphics.c video.c clipboard5522.c vimnav.c persist.c cmdline.c search.c macos/locale.c macos/emoji.c macos/spare.c macos/control.c macos/launchlog.c
OBJC_SRC = macos/backend.m macos/renderer.m macos/pty.m macos/pasteboard5522.m
OBJ = $(SRC:.c=.o) $(OBJC_SRC:.m=.o)
else
SRC = st.c x.c graphics.c video.c clipboard5522.c vimnav.c sshind.c notif.c persist.c cmdline.c search.c
OBJ = $(SRC:.c=.o)
endif
OBJ += render/image_occlusion.o render/smoothscroll.o render/smoothscroll_term.o
APP = .build/st.app

ifeq ($(UNAME_S),Darwin)
# The AeroSpace launcher, a separate small program bundled with the app.
LAUNCHER = st-aerospace-launch
LAUNCHER_OBJ = macos/aerospace_launch.o macos/aerospace.o macos/spare.o macos/launchlog.o
endif

all: st $(LAUNCHER) install-hint

# st-play: the video player (play/, docs/video.md).  Built separately
# (make st-play) because it needs the ffmpeg libraries.
FFMPEG_PKGS = libavformat libavcodec libswscale libswresample libavutil
PLAY_CFLAGS = -O2 -g -Wall -Wextra `$(PKG_CONFIG) --cflags $(FFMPEG_PKGS)`
PLAY_LIBS = `$(PKG_CONFIG) --libs $(FFMPEG_PKGS)` -lpthread -lm
PLAY_OBJ = play/st-play.o play/media.o play/flow.o play/out.o play/input.o

play/st-play.o: play/st-play.c play/flow.h play/input.h play/media.h play/out.h
	$(CC) $(PLAY_CFLAGS) -c play/st-play.c -o $@
play/media.o: play/media.c play/media.h
	$(CC) $(PLAY_CFLAGS) -c play/media.c -o $@
play/flow.o: play/flow.c play/flow.h
	$(CC) $(PLAY_CFLAGS) -c play/flow.c -o $@
play/out.o: play/out.c play/out.h
	$(CC) $(PLAY_CFLAGS) -c play/out.c -o $@
play/input.o: play/input.c play/input.h
	$(CC) $(PLAY_CFLAGS) -c play/input.c -o $@

st-play: $(PLAY_OBJ)
	$(CC) -o $@ $(PLAY_OBJ) $(PLAY_LIBS)

config.h:
	cp config.def.h config.h

.c.o:
	$(CC) $(STCFLAGS) -c $<

.m.o:
	$(CC) $(STOBJCFLAGS) -c $< -o $@

st.o: config.h st.h win.h graphics.h video.h vimnav.h persist.h macos/emoji.h
x.o: arg.h config.h st.h win.h xstate.h graphics.h sync.h clipboard5522.h sshind.h notif.h persist.h cmdline.h search.h render/gpu.c
graphics.o: graphics.c graphics.h video.h st.h vendor/stb_image.h
video.o: video.c video.h
render/image_occlusion.o: render/image_occlusion.c render/image_occlusion.h graphics.h st.h
	$(CC) $(STCFLAGS) -c render/image_occlusion.c -o $@
render/smoothscroll.o: render/smoothscroll.c render/smoothscroll.h
	$(CC) $(STCFLAGS) -c render/smoothscroll.c -o $@
render/smoothscroll_term.o: render/smoothscroll_term.c render/smoothscroll.h st.h win.h
	$(CC) $(STCFLAGS) -c render/smoothscroll_term.c -o $@
st.o x.o macos/backend.o: render/smoothscroll.h
x.o macos/backend.o: render/image_occlusion.h
clipboard5522.o: clipboard5522.c clipboard5522.h
macos/backend.o: macos/backend.m macos/native.h macos/renderer.h macos/glyph_layout.h macos/pty.h macos/pasteboard5522.h macos/keysyms.h macos/reveal.h macos/spare.h macos/control.h macos/launchlog.h macos/text_input.h config.h st.h win.h graphics.h sync.h
macos/pasteboard5522.o: macos/pasteboard5522.m macos/pasteboard5522.h clipboard5522.h
macos/locale.o: macos/locale.c macos/locale.h
	$(CC) $(STCFLAGS) -c macos/locale.c -o macos/locale.o
macos/spare.o: macos/spare.c macos/spare.h
	$(CC) $(STCFLAGS) -c macos/spare.c -o macos/spare.o
macos/aerospace.o: macos/aerospace.c macos/aerospace.h config.mk
	$(CC) $(STCFLAGS) -c macos/aerospace.c -o macos/aerospace.o
macos/launchlog.o: macos/launchlog.c macos/launchlog.h
	$(CC) $(STCFLAGS) -c macos/launchlog.c -o macos/launchlog.o
macos/aerospace_launch.o: macos/aerospace_launch.c macos/aerospace.h macos/spare.h macos/launchlog.h config.mk
	$(CC) $(STCFLAGS) -c macos/aerospace_launch.c -o macos/aerospace_launch.o
macos/control.o: macos/control.c macos/control.h
	$(CC) $(STCFLAGS) -c macos/control.c -o macos/control.o
macos/emoji.o: macos/emoji.c macos/emoji.h st.h
	$(CC) $(STCFLAGS) -c macos/emoji.c -o macos/emoji.o
macos/renderer.o: macos/renderer.m macos/renderer.h macos/glyph_layout.h
macos/pty.o: macos/pty.m macos/pty.h
vimnav.o: st.h vimnav.h
sshind.o: sshind.h xstate.h
notif.o: sshind.h notif.h xstate.h
persist.o: st.h persist.h
cmdline.o: cmdline.h cmdline_layout.h vimnav.h xstate.h
search.o: search.h st.h vimnav.h

$(OBJ): config.h config.mk

st: $(OBJ)
	$(CC) -o $@ $(OBJ) $(STLDFLAGS)

st-aerospace-launch: $(LAUNCHER_OBJ)
	$(CC) -o $@ $(LAUNCHER_OBJ) $(LDFLAGS)

app: st st-aerospace-launch macos/Info.plist macos/st.icns
	rm -rf $(APP)
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources/bin
	cp -f st $(APP)/Contents/MacOS/st
	chmod 755 $(APP)/Contents/MacOS/st
	cp -f macos/Info.plist $(APP)/Contents/Info.plist
	cp -f macos/st.icns $(APP)/Contents/Resources/st.icns
	cp -f scripts/st-notify scripts/st-save-cmd scripts/st-shell-context.zsh st-aerospace-launch $(APP)/Contents/Resources/bin/
	chmod 755 $(APP)/Contents/Resources/bin/st-notify $(APP)/Contents/Resources/bin/st-save-cmd \
		$(APP)/Contents/Resources/bin/st-aerospace-launch
	codesign --force --sign - $(APP)/Contents/Resources/bin/st-aerospace-launch
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
	rm -f x.o sshind.o notif.o macos/backend.o macos/renderer.o macos/pasteboard5522.o
	rm -f st-aerospace-launch macos/aerospace_launch.o macos/aerospace.o
	rm -f a.out
	rm -f st-play $(PLAY_OBJ)
	$(MAKE) clean-tests
	rm -rf .build

dist: clean
	mkdir -p st-$(VERSION)
	mkdir -p st-$(VERSION)/render
	mkdir -p st-$(VERSION)/docs st-$(VERSION)/macos st-$(VERSION)/scripts
	cp -R CLAUDE.md Makefile README.md TODO.md config.mk\
		config.def.h st.info st.1 arg.h st.h win.h xstate.h sync.h vimnav.h sshind.h notif.h persist.h cmdline.h cmdline_layout.h search.h $(DIST_SRC)\
		st-$(VERSION)
	cp -R render/gpu.c render/image_occlusion.c render/image_occlusion.h render/smoothscroll.c render/smoothscroll.h render/smoothscroll_term.c render/README.md st-$(VERSION)/render
	cp -R docs/kitty-graphics.md st-$(VERSION)/docs
	mkdir -p st-$(VERSION)/vendor
	cp -R vendor/stb_image.h st-$(VERSION)/vendor
	cp -R macos/README.md macos/Info.plist macos/backend.m macos/pasteboard5522.h macos/pasteboard5522.m macos/keysyms.h macos/native.h macos/reveal.h macos/text_input.h\
		macos/renderer.h macos/renderer.m macos/glyph_layout.h macos/emoji.h macos/emoji.c\
		macos/pty.h macos/pty.m macos/locale.h macos/locale.c macos/spare.h macos/spare.c macos/control.h macos/control.c macos/launchlog.h macos/launchlog.c\
		macos/aerospace.h macos/aerospace.c macos/aerospace_launch.c\
		macos/st-icon.png macos/st.icns\
		st-$(VERSION)/macos
	cp -R scripts/st-notify scripts/st-save-cmd scripts/st-shell-context.zsh st-$(VERSION)/scripts
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
	@if $(PKG_CONFIG) --exists $(FFMPEG_PKGS); then \
		$(MAKE) st-play && cp -f st-play $(DESTDIR)$(PREFIX)/bin/st-play && \
		chmod 755 $(DESTDIR)$(PREFIX)/bin/st-play; \
	else \
		echo "st-play not installed: ffmpeg development libraries not found"; \
	fi
	@echo Please see the README file regarding the terminfo entry of st.

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/st
	rm -f $(DESTDIR)$(PREFIX)/bin/st-notify
	rm -f $(DESTDIR)$(PREFIX)/bin/st-save-cmd
	rm -f $(DESTDIR)$(PREFIX)/bin/st-play
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

tests/test_image_occlusion.o: tests/test_image_occlusion.c tests/test.h render/image_occlusion.h graphics.h st.h
	$(CC) $(TESTFLAGS) -c tests/test_image_occlusion.c -o $@

test_image_occlusion: tests/test_image_occlusion.o render/image_occlusion.o
	$(CC) -o tests/test_image_occlusion tests/test_image_occlusion.o render/image_occlusion.o

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

tests/test_mode_reset.o: tests/test_mode_reset.c tests/test.h win.h
	$(CC) $(TESTFLAGS) -c tests/test_mode_reset.c -o tests/test_mode_reset.o

test_mode_reset: tests/test_mode_reset.o
	$(CC) -o tests/test_mode_reset tests/test_mode_reset.o

tests/test_sync.o: tests/test_sync.c tests/test.h sync.h
	$(CC) $(TESTFLAGS) -c tests/test_sync.c -o tests/test_sync.o

test_sync: tests/test_sync.o
	$(CC) -o tests/test_sync tests/test_sync.o

tests/test_clipboard5522.o: tests/test_clipboard5522.c tests/test.h clipboard5522.h
	$(CC) $(TESTFLAGS) -c tests/test_clipboard5522.c -o tests/test_clipboard5522.o

test_clipboard5522: tests/test_clipboard5522.o clipboard5522.o
	$(CC) -o tests/test_clipboard5522 tests/test_clipboard5522.o clipboard5522.o

tests/test_graphics.o: tests/test_graphics.c tests/test.h graphics.h st.h
	$(CC) $(TESTFLAGS) -c tests/test_graphics.c -o tests/test_graphics.o

test_graphics: tests/test_graphics.o graphics.o video.o
	$(CC) -o tests/test_graphics tests/test_graphics.o graphics.o video.o -lz

tests/test_video.o: tests/test_video.c tests/test.h graphics.h video.h st.h
	$(CC) $(TESTFLAGS) -c tests/test_video.c -o tests/test_video.o

test_video: tests/test_video.o graphics.o video.o
	$(CC) -o tests/test_video tests/test_video.o graphics.o video.o -lz

# st-play: parser, framing and flow-control unit tests (no ffmpeg needed)
tests/test_play.o: tests/test_play.c tests/test.h play/flow.h play/input.h play/out.h
	$(CC) $(TESTFLAGS) -c tests/test_play.c -o tests/test_play.o

tests/play_flow.o: play/flow.c play/flow.h
	$(CC) $(TESTFLAGS) -c play/flow.c -o $@
tests/play_input.o: play/input.c play/input.h
	$(CC) $(TESTFLAGS) -c play/input.c -o $@
tests/play_out.o: play/out.c play/out.h
	$(CC) $(TESTFLAGS) -c play/out.c -o $@

test_play: tests/test_play.o tests/play_flow.o tests/play_input.o tests/play_out.o
	$(CC) -o tests/test_play tests/test_play.o tests/play_flow.o tests/play_input.o tests/play_out.o -lm

# Fake terminal for st-play end-to-end runs: st's graphics.c/video.c, a
# timing-model video backend and a throttled PTY link (decodes with
# libavcodec to validate the stream).
tests/fake_video_term: tests/fake_video_term.c graphics.o video.o graphics.h video.h
	$(CC) $(TESTFLAGS) -O2 -DFAKE_DECODE `$(PKG_CONFIG) --cflags libavcodec libavutil` \
		tests/fake_video_term.c graphics.o video.o -o $@ -lz -lm \
		`$(PKG_CONFIG) --libs libavcodec libavutil`

# End-to-end st-play runs through the fake terminal (skipped without ffmpeg).
test_play_e2e:
	@if $(PKG_CONFIG) --exists $(FFMPEG_PKGS) && command -v ffmpeg >/dev/null; then \
		$(MAKE) -s st-play tests/fake_video_term && ./tests/test_play_e2e.sh; \
	else \
		echo "test_play_e2e: skipped (no ffmpeg)"; \
	fi

# smooth scrolling engine + terminal glue (includes both .c files, fake terminal)
tests/test_smoothscroll.o: tests/test_smoothscroll.c tests/test.h render/smoothscroll.c render/smoothscroll_term.c render/smoothscroll.h st.h win.h
	$(CC) $(TESTFLAGS) -c tests/test_smoothscroll.c -o tests/test_smoothscroll.o

test_smoothscroll: tests/test_smoothscroll.o
	$(CC) -o tests/test_smoothscroll tests/test_smoothscroll.o -lm

ifeq ($(UNAME_S),Darwin)
# Include the production backend/renderer to test their actual private emitters
# against an offscreen Metal target; link the normal terminal core.
MAC_IMAGE_TEST_OBJ = $(filter-out macos/backend.o macos/renderer.o,$(OBJ))
tests/test_macos_image_occlusion.o: tests/test_macos_image_occlusion.m tests/test.h macos/backend.o macos/renderer.o
	$(CC) $(STOBJCFLAGS) -c tests/test_macos_image_occlusion.m -o $@

test_macos_image_occlusion: tests/test_macos_image_occlusion.o $(MAC_IMAGE_TEST_OBJ)
	$(CC) -o tests/test_macos_image_occlusion tests/test_macos_image_occlusion.o $(MAC_IMAGE_TEST_OBJ) $(STLDFLAGS)

tests/test_macos_mouse_cursor.o: tests/test_macos_mouse_cursor.m tests/test.h macos/backend.o macos/renderer.o
	$(CC) $(STOBJCFLAGS) -c tests/test_macos_mouse_cursor.m -o $@

test_macos_mouse_cursor: tests/test_macos_mouse_cursor.o $(MAC_IMAGE_TEST_OBJ)
	$(CC) -o tests/test_macos_mouse_cursor tests/test_macos_mouse_cursor.o $(MAC_IMAGE_TEST_OBJ) $(STLDFLAGS)

tests/test_macos_symbol_input.o: tests/test_macos_symbol_input.m tests/test.h scripts/macos-type-symbol.m macos/backend.o macos/renderer.o
	$(CC) $(STOBJCFLAGS) -c tests/test_macos_symbol_input.m -o $@

tests/macos_symbol_sender: tests/macos_symbol_sender.m scripts/macos-type-symbol.m
	$(CC) $(STOBJCFLAGS) tests/macos_symbol_sender.m -o $@ -framework Cocoa

test_macos_symbol_input: tests/test_macos_symbol_input.o $(MAC_IMAGE_TEST_OBJ) tests/macos_symbol_sender
	$(CC) -o tests/test_macos_symbol_input tests/test_macos_symbol_input.o $(MAC_IMAGE_TEST_OBJ) $(STLDFLAGS)

tests/test_macos_pty.o: tests/test_macos_pty.m tests/test.h macos/pty.h
	$(CC) $(STOBJCFLAGS) -c tests/test_macos_pty.m -o tests/test_macos_pty.o

test_macos_pty: tests/test_macos_pty.o macos/pty.o
	$(CC) -o tests/test_macos_pty tests/test_macos_pty.o macos/pty.o -framework Foundation

tests/test_macos_reveal.o: tests/test_macos_reveal.c tests/test.h macos/reveal.h
	$(CC) $(TESTFLAGS) -c tests/test_macos_reveal.c -o tests/test_macos_reveal.o

test_macos_reveal: tests/test_macos_reveal.o
	$(CC) -o tests/test_macos_reveal tests/test_macos_reveal.o

tests/test_macos_spare.o: tests/test_macos_spare.c tests/test.h macos/spare.h
	$(CC) $(TESTFLAGS) -c tests/test_macos_spare.c -o tests/test_macos_spare.o

test_macos_spare: tests/test_macos_spare.o macos/spare.o
	$(CC) -o tests/test_macos_spare tests/test_macos_spare.o macos/spare.o

tests/test_macos_launchlog.o: tests/test_macos_launchlog.c tests/test.h macos/launchlog.h
	$(CC) $(TESTFLAGS) -c tests/test_macos_launchlog.c -o tests/test_macos_launchlog.o

test_macos_launchlog: tests/test_macos_launchlog.o macos/launchlog.o
	$(CC) -o tests/test_macos_launchlog tests/test_macos_launchlog.o macos/launchlog.o

tests/test_macos_aerospace.o: tests/test_macos_aerospace.c tests/test.h macos/aerospace.h macos/spare.h
	$(CC) $(TESTFLAGS) -c tests/test_macos_aerospace.c -o tests/test_macos_aerospace.o

test_macos_aerospace: tests/test_macos_aerospace.o macos/aerospace.o macos/spare.o
	$(CC) -o tests/test_macos_aerospace tests/test_macos_aerospace.o macos/aerospace.o macos/spare.o

tests/test_macos_control.o: tests/test_macos_control.c tests/test.h macos/control.h
	$(CC) $(TESTFLAGS) -c tests/test_macos_control.c -o tests/test_macos_control.o

test_macos_control: tests/test_macos_control.o macos/control.o
	$(CC) -o tests/test_macos_control tests/test_macos_control.o macos/control.o

tests/test_macos_text_input.o: tests/test_macos_text_input.c tests/test.h macos/text_input.h
	$(CC) $(TESTFLAGS) -c tests/test_macos_text_input.c -o tests/test_macos_text_input.o

test_macos_text_input: tests/test_macos_text_input.o
	$(CC) -o tests/test_macos_text_input tests/test_macos_text_input.o

tests/test_macos_locale.o: tests/test_macos_locale.c tests/test.h macos/locale.h
	$(CC) $(TESTFLAGS) -c tests/test_macos_locale.c -o tests/test_macos_locale.o

test_macos_locale: tests/test_macos_locale.o macos/locale.o
	$(CC) -o tests/test_macos_locale tests/test_macos_locale.o macos/locale.o

tests/test_macos_glyph_layout.o: tests/test_macos_glyph_layout.c tests/test.h macos/glyph_layout.h
	$(CC) $(TESTFLAGS) -c tests/test_macos_glyph_layout.c -o tests/test_macos_glyph_layout.o

test_macos_glyph_layout: tests/test_macos_glyph_layout.o
	$(CC) -o tests/test_macos_glyph_layout tests/test_macos_glyph_layout.o

tests/test_macos_emoji.o: tests/test_macos_emoji.c tests/test.h macos/emoji.h st.h
	$(CC) $(TESTFLAGS) -c tests/test_macos_emoji.c -o tests/test_macos_emoji.o

tests/macos_emoji.o: macos/emoji.c macos/emoji.h st.h
	$(CC) $(TESTFLAGS) -c macos/emoji.c -o tests/macos_emoji.o

test_macos_emoji: tests/test_macos_emoji.o tests/macos_emoji.o
	$(CC) -o tests/test_macos_emoji tests/test_macos_emoji.o tests/macos_emoji.o

tests/test_macos_pasteboard5522.o: tests/test_macos_pasteboard5522.m tests/test.h macos/pasteboard5522.h clipboard5522.h
	$(CC) $(STOBJCFLAGS) -c tests/test_macos_pasteboard5522.m -o tests/test_macos_pasteboard5522.o

test_macos_pasteboard5522: tests/test_macos_pasteboard5522.o macos/pasteboard5522.o clipboard5522.o
	$(CC) -o tests/test_macos_pasteboard5522 tests/test_macos_pasteboard5522.o macos/pasteboard5522.o clipboard5522.o -framework AppKit -framework ImageIO
endif

test_gpu_regressions: st
	@./tests/test_gpu_regressions.sh
	@./tests/test_cursor_rendering.sh
	@./tests/test_image_occlusion.sh

test_aerospace_launcher: $(LAUNCHER)
	@./tests/test_aerospace_launcher.sh
ifeq ($(UNAME_S),Darwin)
	@zsh -f ./tests/test_shell_context.zsh
endif

test: test_image_occlusion test_smoothscroll test_vimnav test_sshind test_scrollback test_cwd test_notif test_persist test_search test_cmdline_layout test_mode_reset test_sync test_clipboard5522 test_graphics test_video test_play test_aerospace_launcher
ifeq ($(UNAME_S),Darwin)
test: test_macos_image_occlusion test_macos_mouse_cursor test_macos_pty test_macos_reveal test_macos_spare test_macos_launchlog test_macos_aerospace test_macos_control test_macos_text_input test_macos_locale test_macos_glyph_layout test_macos_emoji test_macos_pasteboard5522
test: test_macos_symbol_input
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
	@./tests/test_mode_reset
	@./tests/test_sync
	@./tests/test_clipboard5522
	@./tests/test_graphics
	@./tests/test_video
	@./tests/test_play
	@$(MAKE) -s test_play_e2e
	@./tests/test_image_occlusion
	@./tests/test_smoothscroll
ifeq ($(UNAME_S),Darwin)
	@./tests/test_macos_image_occlusion
	@./tests/test_macos_mouse_cursor
	@./tests/test_macos_pty
	@./tests/test_macos_reveal
	@./tests/test_macos_spare
	@./tests/test_macos_launchlog
	@./tests/test_macos_aerospace
	@./tests/test_macos_control
	@./tests/test_macos_text_input
	@./tests/test_macos_symbol_input
	@./tests/test_macos_locale
	@./tests/test_macos_glyph_layout
	@./tests/test_macos_emoji
	@./tests/test_macos_pasteboard5522
endif

clean-tests:
	rm -f tests/test_macos_symbol_input tests/test_macos_symbol_input.o tests/macos_symbol_sender
	rm -f tests/*.o tests/test_macos_image_occlusion tests/test_macos_mouse_cursor tests/test_image_occlusion tests/test_vimnav tests/test_sshind tests/test_scrollback tests/test_cwd tests/test_notif tests/test_persist tests/test_search tests/test_cmdline_layout tests/test_mode_reset tests/test_sync tests/test_clipboard5522 tests/test_graphics tests/test_video tests/test_play tests/fake_video_term tests/test_smoothscroll tests/test_macos_pty tests/test_macos_reveal tests/test_macos_spare tests/test_macos_launchlog tests/test_macos_aerospace tests/test_macos_control tests/test_macos_text_input tests/test_macos_locale tests/test_macos_glyph_layout tests/test_macos_emoji tests/test_macos_pasteboard5522

.PHONY: all app install-app uninstall-app install-hint clean dist install uninstall test test_image_occlusion test_smoothscroll test_macos_image_occlusion test_macos_mouse_cursor test_gpu_regressions test_aerospace_launcher test_mode_reset test_sync test_graphics test_video test_macos_pty test_macos_reveal test_macos_spare test_macos_launchlog test_macos_aerospace test_macos_control test_macos_text_input test_macos_locale test_macos_glyph_layout test_macos_emoji test_macos_pasteboard5522 clean-tests
.PHONY: test_macos_symbol_input test_play test_play_e2e
