# Verilator 仿真框架阶段性设计

状态：修订 3。阶段 1 已实现，阶段 2 未开始。一次只做一个阶段。

参考实现：`/home/yang/study_dir/rvbosc/difftest`。AXI 访存时序的 RTL 在香山 `AXI4Memory`，不在 difftest 仓库内。

这次修订把目标从“先抄一套能跑的主循环”改成“主循环不认识具体 DPI”。DRAMSim3、difftest 和以后的其它 DPI-C 都是同一种服务。

## 目标

框架分成平台、会话、服务三层。`main` 不是第四层，它是进程入口。

- 平台只负责 Verilator 的时钟、复位、eval 和波形。
- 会话是这一次运行的剧本：解析参数、复位、主循环、阶段调度和服务表。
- 每个 DPI 功能是一个服务。服务自己带 `extern "C"` 入口，自己注册参数和每周期动作。
- `main` 做进程级初始化，打开一次会话并调用 `run`。循环体不写进 `main`。会话把 trap 交回来之后，`main` 打印结果并转换成进程返回码。`main` 不点名具体服务，也不拨时钟。

第一份可运行结果仍然是：初始化、参数、只记上半沿的波形、DRAMSim3 访存延迟。但它必须沿着服务接口接进去，不能写死在主循环里。

## 不做什么

- 不把 difftest 整仓搬进来，也不在早期阶段实现 checker。
- 不把 DRAMSim3 当成存数据的存储器。
- 不做 UART、Flash、SD、JTAG、VGA、coverage、ChiselDB、PGO、fuzz、VCS、FPGA。
- 不为了“以后可能用”给平台类加空参数。扩展只通过服务接口。

## 参考实现里耦合在哪里

`src/test/csrc/emu/emu.cpp` 直接包含并调用内存、DRAMSim3、difftest、UART、Flash、LightSSS、coverage。`Simulator` 还要求每个后端实现 `get_difftest_exit`、`get_difftest_step`、perf 和 log 引脚。

结果是：

- 加一个 DPI，就要改主循环、参数表和仿真器基类。
- 内存 DPI 和 difftest DPI 看起来都是 `extern "C"`，但生命周期没有统一位置。
- RTL 在 `eval()` 里面调用 DPI；C++ 又在 eval 之外推进 DRAMSim3 和 difftest。这两类时机混在同一个函数里。

这些 feature 本身要保留，调用方式要换掉。

## 分层

```
平台 Platform
  生成模型的 clock / reset / eval
  波形
  不包含任何 DPI 名字

main
  Verilator 参数和信号处理
  打开会话，调用 run
  打印 trap，转换成返回码

会话 Session
  解析参数
  复位和主循环
  按固定阶段调用服务
  把 trap 交回 main

服务 Service
  MemoryService     功能 RAM + DRAMSim3
  ProbeService      用来证明能再加一个 DPI
  DiffTestService   以后的差分测试
  其它 DPI          同一套接法
```

RTL 侧也按对拆开。顶层模块不包含服务实现。每组 DPI 有自己的 SystemVerilog 包装，放在 `vsrc/dpi/`，只调用该服务公开的 C 符号。

顶层模块名用 `make TOP=...` 更换，RTL 文件用 `make RTL=...` 更换。Verilator `--prefix VTop` 把生成的 C++ 类固定为 `VTop`，所以 `csrc/platform/verilator.cpp` 不用跟着改。引脚仍然是 `clock`、`reset`、`exit`。

```
csrc/platform/     时钟、复位、波形、Verilator 包装
csrc/session/      进程入口、参数、一次运行的循环、服务表
csrc/service/memory/
csrc/service/probe/        阶段 3 才加
csrc/service/difftest/     阶段 4 才加
vsrc/dpi/SimAXIMem.sv
tb/SmokeTop.sv
```

会话只能包含 `service.h` 和服务表。服务表是唯一允许点名具体服务的地方。

## 服务合同

DPI 会在 `eval()` 内部被 RTL 调用。服务的 C++ 钩子只跑在 eval 外面。两边不能混用。

