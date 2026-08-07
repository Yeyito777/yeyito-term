# st version
VERSION = 0.9.3

# Customize below to fit your system

UNAME_S = $(shell uname -s)

# paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

PKG_CONFIG = pkg-config

# Linux and other X11 systems
X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib
INCS = -I$(X11INC) \
       `$(PKG_CONFIG) --cflags fontconfig` \
       `$(PKG_CONFIG) --cflags freetype2`
LIBS = -L$(X11LIB) -lm -lrt -lz -lX11 -lutil -lXft -lGL \
       `$(PKG_CONFIG) --libs fontconfig` \
       `$(PKG_CONFIG) --libs freetype2`

STCPPFLAGS = -DVERSION=\"$(VERSION)\" -D_XOPEN_SOURCE=600
CFLAGS = -O3 -march=native -flto -frename-registers
LDFLAGS = -flto

# Native macOS backend: Cocoa window/input, CoreText fonts, and Metal drawing.
ifeq ($(UNAME_S),Darwin)
INCS = -I.
LIBS = -lm -lutil -lz -framework Cocoa -framework Metal -framework MetalKit \
       -framework QuartzCore -framework CoreText -framework CoreGraphics
STCPPFLAGS += -D_DARWIN_C_SOURCE -DST_NATIVE_MACOS
CFLAGS = -O3 -march=native -flto
OBJCFLAGS = -fobjc-arc -mmacosx-version-min=11.0
endif

# flags
STCFLAGS = $(INCS) $(STCPPFLAGS) $(CPPFLAGS) $(CFLAGS)
STOBJCFLAGS = $(INCS) $(STCPPFLAGS) $(CPPFLAGS) $(CFLAGS) $(OBJCFLAGS)
STLDFLAGS = $(LIBS) $(LDFLAGS)

# OpenBSD:
#CPPFLAGS = -DVERSION=\"$(VERSION)\" -D_XOPEN_SOURCE=600 -D_BSD_SOURCE
#LIBS = -L$(X11LIB) -lm -lX11 -lutil -lXft \
#       `$(PKG_CONFIG) --libs fontconfig` \
#       `$(PKG_CONFIG) --libs freetype2`
#MANPREFIX = ${PREFIX}/man

# compiler and linker
# CC = c99
