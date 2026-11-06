# Host build (Linux/macOS/WSL): unit tests, dataset generator, offline evaluation.
# QNX build: see scripts/build_qnx.sh (uses qcc from QNX SDP 8.0).
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L
INC     := -Iinclude -Isrc/core
CORE    := src/core/sim.c src/core/features.c src/core/model.c src/core/supervisor.c
BIN     := build/host

all: $(BIN)/gen_dataset $(BIN)/evaluate $(BIN)/test_core

$(BIN):
	mkdir -p $@

$(BIN)/%: tools/%.c $(CORE) include/qs_core.h src/core/model_weights.h | $(BIN)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(CORE) -lm

$(BIN)/test_core: tests/test_core.c $(CORE) include/qs_core.h | $(BIN)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(CORE) -lm

test: $(BIN)/test_core
	$(BIN)/test_core

model: $(BIN)/gen_dataset
	$(BIN)/gen_dataset 600 1 > build/train.csv
	$(BIN)/gen_dataset 150 2 > build/val.csv
	python3 train/train.py build/train.csv build/val.csv src/core/model_weights.h

eval: $(BIN)/evaluate
	$(BIN)/evaluate 50 2027

clean:
	rm -rf build

.PHONY: all test model eval clean

# ---------------------------------------------------------------------------
# QNX 8.0 cross build (run after: source ~/qnx800/qnxsdp-env.sh)
#   make qnx                 -> build/qnx-x86_64/   (QEMU x86_64 target)
#   make qnx QNX_ARCH=aarch64le                     (Raspberry Pi 4/5, QEMU aarch64)
QNX_ARCH ?= x86_64
QCC      ?= qcc
QFLAGS   := -Vgcc_nto$(QNX_ARCH) -O2 -std=gnu11 -Wall -Wextra
QOUT     := build/qnx-$(QNX_ARCH)
QINC     := $(INC) -Isrc/qnx

qnx: $(QOUT)/qsentryd $(QOUT)/qsensord $(QOUT)/qsentry-top $(QOUT)/qsload

$(QOUT):
	mkdir -p $@

$(QOUT)/qsentryd: src/qnx/qsentryd.c src/qnx/qs_proto.h $(CORE) | $(QOUT)
	$(QCC) $(QFLAGS) $(QINC) -o $@ $< $(CORE) -lm

$(QOUT)/qsensord: src/qnx/qsensord.c src/qnx/qs_proto.h $(CORE) | $(QOUT)
	$(QCC) $(QFLAGS) $(QINC) -o $@ $< $(CORE) -lm

$(QOUT)/qsentry-top: src/qnx/qsentry_top.c | $(QOUT)
	$(QCC) $(QFLAGS) -o $@ $<

$(QOUT)/qsload: src/qnx/qsload.c | $(QOUT)
	$(QCC) $(QFLAGS) -o $@ $<

.PHONY: qnx
