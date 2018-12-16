#!/usr/bin/make -f
PREFIX ?= /usr/local
LV2DIR ?= $(PREFIX)/lib/lv2

OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer -O3 -fno-finite-math-only -DNDEBUG
CXXFLAGS ?= $(OPTIMIZATIONS) -Wall
STRIP ?= strip

conv_VERSION ?= $(shell (git describe --tags HEAD || echo "0") | sed 's/-g.*$$//;s/^v//')

###############################################################################
BUILDDIR=build/

LV2NAME=zeroconvolv
BUNDLE=zeroconvo.lv2

targets =

UNAME=$(shell uname)
ifeq ($(UNAME),Darwin)
  LV2LDFLAGS=-dynamiclib
  LIB_EXT=.dylib
  STRIPFLAGS=-u -r -arch all -s lv2syms
  targets+=lv2syms
  EXTENDED_RE=-E
else
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic
  LIB_EXT=.so
  STRIPFLAGS=-s
  EXTENDED_RE=-r
endif

ifneq ($(XWIN),)
  CC=$(XWIN)-gcc
  CXX=$(XWIN)-g++
  STRIP=$(XWIN)-strip
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic -Wl,--as-needed -lpthread
  LIB_EXT=.dll
  override LDFLAGS += -static-libgcc -static-libstdc++
else
  override CXXFLAGS += -fPIC
endif

targets += $(BUILDDIR)$(LV2NAME)$(LIB_EXT)
override CXXFLAGS += -fvisibility=hidden

###############################################################################
# extract versions
LV2VERSION=$(conv_VERSION)
include git2lv2.mk

###############################################################################
# check for build-dependencies

ifeq ($(shell pkg-config --exists lv2 || echo no), no)
  $(error "LV2 SDK was not found")
endif

ifeq ($(shell pkg-config --atleast-version=1.4 lv2 || echo no), no)
  $(error "LV2 SDK needs to be version 1.4 or later")
endif

ifneq ($(shell pkg-config --exists sndfile samplerate && echo yes), yes)
  $(error "libsndfile and libsamplerate are required")
endif

ifeq ($(LIBZITACONVOLVER),)
  ifeq ($(shell test -f /usr/include/zita-convolver.h -o -f /usr/local/include/zita-convolver.h || echo no ), no)
    $(error "libzita-convolver3 or 4, is required")
  endif
  LOADLIBES += -lzita-convolver
endif

# add library dependent flags and libs

override CXXFLAGS +=`pkg-config --cflags glib-2.0 lv2 sndfile samplerate`
override LOADLIBES +=`pkg-config --libs sndfile samplerate` -lm

ifeq ($(shell pkg-config --atleast-version=1.8.1 lv2 && echo yes), yes)
	override CXXFLAGS += -DHAVE_LV2_1_8
endif

###############################################################################
# build target definitions

default: all

all: $(BUILDDIR)manifest.ttl $(BUILDDIR)presets.ttl $(BUILDDIR)$(LV2NAME).ttl $(targets)

lv2syms:
	echo "_lv2_descriptor" > lv2syms

$(BUILDDIR)manifest.ttl: lv2ttl/manifest.ttl.in
	@mkdir -p $(BUILDDIR)
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LIB_EXT@/$(LIB_EXT)/" \
	  lv2ttl/manifest.ttl.in > $(BUILDDIR)manifest.ttl
	@for file in presets/*.ttl; do \
		echo "" \
			>> $(BUILDDIR)manifest.ttl;\
		head -n 3 $$file \
			| sed "s/@LV2NAME@/$(LV2NAME)/g" \
			>> $(BUILDDIR)manifest.ttl; \
		echo "rdfs:seeAlso <presets.ttl> ." \
			>> $(BUILDDIR)manifest.ttl;\
		done

$(BUILDDIR)presets.ttl: lv2ttl/presets.ttl.in presets/*.ttl presets/ir
	@mkdir -p $(BUILDDIR)
	sed "s/@LV2NAME@/$(LV2NAME)/" \
		lv2ttl/presets.ttl.in > $(BUILDDIR)presets.ttl
	cat presets/*.ttl | sed "s/@LV2NAME@/$(LV2NAME)/g" >> $(BUILDDIR)presets.ttl
	@mkdir -p $(BUILDDIR)/ir
	cp presets/ir/*.wav $(BUILDDIR)/ir/

$(BUILDDIR)$(LV2NAME).ttl: lv2ttl/$(LV2NAME).ttl.in
	@mkdir -p $(BUILDDIR)
	sed "s/@LV2NAME@/$(LV2NAME)/g;s/@VERSION@/lv2:microVersion $(LV2MIC); lv2:minorVersion $(LV2MIN);/" \
		lv2ttl/$(LV2NAME).ttl.in > $(BUILDDIR)$(LV2NAME).ttl

#DSP_SRC = src/lv2.cc src/convolver.cc
DSP_SRC = src/audiosrc.cc src/convolver.cc src/lv2.cc
DSP_DEPS = $(DSP_SRC) src/audiosrc.h src/convolver.h src/readable.h

$(BUILDDIR)$(LV2NAME)$(LIB_EXT): $(DSP_DEPS) Makefile
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
	  -o $(BUILDDIR)$(LV2NAME)$(LIB_EXT) $(DSP_SRC) \
	  $(LIBZITACONVOLVER) \
	  -shared $(LV2LDFLAGS) $(LDFLAGS) $(LOADLIBES)
	$(STRIP) $(STRIPFLAGS) $(BUILDDIR)$(LV2NAME)$(LIB_EXT)

# install/uninstall/clean target definitions

install: all
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)/ir
	install -m755 $(BUILDDIR)$(LV2NAME)$(LIB_EXT) $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m644 $(BUILDDIR)manifest.ttl $(BUILDDIR)presets.ttl $(BUILDDIR)$(LV2NAME).ttl $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m644 $(BUILDDIR)/ir/* $(DESTDIR)$(LV2DIR)/$(BUNDLE)/ir

uninstall:
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/manifest.ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/presets.ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME).ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME)$(LIB_EXT)
	rm -rf $(DESTDIR)$(LV2DIR)/$(BUNDLE)/ir
	-rmdir $(DESTDIR)$(LV2DIR)/$(BUNDLE)

clean:
	rm -f \
		$(BUILDDIR)manifest.ttl \
		$(BUILDDIR)presets.ttl \
		$(BUILDDIR)$(LV2NAME).ttl \
		$(BUILDDIR)$(LV2NAME)$(LIB_EXT) \
		lv2syms
	rm -rf $(BUILDDIR)/ir
	rm -rf $(BUILDDIR)*.dSYM
	-test -d $(BUILDDIR) && rmdir $(BUILDDIR) || true

.PHONY: clean all install uninstall
