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
CPPFLAGS := \
	-I$(PROJECT_ROOT)/src \
	-I$(PROJECT_ROOT)/linux_bridge \
	-I$(PROJECT_ROOT)/linux_bridge/third_party/mongoose \
	-I$(IPCMSG_ROOT)/include \
	-I$(MPP_INCLUDE)
LDFLAGS := -Wl,--gc-sections
LDLIBS := $(IPCMSG_LIB) -lpthread -latomic

ifeq ($(STATIC),1)
CFLAGS += -static
LDFLAGS += -static
endif

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

ifneq ($(DO_STRIP),0)
	$(STRIP) $@
endif

clean:
	rm -rf $(OUT_DIR)
