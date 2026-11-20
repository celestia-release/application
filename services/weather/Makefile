#
# Makefile for application/services/weather module
#
include $(COMPILATION_RULES)
include $(PROJECT_CONFIG_FILE)

PROJ = weather_service
MODULE_ROOT ?= $(shell pwd)
WEATHER_SRC_DIR := $(MODULE_ROOT)/src
BUILD_DIR ?= $(MODULE_ROOT)/build

CSRCS = $(wildcard $(WEATHER_SRC_DIR)/*.c)
OBJS := $(patsubst $(WEATHER_SRC_DIR)/%.c,$(BUILD_DIR)/src/%.o,$(CSRCS))

STATIC_LIB := lib$(PROJ).a
DYNAMIC_LIB := lib$(PROJ).so
TARGET := $(BUILD_DIR)/$(STATIC_LIB)
TARGET_SO := $(BUILD_DIR)/$(DYNAMIC_LIB)

INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include
INCLUDE += -I$(PROJECT_DIR)/include
PROJECT_GEN_INCLUDE_DIR ?= $(PROJECT_DIR)/include
INCLUDE += -I$(PROJECT_GEN_INCLUDE_DIR)
INCLUDE += -I$(MODULE_ROOT)/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/curl/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/curl/include/curl
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include

CFLAGS += $(INCLUDE)

ifeq ($(CONFIG_FW_NETWORK), y)
CFLAGS += -DCONFIG_NETWORK_SUPPORTED=1
endif

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
# DT_NEEDED libcurl (+ mbedtls via curl). Loaded only when ipc_main stub dlopens this .so.
WEATHER_SHLIB_LIBS := -Wl,-rpath,/usr/lib \
	-Wl,--no-as-needed \
	-L$(THIRD_PARTY_INSTALL_DIR)/curl/lib -lcurl \
	-L$(THIRD_PARTY_INSTALL_DIR)/cjson/lib -lcjson \
	-Wl,--as-needed
$(TARGET_SO): $(OBJS)
	@mkdir -p $(dir $(TARGET_SO))
	$(CC) $(CFLAGS) $(OBJS) -shared -fPIC -o $@ $(LDFLAGS) $(WEATHER_SHLIB_LIBS)
else
$(TARGET_SO): $(OBJS)
	@mkdir -p $(dir $(TARGET_SO))
	$(CC) $(CFLAGS) $(OBJS) -shared -fPIC -o $@ $(LDFLAGS)
endif

$(BUILD_DIR)/src/%.o: $(WEATHER_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

install:
	@mkdir -p $(APPLICATION_INSTALL_DIR)/include/services/weather
	@mkdir -p $(APPLICATION_INSTALL_DIR)/lib
	@cp -f $(BUILD_DIR)/*.a $(APPLICATION_INSTALL_DIR)/lib 2>/dev/null || true
	@cp -f $(BUILD_DIR)/*.so $(APPLICATION_INSTALL_DIR)/lib 2>/dev/null || true
	@cp $(MODULE_ROOT)/include/*.h $(APPLICATION_INSTALL_DIR)/include/services/weather/

clean:
	$(RM) -rf $(STATIC_LIB)
	$(RM) -rf $(OBJS)
	$(RM) -rf $(BUILD_DIR)
