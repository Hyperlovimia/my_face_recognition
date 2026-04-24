PROJECT_ROOT := $(abspath ..)
SDK_ROOT ?= $(abspath $(PROJECT_ROOT)/../../../../)
TOOLCHAIN_ROOT ?= $(SDK_ROOT)/toolchain/riscv64-linux-musleabi_for_x86_64-pc-linux-gnu/bin
CROSS_COMPILE ?= $(TOOLCHAIN_ROOT)/riscv64-unknown-linux-musl-
CC := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++
AR := $(CROSS_COMPILE)ar
STRIP := $(CROSS_COMPILE)strip

IPCMSG_ROOT := $(SDK_ROOT)/src/common/cdk/user/component/ipcmsg
IPCMSG_LIB := $(IPCMSG_ROOT)/host/lib/libipcmsg.a
MPP_INCLUDE := $(SDK_ROOT)/src/big/mpp/include

OUT_DIR := $(PROJECT_ROOT)/linux_bridge/out
TARGET := $(OUT_DIR)/face_netd

CFLAGS := -O2 -pipe -static -ffunction-sections -fdata-sections
CXXFLAGS := $(CFLAGS) -std=c++17
CPPFLAGS := \
	-I$(PROJECT_ROOT)/src \
	-I$(PROJECT_ROOT)/linux_bridge \
	-I$(PROJECT_ROOT)/linux_bridge/third_party/mongoose \
	-I$(IPCMSG_ROOT)/include \
	-I$(MPP_INCLUDE)
LDFLAGS := -static -Wl,--gc-sections
LDLIBS := $(IPCMSG_LIB) -lpthread -latomic

OBJS := $(OUT_DIR)/main.o $(OUT_DIR)/mongoose.o

.PHONY: all clean

all: $(TARGET)

$(OUT_DIR):
	mkdir -p $@

$(OUT_DIR)/main.o: $(PROJECT_ROOT)/linux_bridge/main.cpp | $(OUT_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(OUT_DIR)/mongoose.o: $(PROJECT_ROOT)/linux_bridge/third_party/mongoose/mongoose.c | $(OUT_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(TARGET): $(OBJS) $(IPCMSG_LIB)
	$(CXX) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@
	$(STRIP) $@

clean:
	rm -rf $(OUT_DIR)
