# Phase 1 Verilator session.
#   make smoke                  -> build/sim
#   EMU_TRACE=0 make smoke      -> build/sim-off
#   EMU_TRACE=fst make smoke    -> build/sim-fst
#   make check

# SV module name. The generated C++ class stays VTop via --prefix.
TOP ?= SimTop
RTL ?= tb/SmokeTop.sv
PREFIX := VTop
BUILD_DIR := build
EMU_TRACE ?= 1
JOBS ?= $(shell nproc)
VERILATOR ?= verilator

ifeq ($(EMU_TRACE),fst)
TRACE_ID := fst
TRACE_VERILATOR := --trace-fst
TRACE_CXX := -DEMU_TRACE_FST
else ifeq ($(EMU_TRACE),FST)
TRACE_ID := fst
TRACE_VERILATOR := --trace-fst
TRACE_CXX := -DEMU_TRACE_FST
else ifneq ($(filter 1 vcd VCD,$(EMU_TRACE)),)
TRACE_ID := vcd
TRACE_VERILATOR := --trace
TRACE_CXX := -DEMU_TRACE_VCD
else
TRACE_ID := off
TRACE_VERILATOR :=
TRACE_CXX :=
endif

OBJ_DIR := $(BUILD_DIR)/obj-$(TRACE_ID)
ifeq ($(TRACE_ID),vcd)
BIN := $(BUILD_DIR)/sim
else
BIN := $(BUILD_DIR)/sim-$(TRACE_ID)
endif

CSRC := $(wildcard csrc/session/*.cpp) $(wildcard csrc/platform/*.cpp)
HEADERS := $(wildcard csrc/session/*.h) $(wildcard csrc/platform/*.h) $(wildcard csrc/service/*.h)
ABS_CSRC := $(abspath $(CSRC))

.PHONY: smoke check clean
.PHONY: compile-commands

smoke: $(BIN)

$(OBJ_DIR)/$(PREFIX).mk: $(RTL) $(CSRC) $(HEADERS)
	mkdir -p $(OBJ_DIR)
	$(VERILATOR) --cc --exe --top-module $(TOP) --prefix $(PREFIX) \
	  --Mdir $(OBJ_DIR) \
	  -o $(abspath $(BIN)) \
	  $(TRACE_VERILATOR) \
	  --assert \
	  -Wno-WIDTH -Wno-DECLFILENAME \
	  -CFLAGS "-I$(abspath csrc) $(TRACE_CXX)" \
	  $(abspath $(RTL)) $(ABS_CSRC)

$(BIN): $(OBJ_DIR)/$(PREFIX).mk $(CSRC) $(HEADERS)
	$(MAKE) -C $(OBJ_DIR) -f $(PREFIX).mk -j$(JOBS) OBJCACHE=

check:
	$(MAKE) smoke EMU_TRACE=1
	$(MAKE) smoke EMU_TRACE=0
	./scripts/check_phase1.sh

compile-commands: $(OBJ_DIR)/$(PREFIX).mk
	bear --output $(abspath compile_commands.json) -- $(MAKE) -C $(OBJ_DIR) -f $(PREFIX).mk -B OBJCACHE=
	python3 scripts/use_path_compiler.py $(abspath compile_commands.json)

clean:
	rm -rf $(BUILD_DIR)
