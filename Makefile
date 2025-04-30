# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

# FLAGS will be passed to both the C and C++ compiler
FLAGS +=
CFLAGS +=
CXXFLAGS += \
    -std=c++17 \
	-I$(CURDIR)/src \
	-I$(CURDIR)/vendor/oscpack \

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
OPUS_PATH := $(shell brew --prefix opus)
LDFLAGS += -L$(OPUS_PATH)/lib -lopus 


# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)
# Add sources for oscpack
SOURCES += $(wildcard vendor/oscpack/ip/posix/*.cpp)
SOURCES += $(wildcard vendor/oscpack/ip/*.cpp) 
SOURCES += $(wildcard vendor/oscpack/osc/*.cpp)
# SOURCES += $(wildcard vendor/aoo/src/codec/opus.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

CXXFLAGS := $(filter-out -std=c++11, $(CXXFLAGS))
