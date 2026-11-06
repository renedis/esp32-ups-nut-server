# ESP32 UPS NUT Server — Copyright (c) 2026 renedis — GPL-3.0
# Host unit tests for the pure-C modules: builds every test_<x>.c against
# ../../main/<x>.c and runs it. Usage: make -C tests/host
CC     ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -g
MAIN   := ../../main
TESTS  := $(patsubst %.c,%,$(wildcard test_*.c))

# macOS: when the Command Line Tools SDK is newer than the Xcode linker
# ("unknown architecture"), link against the Xcode SDK instead.
XCODE_SDK := /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk
ifneq ($(wildcard $(XCODE_SDK)),)
export SDKROOT ?= $(XCODE_SDK)
endif

all: test

test_%: test_%.c $(MAIN)/%.c check.h
	$(CC) $(CFLAGS) -I$(MAIN) -o $@ $< $(MAIN)/$*.c

test: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

clean:
	rm -f $(TESTS)
	rm -rf *.dSYM

.PHONY: all test clean
