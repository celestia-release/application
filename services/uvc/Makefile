#
# Makefile for anyka
# May 2026
#


include $(COMPILATION_RULES)
SRC_DIR ?= .
INSTALL_PATH = $(APPLICATION_INSTALL_DIR)

PROJ = uvc_server


MODULE_ROOT ?= $(shell pwd)


include $(PROJECT_CONFIG_FILE)
include $(COMPILATION_RULES)

BUILD_DIR := $(MODULE_ROOT)/build
BIN_DIR :=  $(BUILD_DIR)/bin


SRC_FILES :=
H_FILES :=
SRC_SUBDIRS :=

SRC_FILES += uvc_common.c uvc_server.c uvc_video.c

#################
#build
##################

BUILD_DIR := $(MODULE_ROOT)/build



# 生成输出文件列表

OUT_SRC_FILES := $(filter-out "", $(patsubst $(SRC_DIR)/%.c, $(MODULE_ROOT)/%.c, $(SRC_FILES)))
OUT_H_FILES := $(filter-out "", $(patsubst $(SRC_DIR)/%.h, $(MODULE_ROOT)/%.h, $(H_FILES)))

OUT_SRC_FILES_REL := $(patsubst $(MODULE_ROOT)/%.c, %.c, $(OUT_SRC_FILES))
OUT_H_FILES_REL := $(patsubst $(MODULE_ROOT)/%.h, %.h, $(OUT_H_FILES))

$(shell mkdir -p $(BUILD_DIR))
$(shell mkdir -p $(BIN_DIR))

TARGET :=
STATIC_LIB := $(BUILD_DIR)/lib$(PROJ).a
TARGET += $(STATIC_LIB)

SHARED_LIB = $(BUILD_DIR)/lib$(PROJ).so
TARGET += $(SHARED_LIB)

# 生成目标对象文件列表

OBJS := $(patsubst %.c, %.o, $(OUT_SRC_FILES_REL))

# 包含路径
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/cjson/include
INCLUDE += -I$(THIRD_PARTY_INSTALL_DIR)/zxing/include
INCLUDE += -I$(HARDWARE_INSTALL_DIR)/include
INCLUDE += -I$(SYSTEM_CORE_INSTALL_DIR)/include

INCLUDE += $(patsubst %, -I$(MODULE_ROOT)/%, $(SRC_SUBDIRS))
INCLUDE += -I$(MODULE_ROOT)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/multimedia
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/network
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/system
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/notification
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include
INCLUDE += -I$(FRAMEWORK_INSTALL_DIR)/include/services/multimedia
INCLUDE += -I$(SRC_APPLICATION_DIR)/app_main
INCLUDE += -I$(SRC_APPLICATION_DIR)/rtsp/rtsp_server_app
# project config: hand-written headers + auto-generated config.h (out/<product>/build/project).
PROJECT_GEN_INCLUDE_DIR ?= $(dir $(PROJECT_CONFIG_FILE))include
INCLUDE += -I$(dir $(PROJECT_CONFIG_FILE))include
INCLUDE += -I$(PROJECT_GEN_INCLUDE_DIR)

# 编译选项
CFLAGS += -Werror -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
CFLAGS += $(INCLUDE)

define copy_file
	$(foreach file,$1, $(shell mkdir -p $3/$(dir $(file)) && cp $2/$(file) $3/$(file)))
endef

define copy_h_file
	$(foreach file,$1, $(shell mkdir -p $3/$(dir $(file)) && cp $2/$(file) $3/$(file)))
endef

##build rule
.PHONY: all install clean copy

#RES := $(call copy_file, $(OUT_SRC_FILES_REL), $(SRC_DIR), $(MODULE_ROOT))

all: copy_src $(TARGET) install
$(STATIC_LIB):$(OBJS)
	$(AR) -rsv $(STATIC_LIB) $(OBJS)

$(SHARED_LIB):$(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -shared -fPIC -o $@ $(LDFLAGS)

$(OBJS):%.o:%.c
	$(CC) $(CFLAGS) -c $(SRC_DIR)/$< -o $(MODULE_ROOT)/$@

copy_src:
	$(call copy_file, $(OUT_SRC_FILES_REL), $(SRC_DIR), $(MODULE_ROOT))
	$(call copy_h_file, $(OUT_H_FILES_REL), $(SRC_DIR), $(MODULE_ROOT))
	cp uvc_server.h $(APPLICATION_INSTALL_DIR)/include/


install: $(TARGET)
	mkdir -p $(INSTALL_PATH)/lib
	cp $(TARGET) $(INSTALL_PATH)/lib

clean:
	$(RM) $(MODULE_ROOT)


