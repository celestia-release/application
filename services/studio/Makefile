#
# Makefile for application/services/studio module
#
include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

PROJ = studio
MODULE_ROOT ?= $(shell pwd)
# Do not name this SRC_DIR: application/Makefile passes SRC_DIR=<module root>
# on the make command line, which overrides a makefile assignment and makes
# $(wildcard) miss src/*.c (empty libstudio.so).
STUDIO_SRC_DIR := $(MODULE_ROOT)/src
BUILD_DIR ?= $(MODULE_ROOT)/build

CSRCS = $(wildcard $(STUDIO_SRC_DIR)/*.c)
OBJS := $(patsubst $(STUDIO_SRC_DIR)/%.c,$(BUILD_DIR)/src/%.o,$(CSRCS))

STATIC_LIB := lib$(PROJ).a
DYNAMIC_LIB := lib$(PROJ).so
TARGET := $(BUILD_DIR)/$(STATIC_LIB)
TARGET_SO := $(BUILD_DIR)/$(DYNAMIC_LIB)

INCLUDE += -I$(MODULE_ROOT)/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/jpeg/include

CFLAGS += $(INCLUDE)

ifeq ($(CONFIG_OS_NAME), linux)
CFLAGS += -fPIC
endif

$(shell mkdir -p $(BUILD_DIR))

.PHONY: all target clean install

all: target install

target: $(TARGET) $(TARGET_SO)

$(TARGET): $(OBJS)
	@mkdir -p $(dir $(TARGET))
	$(AR) -rsv $(TARGET) $(OBJS)

ifeq ($(CONFIG_OS_NAME), linux)
# DT_NEEDED libcjson + libjpeg: both .so are deployed to /usr/lib via
# CONFIG_THIRD_PARTY_SO_ALLOWLIST (cjson) and the jpeg entry added to it.
STUDIO_SHLIB_LIBS := -Wl,-rpath,/usr/lib \
	-L$(THIRD_PARTY_INSTALL_DIR)/cjson/lib -lcjson \
	-L$(THIRD_PARTY_INSTALL_DIR)/jpeg/lib -ljpeg
$(TARGET_SO): $(OBJS)
	@mkdir -p $(dir $(TARGET_SO))
	$(CC) $(CFLAGS) $(OBJS) -shared -fPIC -o $@ $(LDFLAGS) $(STUDIO_SHLIB_LIBS)
else
$(TARGET_SO): $(OBJS)
	@mkdir -p $(dir $(TARGET_SO))
	$(CC) $(CFLAGS) $(OBJS) -shared -fPIC -o $@ $(LDFLAGS)
endif

$(BUILD_DIR)/src/%.o: $(STUDIO_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

install:
	@mkdir -p $(APPLICATION_INSTALL_DIR)/lib
	@cp -f $(BUILD_DIR)/*.a $(APPLICATION_INSTALL_DIR)/lib 2>/dev/null || true
	@cp -f $(BUILD_DIR)/*.so $(APPLICATION_INSTALL_DIR)/lib 2>/dev/null || true

clean:
	$(RM) -rf $(BUILD_DIR)