```cpp
enum class SimPhase {
  AfterPosedge,  // 上半沿 eval 已结束，本拍 RTL 的 DPI 已调用完
  MidCycle,      // 两条沿之间。DRAMSim3 在这里推进
  AfterNegedge,  // 下半沿 eval 已结束
  EndCycle       // 整个架构周期结束。difftest 在这里比对
};

class SimService {
public:
  virtual ~SimService() = default;
  virtual const char *name() const = 0;
  virtual void register_options(OptionSet &) {}
  virtual void init(const SimContext &) {}
  virtual void on_phase(SimPhase, const SimContext &) {}
  virtual int poll_trap() const { return STATE_RUNNING; }
  virtual void at_clone() {}
  virtual void finish() {}
};
```

`SimContext` 只放周期号和已经解析好的参数视图。不放 `VTop*`，不放别的服务指针。

阶段顺序固定：

1. `main` 把原始 argv 交给 Verilator，并装上 SIGINT / SIGTERM。
2. 会话构造服务表。每个服务 `register_options`，然后统一解析 argv。未知参数失败。帮助和参数错误由 `main` 直接返回，不进入循环。
3. 创建平台，必要时打开波形。
4. 复位。复位期间不调用服务的 `on_phase`。
5. 每个服务 `init`。功能 RAM 和 DRAMSim3 在这里初始化。
6. 主循环每个架构周期：
   - 上半沿 eval
   - 需要时记上半沿波形
   - `AfterPosedge`
   - `MidCycle`
   - 下半沿 eval
   - 全波形时记下半沿
   - `AfterNegedge`
   - `EndCycle`
   - 读平台 `exit` 引脚，再问每个服务 `poll_trap`
7. 结束时每个服务 `finish`，然后关波形。会话把 trap 和周期号交回 `main`。
8. `main` 打印 trap。波形或初始化失败时不打印 trap 行，进程返回 1。

服务之间不互相 include 私有头。阶段 4 之前不允许服务互相调用。真有共享状态时，先加一个更窄的只读接口，不把会话做成服务定位器。

`extern "C"` 规则：

- 符号定义放在对应服务目录里。
- DPI 函数只转发给本服务，不调用 `eval`、不记波形、不推进别的服务。
- 符号名字一旦被 RTL 使用就不再改。内存继续用 `difftest_ram_read`、`difftest_ram_write`、`memory_request`、`memory_response`，以便以后接香山 `AXI4Memory`。

加一个新 DPI 时只允许做这些事：

1. 新建 `csrc/service/<name>/`，实现 `SimService`。
2. 在该目录写 `extern "C"`。
3. 在服务表加一行。
4. 需要 RTL 包装时，加 `vsrc/dpi/<Name>.sv`，由 DUT 例化。
5. 不改平台，不改主循环，不加新的 `SimPhase`。

现有四个阶段不够用时，先改本文，再增加枚举值。不能在某个服务里私自再跑一次 eval。

## 参数

参数分成两层。

- 会话拥有全局参数：`--help`、`--max-cycles`、`--reset-cycles`、`--seed`、`-b/--log-begin`、`-e/--log-end`、`--dump-wave`、`--dump-wave-full`、`--wave-path`。
- 服务拥有自己的参数。内存服务拥有 `-i/--image`、`--ram-size`、`--dramsim3-ini`、`--dramsim3-outdir`。

没编进二进制的服务，它的参数必须报错退出，不能静默忽略。这和参考实现一致。

参考实现还有大量参数，以后哪个服务实现，就由那个服务注册，不提前占位：

- 负载扩展：gcpt、footprints、copy-ram、random-mem、ELF/gz/zstd
- 停止和统计：`--max-instr`、`--warmup-instr`、`--stat-cycles`
- LightSSS：`--enable-fork`、`--fork-interval`
- difftest：`--diff`、`--no-diff`、trace、squash
- 设备：flash、jtag、uart、coverage、ChiselDB

## 平台行为

时钟只有 core clock。一个架构周期是两次 eval：先 `clock=1`，再 `clock=0`。不做 accelerator clock。

复位默认 50 个周期，可用 `--reset-cycles` 修改，最小为 1。每个复位周期 reset 在两次 eval 期间为 1，循环结束后清 0。复位周期不计入 `cycles`，也不 tick 服务。断言在复位完成、服务 `init` 之后才打开。

波形属于平台，不属于服务。它需要 Verilator 模型对象。

- `EMU_TRACE=1` 生成 VCD，`EMU_TRACE=fst` 生成 FST。没开 trace 又请求波形时，进程直接失败。
- `--dump-wave` 只在上半沿 eval 之后记一次。
- `--dump-wave-full` 两个半沿都记。
- 只追上半沿时，时间戳等于周期号。全波形从 `2 * log_begin` 起，每个半周期加 1。
- 复位期间只有“全波形且 `log_begin == 0`”才记录。

