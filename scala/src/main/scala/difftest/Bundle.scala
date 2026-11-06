package difftest

import chisel3._

// chisel3._ also brings in chisel3.probe. Use the root package.
import _root_.probe.ProbeBundle

// Hart id. Flattened probe words start here. The checker accepts only hart 0.
abstract class DifftestBundle extends ProbeBundle {
  val coreid = UInt(8.W)
}

// One committed ROB entry. Index 0 is the oldest entry in the cycle.
// interrupt means do not execute this slot. The checker calls
// difftest_raise_trap(cause, tval). cause is mcause: bit 63 set is an
// interrupt, clear is a synchronous exception. size is the instruction
// length in bytes and is not used for a trap.
class DiffArchCommitInfo extends Bundle {
  val valid = Bool()
  val skip = Bool()
  val interrupt = Bool()
  val size = UInt(8.W)
  val cause = UInt(64.W)
  val tval = UInt(64.W)
}

// Architectural CSRs after this cycle's commits. sstatus is the visible
// S-mode view. The reference keeps that view in mstatus, not a second copy.
class DiffCSR extends Bundle {
  val privilege = UInt(64.W)
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
  val fcsr = UInt(64.W)
}

// Sampled only when the ROB commits. valid is the DPI enable. A nested
// commit(i).valid does not enable the probe.
// xrf, frf, csr, and pc are the architectural state after these commits.
class DiffArchCommit(commitWidth: Int = 4) extends DifftestBundle {
  require(commitWidth > 0)

  val valid = Bool()
  val commit = Vec(commitWidth, new DiffArchCommitInfo)
  val xrf = Vec(32, UInt(64.W))
  val frf = Vec(32, UInt(64.W))
  val csr = new DiffCSR
  val pc = UInt(64.W)
}
