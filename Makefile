# Path to the VCV Rack SDK. Override on the command line or via environment:
#   make RACK_DIR=/path/to/Rack-SDK
RACK_DIR ?= ../Rack-SDK

# reSID (MOS 6581/8580 emulation, GPL-2.0-or-later), vendored in dep/resid from
# https://github.com/libsidplayfp/resid
RESID := dep/resid
RESID_SOURCES := sid.cc voice.cc wave.cc envelope.cc filter.cc dac.cc extfilt.cc pot.cc version.cc
RESID_DATA := $(wildcard $(RESID)/*.dat)
# Headers reSID's autotools build normally generates
RESID_GENERATED := $(RESID)/siddefs.h $(RESID_DATA:.dat=.h)

FLAGS += -I$(RESID)
CFLAGS +=
CXXFLAGS +=
LDFLAGS +=

SOURCES += $(wildcard src/*.cpp)
SOURCES += $(addprefix $(RESID)/, $(RESID_SOURCES))

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/plugin.mk

# reSID is third-party code: keep its warnings out of the build log
build/$(RESID)/%.cc.o: FLAGS += -w -DVERSION=\"1.0-pre2\"

$(OBJECTS): $(RESID_GENERATED)

$(RESID)/siddefs.h: $(RESID)/siddefs.h.in
	sed -e 's/@RESID_INLINING@/1/; s/@RESID_INLINE@/inline/; s/@RESID_BRANCH_HINTS@/1/; s/@NEW_8580_FILTER@/0/' \
	    -e 's/@HAVE_BOOL@/1/; s/@HAVE_BUILTIN_EXPECT@/1/; s/@HAVE_LOG1P@/1/' $< > $@

$(RESID)/%.h: $(RESID)/%.dat $(RESID)/samp2src.pl
	perl $(RESID)/samp2src.pl $* $< $@

# Offline test: renders every factory patch to WAV in build/test
test: $(RESID_GENERATED)
	@mkdir -p build/test
	$(CXX) -O2 -I$(RESID) -Isrc -DVERSION=\"1.0-pre2\" -w -o build/test/render test/render.cpp src/sidvoice.cpp src/patches.cpp \
	    $(addprefix $(RESID)/, $(RESID_SOURCES))
	./build/test/render all build/test

.PHONY: test
