# Verilator session.
#   make smoke RTL=...          -> build/sim
#   EMU_TRACE=0 make smoke      -> build/sim-off
#   EMU_TRACE=fst make smoke    -> build/sim-fst
#   make dram RTL=...           -> build/sim-dram, timed by third_party/DRAMsim3

# SV module name. The generated C++ class stays VTop via --prefix.
TOP ?= SimTop
PREFIX := VTop
BUILD_DIR := build
EMU_TRACE ?= 1
JOBS ?= $(shell nproc)
VERILATOR_EXTRA ?=
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

CSRC := $(wildcard csrc/session/*.cpp) $(wildcard csrc/platform/*.cpp) \
        $(wildcard csrc/service/*/*.cpp) \
        $(wildcard csrc/service/*/*/*.cpp)
HEADERS := $(wildcard csrc/session/*.h) $(wildcard csrc/platform/*.h) \
           $(wildcard csrc/service/*.h) $(wildcard csrc/service/*/*.h) \
           $(wildcard csrc/service/*/*/*.h)
ABS_CSRC := $(abspath $(CSRC))

WITH_DRAMSIM3 ?= 0
DRAMSIM3_DIR := third_party/DRAMsim3
DRAMSIM3_BUILD := $(DRAMSIM3_DIR)/build
DRAMSIM3_LIB := $(DRAMSIM3_BUILD)/libdramsim3.a
MEMORY_CXX :=
MEMORY_LDFLAGS :=

ISS_HOME ?= /home/yang/work/ISS
SOFTFLOAT_DIR := $(ISS_HOME)/third_party/softfloat
SOFTFLOAT_SRC := $(wildcard $(SOFTFLOAT_DIR)/*.c)
CSRC += $(SOFTFLOAT_SRC)

ifeq ($(WITH_DRAMSIM3),1)
BIN := $(BUILD_DIR)/sim-dram
OBJ_DIR := $(BUILD_DIR)/obj-dram
MEMORY_CXX := -DWITH_DRAMSIM3 -I$(abspath $(DRAMSIM3_DIR)/src) \
  -I$(abspath $(BUILD_DIR))
MEMORY_LDFLAGS := $(abspath $(DRAMSIM3_LIB))
endif

LINK_LDFLAGS := $(MEMORY_LDFLAGS)

$(BUILD_DIR)/dramsim3_paths.h:
	mkdir -p $(BUILD_DIR)
	printf '#define DRAMSIM3_CONFIG "%s"\n#define DRAMSIM3_OUTDIR "%s"\n' \
	  "$(abspath $(DRAMSIM3_DIR)/configs/XiangShan.ini)" \
	  "$(abspath $(BUILD_DIR)/dramsim3-out)" > $@

.PHONY: smoke clean dram compile-commands ref

smoke: $(BIN)

$(OBJ_DIR)/$(PREFIX).mk: $(RTL) $(CSRC) $(HEADERS)
	@test -n "$(strip $(RTL))" || { echo "RTL is required"; exit 1; }
	mkdir -p $(OBJ_DIR)
	$(VERILATOR) --cc --exe --top-module $(TOP) --prefix $(PREFIX) \
	  --Mdir $(OBJ_DIR) \
	  -o $(abspath $(BIN)) \
	  $(TRACE_VERILATOR) \
	  $(VERILATOR_EXTRA) \
	  --assert \
	  -Wno-WIDTH -Wno-DECLFILENAME \
	  $(if $(LINK_LDFLAGS),-LDFLAGS "$(LINK_LDFLAGS)") \
	  -CFLAGS "-I$(abspath csrc) -I$(abspath $(ISS_HOME)/include) -I$(abspath $(SOFTFLOAT_DIR)) $(TRACE_CXX) $(MEMORY_CXX)" \
	  $(abspath $(RTL)) $(ABS_CSRC)

$(BIN): $(OBJ_DIR)/$(PREFIX).mk $(CSRC) $(HEADERS) $(if $(filter 1,$(WITH_DRAMSIM3)),$(BUILD_DIR)/dramsim3_paths.h)
	$(MAKE) -C $(OBJ_DIR) -f $(PREFIX).mk -j$(JOBS) OBJCACHE=

$(DRAMSIM3_LIB):
	@test -f $(DRAMSIM3_DIR)/src/cosimulation.h || { \
	  echo "DRAMsim3 submodule is missing. Run: git submodule update --init third_party/DRAMsim3"; \
	  exit 1; }
	cmake -S $(DRAMSIM3_DIR) -B $(DRAMSIM3_BUILD) -DCOSIM=1 -DCMAKE_BUILD_TYPE=Release
	cmake --build $(DRAMSIM3_BUILD) -j$(JOBS) --target dramsim3

dram: $(DRAMSIM3_LIB) $(BUILD_DIR)/dramsim3_paths.h
	$(MAKE) smoke WITH_DRAMSIM3=1 EMU_TRACE=0

compile-commands: $(OBJ_DIR)/$(PREFIX).mk
	bear --output $(abspath compile_commands.json) -- $(MAKE) -C $(OBJ_DIR) -f $(PREFIX).mk -B OBJCACHE=
	python3 scripts/use_path_compiler.py $(abspath compile_commands.json)

clean:
	rm -rf $(BUILD_DIR)

ref:
	CCACHE_DIR=/tmp/ccache-rviss CCACHE_TEMPDIR=/tmp/ccache-rviss/tmp TMPDIR=/tmp \
	  $(MAKE) -C $(ISS_HOME) ref -j$(JOBS)

