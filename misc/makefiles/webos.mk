#-----------------------------
# Configurable flags and names
#-----------------------------
# Cross-compilation SDK paths (override with environment variables if needed)
SDK_DIR ?= /run/media/1TB/Dev-Projects/Porting/Tools/native-toolchain-compiled/arm-webos-linux-gnueabi_sdk-buildroot
STAGING_DIR ?= $(SDK_DIR)/arm-webos-linux-gnueabi/sysroot
CROSS_PREFIX ?= $(SDK_DIR)/bin/arm-webos-linux-gnueabi-

SOURCE_DIRS := src src/webos third_party/bearssl
BUILD_DIR	:= build/webos

CC := $(CROSS_PREFIX)gcc
CFLAGS  := -fvisibility=hidden -fno-ident -DCC_BUILD_WEBOS \
           -mcpu=cortex-a9 -mtune=cortex-a53 -mfloat-abi=softfp -mfpu=neon -O2 \
           -I$(STAGING_DIR)/usr/include -I$(STAGING_DIR)/usr/include/SDL2
LDFLAGS := -rdynamic -L$(STAGING_DIR)/usr/lib -L$(STAGING_DIR)/lib
LIBS 	:= -lSDL2 -lGLESv2 -lpthread -ldl -lm
include misc/makefiles/common_config.mk


include misc/makefiles/common_build.mk
