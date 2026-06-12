.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

WUPS_ROOT ?= $(DEVKITPRO)/wups
include $(WUPS_ROOT)/share/wups_rules

TARGET   := spotify-cache-sweep
BUILD    := build
SOURCES  := src

CFLAGS   := -Wall -O2 $(INCLUDE)
CXXFLAGS := $(CFLAGS) -std=c++20
LDFLAGS  := $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS := -lwups -lwut -lwhb

all: $(TARGET).wps

$(TARGET).wps: $(BUILD)
	@[ -d $@ ] || $(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

.PHONY: clean
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).wps
