# Linux build: `make`, a C++20 compiler and the GStreamer development packages.
#
#   make            release build -> build/bin/st2022_testsignal
#   make -j         ... in parallel
#   make test       build and run the self-tests (no network, no GStreamer)
#   make install    to $(PREFIX)/bin, default /usr/local
#   make clean

CXX      ?= g++
PREFIX   ?= /usr/local
BUILD    ?= build
PCR      := third_party/pcapreplay

CXXSTD   := -std=c++20
WARN     := -Wall -Wextra
INCLUDES := -Isrc -I$(PCR)/common/include -I$(PCR)/nmos/include
CXXFLAGS ?= -O2 -g
# The C++ runtime is linked in, so the binary copies to any machine with the
# same glibc and the GStreamer runtime (libgstreamer1.0-0, gstreamer1.0-plugins-base)
# without a matching libstdc++. GStreamer itself cannot be linked statically: its
# elements, videotestsrc included, are plugins it loads at run time.
LDFLAGS  ?= -static-libstdc++ -static-libgcc

GST_PKGS := gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0
ifneq ($(MAKECMDGOALS),clean)
GST_CFLAGS := $(shell pkg-config --cflags $(GST_PKGS) 2>/dev/null)
GST_LIBS   := $(shell pkg-config --libs $(GST_PKGS) 2>/dev/null)
ifeq ($(strip $(GST_LIBS)),)
$(error GStreamer development packages not found -- install libgstreamer1.0-dev and libgstreamer-plugins-base1.0-dev, see README.md)
endif
endif

# The vendored PCAP Replay code: SDI raster, HBRMT, pacing, multicast, NMOS.
PCR_SRC := \
	$(PCR)/common/src/sdi_format.cpp \
	$(PCR)/common/src/bitpack.cpp \
	$(PCR)/common/src/crc.cpp \
	$(PCR)/common/src/sdi_raster.cpp \
	$(PCR)/common/src/hbrmt.cpp \
	$(PCR)/common/src/net_interfaces.cpp \
	$(PCR)/common/src/net_multicast.cpp \
	$(PCR)/common/src/pacer.cpp \
	$(PCR)/common/src/platform.cpp \
	$(PCR)/nmos/src/json.cpp \
	$(PCR)/nmos/src/http.cpp \
	$(PCR)/nmos/src/mdns.cpp \
	$(PCR)/nmos/src/mdns_posix.cpp \
	$(PCR)/nmos/src/uuid.cpp \
	$(PCR)/nmos/src/nmos_node.cpp \
	$(PCR)/nmos/src/status_text.cpp
AVX2_SRC := $(PCR)/common/src/bitpack_avx2.cpp

# Picture, audio and raster composition: everything the self-tests exercise.
LIB_SRC  := src/timecode.cpp src/overlay.cpp src/audio_embed.cpp src/composer.cpp
# Needs GStreamer and the network.
APP_SRC  := src/picture_source.cpp src/sender.cpp src/main.cpp
TEST_SRC := tests/test_main.cpp

# The delay probe: receive, locate, decode and correlate (testable) ...
PROBE_LIB_SRC := src/probe/marker_decode.cpp src/probe/st2022_receiver.cpp src/probe/analyzer.cpp
# ... and the DeckLink capture and command line.
PROBE_APP_SRC := src/probe/sdi_capture.cpp src/probe_main.cpp
PROBE_TEST_SRC := tests/test_probe.cpp

obj = $(patsubst %.cpp,$(BUILD)/obj/%.o,$(1))

PCR_OBJ  := $(call obj,$(PCR_SRC))
AVX2_OBJ := $(call obj,$(AVX2_SRC))
LIB_OBJ  := $(call obj,$(LIB_SRC))
APP_OBJ  := $(call obj,$(APP_SRC))
TEST_OBJ := $(call obj,$(TEST_SRC))
PROBE_LIB_OBJ  := $(call obj,$(PROBE_LIB_SRC))
PROBE_APP_OBJ  := $(call obj,$(PROBE_APP_SRC))
PROBE_TEST_OBJ := $(call obj,$(PROBE_TEST_SRC))
DEPS     := $(patsubst %.o,%.d,$(PCR_OBJ) $(AVX2_OBJ) $(LIB_OBJ) $(APP_OBJ) $(TEST_OBJ) \
              $(PROBE_LIB_OBJ) $(PROBE_APP_OBJ) $(PROBE_TEST_OBJ))

TARGET            := $(BUILD)/bin/st2022_testsignal
PROBE_TARGET      := $(BUILD)/bin/st2022_delayprobe
TEST_TARGET       := $(BUILD)/bin/testsignal_tests
PROBE_TEST_TARGET := $(BUILD)/bin/delayprobe_tests

.PHONY: all test clean install

all: $(TARGET) $(PROBE_TARGET)

$(TARGET): $(APP_OBJ) $(LIB_OBJ) $(PCR_OBJ) $(AVX2_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(GST_LIBS) -lpthread
	@echo "built $@"

$(PROBE_TARGET): $(PROBE_APP_OBJ) $(PROBE_LIB_OBJ) $(LIB_OBJ) $(PCR_OBJ) $(AVX2_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ $(GST_LIBS) -lpthread
	@echo "built $@"

$(TEST_TARGET): $(TEST_OBJ) $(LIB_OBJ) $(PCR_OBJ) $(AVX2_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ -lpthread

$(PROBE_TEST_TARGET): $(PROBE_TEST_OBJ) $(PROBE_LIB_OBJ) $(LIB_OBJ) $(PCR_OBJ) $(AVX2_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS) -o $@ $^ -lpthread

test: $(TEST_TARGET) $(PROBE_TEST_TARGET)
	$(TEST_TARGET)
	$(PROBE_TEST_TARGET)

$(BUILD)/obj/src/probe/sdi_capture.o: src/probe/sdi_capture.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(CXXFLAGS) $(INCLUDES) $(GST_CFLAGS) -MMD -MP -c $< -o $@

# Only this translation unit is built above baseline x86-64; bitpack.cpp probes
# for AVX2 at run time before calling into it.
$(AVX2_OBJ): $(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(CXXFLAGS) -mavx2 $(INCLUDES) -MMD -MP -c $< -o $@

$(BUILD)/obj/src/picture_source.o: src/picture_source.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(CXXFLAGS) $(INCLUDES) $(GST_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARN) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/st2022_testsignal
	@echo "installed $(DESTDIR)$(PREFIX)/bin/st2022_testsignal"
	@echo
	@echo "Optional, for real-time packet pacing without running as root:"
	@echo "    sudo setcap cap_sys_nice=eip $(DESTDIR)$(PREFIX)/bin/st2022_testsignal"

clean:
	rm -rf $(BUILD)

-include $(DEPS)
