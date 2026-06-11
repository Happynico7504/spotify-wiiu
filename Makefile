.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/wut/share/wut_rules

#-------------------------------------------------------------------------------
# App metadata
#-------------------------------------------------------------------------------
TARGET        := spotify-wiiu
BUILD         := build
DATA          :=

APP_NAME       := Spotify Wii U
APP_SHORTNAME  := Spotify
APP_AUTHOR     := WiiU Homebrew
APP_CONTENT    := content
APP_ICON       := meta/icon.png

#-------------------------------------------------------------------------------
# Tremor (libvorbisidec) — portlib if installed, vendor source otherwise
#-------------------------------------------------------------------------------
TREMOR_HDR := $(firstword $(wildcard $(patsubst %,%/include/tremor/ivorbisfile.h,$(PORTLIBS))))

ifeq ($(TREMOR_HDR),)
  $(info Tremor: portlib not found — building from vendor/tremor/)
  TREMOR_SOURCES := vendor/tremor
  TREMOR_LIB     :=
else
  $(info Tremor: using portlib at $(PORTLIBS))
  TREMOR_SOURCES :=
  TREMOR_LIB     := -lvorbisidec
endif

#-------------------------------------------------------------------------------
# Sources
#-------------------------------------------------------------------------------
SOURCES := src               \
           src/connect       \
           src/discovery     \
           src/ui            \
           vendor/cJSON      \
           $(TREMOR_SOURCES)

INCLUDES := src vendor

#-------------------------------------------------------------------------------
# Build flags
#-------------------------------------------------------------------------------
CFLAGS   := -g -Wall -Wextra -O2 $(MACHDEP)     \
            -Wno-unused-parameter                 \
            -Wno-missing-field-initializers       \
            -Wno-implicit-fallthrough             \
            -Wno-maybe-uninitialized

CXXFLAGS := $(CFLAGS) -std=c++17 -fexceptions -fno-rtti

ASFLAGS  := $(ARCH)
LDFLAGS   = $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

LIBS     := -lSDL2_ttf      \
            -lharfbuzz      \
            -lfreetype      \
            -lpng16         \
            -lbz2           \
            -lSDL2          \
            -lcurl          \
            -lbrotlidec     \
            -lbrotlicommon  \
            -lmbedtls       \
            -lmbedcrypto    \
            -lmbedx509      \
            -lz             \
            $(TREMOR_LIB)   \
            -logg           \
            -lwutd          \
            -lwut

LIBDIRS  := $(WUT_ROOT) $(PORTLIBS)

#-------------------------------------------------------------------------------
# Two-phase devkitPro build
#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(TOPDIR)/$(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(TOPDIR)/$(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(TOPDIR)/$(dir)/*.s)))

export OFILES  := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include)    \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export VPATH := $(foreach dir,$(SOURCES),$(TOPDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(TOPDIR)/$(dir))

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).rpx $(TARGET).wuhb

else

CPPFLAGS := $(INCLUDE)
LD       := $(CXX)
DEPENDS  := $(OFILES:.o=.d)

$(OUTPUT).wuhb : $(OUTPUT).rpx
$(OUTPUT).rpx  : $(OUTPUT).elf
$(OUTPUT).elf  : $(OFILES)

-include $(DEPENDS)

endif
