package difftest

import chisel3._

// chisel3._ also brings in chisel3.probe. Use the root package.
import _root_.probe.ProbeBundle

// Difftest adds the hart id. It is always words[0]. Only hart 0 is accepted.
abstract class DifftestBundle extends ProbeBundle {
  val coreid = UInt(8.W)
}

// words: coreid, valid, index, rfwen, wdest, pc, inst, wdata, skip.
// index is the retire port. The oldest instruction is 0.
// valid also gates the DPI call, so a pushed sample has valid == 1.
class Commit extends DifftestBundle {
  val valid = Bool()
  val index = UInt(8.W)
  val rfwen = Bool()
  val wdest = UInt(8.W)
  val pc = UInt(64.W)
  val inst = UInt(32.W)
  val wdata = UInt(64.W)
  val skip = Bool()
}

// words: coreid, value_0 .. value_31. value_0 is x0.
// No valid field: the probe is sent on every non-reset cycle.
class Xrf extends DifftestBundle {
  val value = Vec(32, UInt(64.W))
}

// words: coreid, value_0 .. value_31. value_0 is f0.
class Fpr extends DifftestBundle {
  val value = Vec(32, UInt(64.W))
}

// words: coreid, then the difftest_csr argument order.
class Csr extends DifftestBundle {
  val privilege = UInt(8.W)
  val mstatus = UInt(64.W)
  val sstatus = UInt(64.W)
  val mepc = UInt(64.W)
  val sepc = UInt(64.W)
  val mtval = UInt(64.W)
  val stval = UInt(64.W)
  val mtvec = UInt(64.W)
  val stvec = UInt(64.W)
  val mcause = UInt(64.W)
  val scause = UInt(64.W)
  val satp = UInt(64.W)
  val mip = UInt(64.W)
  val mie = UInt(64.W)
  val mscratch = UInt(64.W)
  val sscratch = UInt(64.W)
  val mideleg = UInt(64.W)
  val medeleg = UInt(64.W)
}
