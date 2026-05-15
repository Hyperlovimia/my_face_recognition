PROJECT_ROOT := $(abspath ..)
SDK_ROOT ?= $(abspath $(PROJECT_ROOT)/../../../../)
TOOLCHAIN_ROOT ?= $(SDK_ROOT)/toolchain/Xuantie-900-gcc-linux-5.10.4-glibc-x86_64-V2.6.0/bin
CROSS_COMPILE ?= $(TOOLCHAIN_ROOT)/riscv64-unknown-linux-gnu-
STATIC ?= 0
DEBUG ?= 0
DO_STRIP ?= 1

CC := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++
AR := $(CROSS_COMPILE)ar
STRIP := $(CROSS_COMPILE)strip

IPCMSG_ROOT := $(SDK_ROOT)/src/common/cdk/user/component/ipcmsg
IPCMSG_LIB := $(IPCMSG_ROOT)/host/lib/libipcmsg.a
MPP_INCLUDE := $(SDK_ROOT)/src/big/mpp/include
UI_DIR := $(PROJECT_ROOT)/linux_bridge/ui
LIBDISP_ROOT := $(SDK_ROOT)/src/little/buildroot-ext/package/libdisp/src
DOORLOCK_K230_ROOT := $(SDK_ROOT)/src/little/buildroot-ext/package/door_lock/src/ui/lvgl_port/k230
LITTLE_OUTPUT_ROOT ?= $(SDK_ROOT)/output/k230_canmv_dongshanpi_defconfig/little/buildroot-ext
LVGL_ROOT ?= $(LITTLE_OUTPUT_ROOT)/build/lvgl-v8.3.7
LVGL_SRC_DIR := $(LVGL_ROOT)/lvgl/src
LITTLE_STAGING_INCLUDE := $(LITTLE_OUTPUT_ROOT)/staging/usr/include
LITTLE_STAGING_LIB := $(LITTLE_OUTPUT_ROOT)/staging/usr/lib
LIBDRM_SO := $(LITTLE_STAGING_LIB)/libdrm.so

OUT_DIR := $(PROJECT_ROOT)/linux_bridge/out
TARGET := $(OUT_DIR)/face_netd

ifeq ($(DEBUG),1)
OPT_FLAGS := -O0 -g3 -fno-omit-frame-pointer
DO_STRIP := 0
else
OPT_FLAGS := -O2
endif

CFLAGS := $(OPT_FLAGS) -pipe -ffunction-sections -fdata-sections
CXXFLAGS := $(CFLAGS) -std=c++17
CPPFLAGS := -DLV_CONF_INCLUDE_SIMPLE \
	\
	-I$(PROJECT_ROOT)/src \
	-I$(PROJECT_ROOT)/linux_bridge \
	-I$(UI_DIR) \
	-I$(PROJECT_ROOT)/linux_bridge/third_party/mongoose \
	-I$(IPCMSG_ROOT)/include \
	-I$(MPP_INCLUDE) \
	-I$(LVGL_ROOT)/lvgl \
	-I$(LITTLE_STAGING_INCLUDE) \
	-I$(LITTLE_STAGING_INCLUDE)/libdrm \
	-I$(LIBDISP_ROOT)
LDFLAGS := -Wl,--gc-sections
LDLIBS := $(IPCMSG_LIB) $(LIBDRM_SO) -lpthread -latomic -lm

ifeq ($(STATIC),1)
CFLAGS += -static
LDFLAGS += -static
endif

LVGL_SRCS := $(shell find $(LVGL_SRC_DIR) -name '*.c')
LVGL_OBJS := $(patsubst $(LVGL_SRC_DIR)/%.c,$(OUT_DIR)/lvgl/%.o,$(LVGL_SRCS))

OBJS := \
	$(OUT_DIR)/main.o \
	$(OUT_DIR)/mongoose.o \
	$(OUT_DIR)/ui_runtime.o \
	$(OUT_DIR)/k230_port.o \
	$(OUT_DIR)/buf_mgt.o \
	$(OUT_DIR)/disp.o \
	$(LVGL_OBJS)

.PHONY: all clean

all: $(TARGET)

$(OUT_DIR):
	mkdir -p $@

$(OUT_DIR)/main.o: $(PROJECT_ROOT)/linux_bridge/main.cpp | $(OUT_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OUT_DIR)/mongoose.o: $(PROJECT_ROOT)/linux_bridge/third_party/mongoose/mongoose.c | $(OUT_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/ui_runtime.o: $(PROJECT_ROOT)/linux_bridge/ui/ui_runtime.cpp | $(OUT_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OUT_DIR)/k230_port.o: $(PROJECT_ROOT)/linux_bridge/ui/k230_port.cpp | $(OUT_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OUT_DIR)/buf_mgt.o: $(DOORLOCK_K230_ROOT)/buf_mgt.cpp | $(OUT_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OUT_DIR)/disp.o: $(LIBDISP_ROOT)/disp.c | $(OUT_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/lvgl/%.o: $(LVGL_SRC_DIR)/%.c
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DLV_CONF_INCLUDE_SIMPLE -c $< -o $@

$(TARGET): $(OBJS) $(IPCMSG_LIB)
	$(CXX) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

ifneq ($(DO_STRIP),0)
	$(STRIP) $@
endif

clean:
	rm -rf $(OUT_DIR)
