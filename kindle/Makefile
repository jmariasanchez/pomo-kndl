# Host builds (Mac/Linux): make test, make dev
# Kindle build: make kindle CROSS=arm-kindlehf-linux-gnueabihf- SYSROOT=...  (see README)
CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
ifneq ($(OPENSSL_PREFIX),)
SSL_CFLAGS = -I$(OPENSSL_PREFIX)/include
SSL_LIBS = -L$(OPENSSL_PREFIX)/lib -lssl -lcrypto
else
SSL_LIBS = -lssl -lcrypto
endif

BUILD = build
CORE = src/core.cpp
NET = src/net.cpp

all: test dev

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)
	$(CXX) $(CXXFLAGS) -o $(BUILD)/core_test tests/core_test.cpp $(CORE)
	./$(BUILD)/core_test

dev: $(BUILD)
	$(CXX) $(CXXFLAGS) $(SSL_CFLAGS) -o $(BUILD)/pomo-kndl-dev src/dev_main.cpp $(CORE) $(NET) $(SSL_LIBS)

# GTK 2 UI. Needs gtk+-2.0 via pkg-config (on the Kindle toolchain sysroot).
gui: $(BUILD)
	$(CXX) $(CXXFLAGS) $(SSL_CFLAGS) $(shell pkg-config --cflags gtk+-2.0) -o $(BUILD)/pomo-kndl-gui \
	  src/gui_main.cpp $(CORE) $(NET) $(shell pkg-config --libs gtk+-2.0) $(SSL_LIBS)

# Cross build for the Kindle (armhf). Example:
#   make kindle CROSS=arm-kindlehf-linux-gnueabihf- SYSROOT=$$HOME/x-tools/arm-kindlehf-linux-gnueabihf/arm-kindlehf-linux-gnueabihf/sysroot
kindle:
	$(MAKE) gui CXX=$(CROSS)g++ OPENSSL_PREFIX= \
	  PKG_CONFIG_SYSROOT_DIR=$(SYSROOT) PKG_CONFIG_LIBDIR=$(SYSROOT)/usr/lib/pkgconfig BUILD=build-kindle

package: kindle
	rm -rf dist && mkdir -p dist/pomo-kndl/bin
	cp build-kindle/pomo-kndl-gui dist/pomo-kndl/bin/
	cp kual/config.xml kual/menu.json kual/run.sh dist/pomo-kndl/
	cd dist && tar czf pomo-kndl-kual.tgz pomo-kndl

clean:
	rm -rf build build-kindle dist

.PHONY: all test dev gui kindle package clean
