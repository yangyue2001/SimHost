# Verilator session.
#   make smoke                  -> build/sim
#   EMU_TRACE=0 make smoke      -> build/sim-off
#   EMU_TRACE=fst make smoke    -> build/sim-fst
#   make check
#   make dram                   -> build/sim-dram, timed by third_party/DRAMsim3
#   make dram-check

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

CSRC := $(wildcard csrc/session/*.cpp) $(wildcard csrc/platform/*.cpp) \
        $(wildcard csrc/service/*/*.cpp)
HEADERS := $(wildcard csrc/session/*.h) $(wildcard csrc/platform/*.h) \
           $(wildcard csrc/service/*.h) $(wildcard csrc/service/*/*.h)
ABS_CSRC := $(abspath $(CSRC))

WITH_DRAMSIM3 ?= 0
DRAMSIM3_DIR := third_party/DRAMsim3
DRAMSIM3_BUILD := $(DRAMSIM3_DIR)/build
DRAMSIM3_LIB := $(DRAMSIM3_BUILD)/libdramsim3.a
MEMORY_CXX :=
MEMORY_LDFLAGS :=

ifeq ($(WITH_DRAMSIM3),1)
BIN := $(BUILD_DIR)/sim-dram
OBJ_DIR := $(BUILD_DIR)/obj-dram
MEMORY_CXX := -DWITH_DRAMSIM3 -I$(abspath $(DRAMSIM3_DIR)/src) \
  -I$(abspath $(BUILD_DIR))
MEMORY_LDFLAGS := $(abspath $(DRAMSIM3_LIB))
endif

$(BUILD_DIR)/dramsim3_paths.h:
	mkdir -p $(BUILD_DIR)
	printf '#define DRAMSIM3_CONFIG "%s"\n#define DRAMSIM3_OUTDIR "%s"\n' \
	  "$(abspath $(DRAMSIM3_DIR)/configs/XiangShan.ini)" \
	  "$(abspath $(BUILD_DIR)/dramsim3-out)" > $@

.PHONY: smoke check clean dram dram-check ram ram-check lightsss-check lightsss-wave-check
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
	  $(if $(MEMORY_LDFLAGS),-LDFLAGS "$(MEMORY_LDFLAGS)") \
	  -CFLAGS "-I$(abspath csrc) $(TRACE_CXX) $(MEMORY_CXX)" \
	  $(abspath $(RTL)) $(ABS_CSRC)

$(BIN): $(OBJ_DIR)/$(PREFIX).mk $(CSRC) $(HEADERS) $(if $(filter 1,$(WITH_DRAMSIM3)),$(BUILD_DIR)/dramsim3_paths.h)
	$(MAKE) -C $(OBJ_DIR) -f $(PREFIX).mk -j$(JOBS) OBJCACHE=

check:
	$(MAKE) smoke EMU_TRACE=1
	$(MAKE) smoke EMU_TRACE=0
	./scripts/check_phase1.sh

$(DRAMSIM3_LIB):
	@test -f $(DRAMSIM3_DIR)/src/cosimulation.h || { \
	  echo "DRAMsim3 submodule is missing. Run: git submodule update --init third_party/DRAMsim3"; \
	  exit 1; }
	cmake -S $(DRAMSIM3_DIR) -B $(DRAMSIM3_BUILD) -DCOSIM=1 -DCMAKE_BUILD_TYPE=Release
	cmake --build $(DRAMSIM3_BUILD) -j$(JOBS) --target dramsim3

dram: $(DRAMSIM3_LIB) $(BUILD_DIR)/dramsim3_paths.h
	$(MAKE) smoke WITH_DRAMSIM3=1 EMU_TRACE=0 RTL="tb/DramProof.sv tb/dpi/SimAXIMem.sv"

dram-check: dram
	$(MAKE) smoke EMU_TRACE=1
	./scripts/check_dramsim3.sh

ram:
	$(MAKE) smoke EMU_TRACE=0 BIN=$(abspath $(BUILD_DIR)/sim-ram) \
	  OBJ_DIR=$(abspath $(BUILD_DIR)/obj-ram) \
	  RTL="tb/RamProof.sv tb/dpi/SimAXIMem.sv"

ram-check: ram
	./scripts/check_ram.sh

lightsss-check: $(DRAMSIM3_LIB) $(BUILD_DIR)/dramsim3_paths.h
	$(MAKE) smoke WITH_DRAMSIM3=1 EMU_TRACE=0 \
	  RTL="tb/LightsssProof.sv tb/dpi/SimAXIMem.sv"
	./scripts/check_lightsss.sh

lightsss-wave-check: $(DRAMSIM3_LIB) $(BUILD_DIR)/dramsim3_paths.h
	$(MAKE) smoke WITH_DRAMSIM3=1 EMU_TRACE=1 \
	  BIN=$(abspath $(BUILD_DIR)/sim-lightsss-wave) \
	  OBJ_DIR=$(abspath $(BUILD_DIR)/obj-lightsss-wave) \
	  RTL="tb/LightsssProof.sv tb/dpi/SimAXIMem.sv"
	./scripts/check_lightsss_wave.sh

compile-commands: $(OBJ_DIR)/$(PREFIX).mk
	bear --output $(abspath compile_commands.json) -- $(MAKE) -C $(OBJ_DIR) -f $(PREFIX).mk -B OBJCACHE=
	python3 scripts/use_path_compiler.py $(abspath compile_commands.json)

clean:
	rm -rf $(BUILD_DIR)