trap 由会话在循环内裁决。进程返回码由 `main` 给出：

| 来源 | 结果 | 返回码 |
| --- | --- | --- |
| 顶层 `exit` 为全 1 | good | 0 |
| 顶层 `exit` 为其它非 0 | abort | 1 |
| 服务 `poll_trap` 返回 abort | abort | 1 |
| 到达 `--max-cycles` | limit | 0 |
| SIGINT 或 SIGTERM | signal | 1 |
| 波形或服务初始化失败 | abort | 1 |

`SmokeTop` 用顶层 `exit` 即可。以后 difftest 失败走服务的 `poll_trap`，不必再给平台加 difftest 引脚。

## 参考 feature 如何落到服务

下面这些都是参考实现已有的能力。没有阶段号的，只记录归属，不排期。

### 已经归到平台或会话

- 半周期主循环、复位、周期上限、信号处理、host/guest 时间统计。
- VCD/FST、上半沿或全波形、日志窗口。
- 旧版 Verilator 需要时把栈调到 32MB。这项由平台处理。

### 内存服务

功能数据和延迟分开。

- mmap 功能 RAM。本框架默认 256MiB，`--ram-size` 接受 `B/KB/MB/GB`。
- 裸二进制装到偏移 0。`PMEM_BASE = 0x80000000`，AXI 字节地址先减基址，再除以 8 得到 RAM index。
- 写掩码语义保持 `old & ~mask | data & mask`。
- 低于基址或超出容量直接失败。参考实现的读越界取模不继承。
- `memory_request(address, id, isWrite)` 返回 1 才表示 DRAMSim3 收下。
- `memory_response(isWrite)` 完成时返回 `id | (1 << 32)`，否则返回 0。读写队列分开。
- 每个架构周期在 `MidCycle` 调用一次 `dramsim3_step`。
- 写数据在 AXI W fire 时进功能 RAM。B 通道只表示 DRAMSim3 完成。读数据在 response 返回对应 id 后读取。
- 默认 ini 是 `$(DRAMSIM3_HOME)/configs/XiangShan.ini`。找不到 home、库或 ini 文件就失败。
- 编译期可关 `WITH_DRAMSIM3`。关闭后内存参数仍然属于这个服务；传入 DRAMSim3 参数要报错。

阶段 2 的 AXI 子集：64 位数据和地址，只支持 `len == 0` 的单拍。id 默认 4 bit。reset 为 1 时不调用内存 DPI。burst、写数据早于地址，等真核接入再加，不因此修改服务接口。

以后仍归这个服务、但不在阶段 2 做的：ELF、gz、zstd、footprints、gcpt、copy-ram、random-mem、MCSim。MCSim 和 DRAMSim3 继续互斥，做成这个服务内部的时序后端，不新加一种会话。

### 阶段 3 的探针服务

这不是参考实现里的产品功能。它用来锁住扩展方式：一个和内存无关的 DPI，例如 `probe_push(value)`，在 `EndCycle` 检查累计值并 `poll_trap`。实现它时，`emu` 主循环的 diff 应该只有服务表那一行。

### difftest 服务

参考能力包括 Gateway、squash、batch、delta、replay、DPI buffer、NEMU/Spike `dlopen`、指令/load/store/trap/TLB/refill/CSR checker、golden memory、stuck limit、IOTrace、MMA。

阶段 4 只放进服务的最小子集：

- 一个提交记录：`valid/pc/inst/rd/wdata`
- DPI 写本服务的缓冲
- `EndCycle` 取出并调用 `ref_init/ref_step/ref_regcpy`
- 比对 PC，以及 `rd != 0` 的写回
- 不一致时 `poll_trap` 返回 abort

squash、batch、delta、多核、TLB、golden memory 各自以后单独做。它们都应该留在 `csrc/service/difftest/` 内，不再回到会话。

### LightSSS

参考实现用 `fork` 保留两个子进程，失败后唤醒最老的，再向前跑约 100 周期并强制记波。它不是 DPI 服务，是会话外面的进程策略。

阶段 5 做。会话在 fork 出的子进程里调用每个服务的 `at_clone()`，平台负责重建波形。DRAMSim3 是否有后台线程，必须在实现前查清；没查清就不能宣称可以和 fork 一起用。

