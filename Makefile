# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS += \
    -std=c++17 \
	-I$(CURDIR)/src \
	-I$(CURDIR)/vendor/oscpack

# platform detection
OS := $(shell uname -s)

SOURCES += $(wildcard src/*.cpp)

SOURCES += $(wildcard vendor/oscpack/ip/*.cpp)
SOURCES += $(wildcard vendor/oscpack/osc/*.cpp)

# add platform-specific oscpack sources and linker flags
ifeq ($(OS), Darwin)
	SOURCES += $(wildcard vendor/oscpack/ip/posix/*.cpp)
else ifeq ($(OS), Linux)
	SOURCES += $(wildcard vendor/oscpack/ip/posix/*.cpp)
	LDFLAGS += -lpthread
else ifneq (,$(findstring MINGW,$(OS)))
	SOURCES += $(wildcard vendor/oscpack/ip/win32/*.cpp)
	LDFLAGS += -lws2_32 -liphlpapi -lwinmm
	CXXFLAGS += -DWIN32 -D_WINDOWS -DWIN32_LEAN_AND_MEAN -DNOMINMAX
else ifneq (,$(findstring CYGWIN,$(OS)))
	   SOURCES += $(wildcard vendor/oscpack/ip/posix/*.cpp)
	   LDFLAGS += -lpthread
else ifeq ($(OS), Windows_NT)
	SOURCES += $(wildcard vendor/oscpack/ip/win32/*.cpp)
	LDFLAGS += -lws2_32 -liphlpapi -lwinmm
	CXXFLAGS += -DWIN32 -D_WINDOWS -DWIN32_LEAN_AND_MEAN -DNOMINMAX
else
	$(warning "Unsupported OS detected: $(OS). Assuming POSIX networking.")
	SOURCES += $(wildcard vendor/oscpack/ip/posix/*.cpp)
endif

# SOURCES += $(wildcard vendor/aoo/src/codec/opus.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

include $(RACK_DIR)/plugin.mk

CXXFLAGS := $(filter-out -std=c++11, $(CXXFLAGS))