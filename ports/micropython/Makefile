# MicroPython for NucleoOS. Driven by build.sh (TOP = the patched upstream tree).
#   make TOP=... NATIVE=1           PC build for the upstream test suite (setjmp-emulated host)
#   make TOP=... WASI_SDK=...       apps/python/app.wasm (wasm32-wasip1)
TOP ?= ../_src/micropython-1.26.1
ifeq ($(NATIVE),1)
BUILD = build-native
else
BUILD = build-wasi
endif
include $(TOP)/py/mkenv.mk

QSTR_DEFS = qstrdefsport.h
MICROPY_ROM_TEXT_COMPRESSION ?= 1
FROZEN_MANIFEST ?= $(CURDIR)/manifest.py
include $(TOP)/py/py.mk
include $(TOP)/extmod/extmod.mk

ifeq ($(NATIVE),1)
PROG = micropython
INC += -Inative
CFLAGS += -O1 -g
SRC_C += native/nv_try_native.c
SRC_SHARED += shared/runtime/gchelper_generic.c
else
PROG = app.wasm
SIZE = true
STRIP =
CC = $(WASI_SDK)/bin/clang
CFLAGS += --target=wasm32-wasip1 --sysroot=$(WASI_SDK)/share/wasi-sysroot -Os \
	-D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS -DNDEBUG
INC += -I../common
LDFLAGS += --target=wasm32-wasip1 --sysroot=$(WASI_SDK)/share/wasi-sysroot \
	-Wl,-z,stack-size=262144 -Wl,--strip-all -lwasi-emulated-signal -lwasi-emulated-process-clocks
endif

INC += -I. -I$(TOP) -I$(BUILD)
CFLAGS += $(INC) -Wall -Werror -std=gnu99 -Wno-unused-label -fdata-sections -ffunction-sections \
	$(CFLAGS_EXTMOD) $(COPT)

SRC_C += main.c nlr_nv.c
SRC_SHARED += $(addprefix shared/,\
	runtime/stdout_helpers.c \
	timeutils/timeutils.c \
	)
SRC_QSTR += $(SRC_C) $(SRC_SHARED)
OBJ += $(PY_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_SHARED:.c=.o))

LDFLAGS += -lm

include $(TOP)/py/mkrules.mk