### 继续后挂的 feature

这些都不进入前五个阶段：

- 设备：UART、Flash、SD、SimJTAG、VGA、keyboard
- 后端：VCS、GSIM、FPGA、Palladium
- 观测：coverage、ChiselDB、ConstantIn、IPC、runahead、simfrontend、PGO

真的要加时，设备优先做成新的 DPI 服务。换仿真器后端只替换平台，服务代码不动。

## 阶段 1：平台和空服务表

状态：已实现。`make smoke` 生成带 VCD 的 `build/sim`。`EMU_TRACE=0 make smoke` 生成 `build/sim-off`。`EMU_TRACE=fst make smoke` 生成 `build/sim-fst`。`make check` 覆盖本阶段验收，并且不读取 `DRAMSIM3_HOME`。

交付：没有 DRAMSim3 也能跑的仿真循环。

- `SmokeTop` 在若干周期后把 `exit` 设为全 1。
- 全局参数和波形规则按上文实现。
- 服务表存在，但可以为空。
- `make smoke` 不要求 `DRAMSIM3_HOME`。

通过标准：

- `--dump-wave` 的采样里 `clock` 始终为 1。
- `--dump-wave-full` 能交替看到 1 和 0。
- 没编译 trace 时请求波形，返回非 0。
- `--max-cycles` 在 DUT 自行结束之前生效，返回 0。

## 阶段 2：内存服务

交付：第一个真正的 DPI 服务。

- 加入 `MemoryService`、功能 RAM、DRAMSim3 和 `vsrc/dpi/SimAXIMem.sv`。
- `SmokeTop` 在 reset 放开后做一笔单拍写和一笔单拍读，读回一致后正常退出。
- 主循环源码里不能出现 `dramsim3` 或 `difftest_ram` 这些名字。它们只出现在内存服务目录。

通过标准：

- 读响应至少晚于请求一个 DRAMSim3 tick。
- 不设置 `DRAMSIM3_HOME` 时，打开该服务的构建失败。
- `--dramsim3-ini` 指向不存在的文件时，在进入主循环前失败。
- 关掉 `WITH_DRAMSIM3` 后仍能跑阶段 1 的纯 `exit` 测试。这时 `SimAXIMem` 不参与编译。

## 阶段 3：第二个 DPI

交付：证明扩展流程。

- 增加 `ProbeService` 和一条 DPI。
- `SmokeTop` 每周期上报一个递增数。服务在达到预期值后给出 good trap。
- 审查范围：除服务表一行外，`csrc/platform` 和 `csrc/session` 没有功能改动。
- 探针和内存服务同时打开时，两者都工作。

这一阶段不引入新的 `SimPhase`。探针放在 `EndCycle`。

## 阶段 4：最小 difftest

交付：`DiffTestService`。

- 只依赖阶段 1 到阶段 3 的服务合同，不依赖 LightSSS。
- 参考模型用 `dlopen`，路径来自该服务自己的 `--diff`。`--no-diff` 时服务仍接收 DPI，但不比对。
- 比对失败由 `poll_trap` 结束仿真。
- 不修改平台端口。特别是不加 `get_difftest_step` 这类虚函数。

通过标准用一个固定指令流的小 DUT：对的参考模型返回 0，故意错的写回返回 1，并且打印 DUT 和 REF 的 PC、寄存器号和数据。

## 阶段 5：LightSSS

- 增加会话级 fork 策略和 `--enable-fork`、`--fork-interval`。
- 父进程自己不记波。子进程 `at_clone()` 后强制记波。
- 失败点之后大约再跑 100 周期。
- 每个已经存在的服务都要声明自己能否被 fork。不能被 fork 的服务在启用 LightSSS 时初始化失败，不能假装可用。

## 实现约束

- 生成物只放 `build/`。
- 服务表是会话层唯一的具体服务名单。
- DPI 符号、SV 包装和服务实现三者同目录审查。不要把 DPI 声明集中到一个公共大头文件里再被所有服务包含。
- 阶段 3 的审查用来防止接口变形：如果加探针必须修改主循环，先回到本文增加阶段或修改服务合同，不能就地把循环改成特判。
- 文档和代码冲突时先改文档。已完成阶段的对外行为改了，就另写修订，不悄悄改验收标准。
