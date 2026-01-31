#include "RegAllocCustom.h"
#include "AllocationOrder.h"
#include "RegAllocGraphSolvers.h"
#include "RegInterferenceGraph.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "RegAllocCounter.h"
#include "llvm/IR/Module.h"

#include "RegAllocGreedy.h"

#include <iostream>

using namespace llvm;

#define USE_GREEDY_OUTPUT 0

#define DEBUG_TYPE "regalloc"

#define REGALLOC_GRAPH_SOLVER_ENTRY(Name, ID) \
static FunctionPass* createCustomRegisterAllocator_##ID() { \
  return new RACustom(#ID); \
} \
//static RegisterRegAlloc CustomRegAlloc_##ID(#ID, #ID, createCustomRegisterAllocator_##ID);
  REGALLOC_GRAPH_SOLVER_TABLE
#undef REGALLOC_GRAPH_SOLVER_ENTRY

char RACustom::ID = 0;

char &llvm::RACustomID = RACustom::ID;

INITIALIZE_PASS_BEGIN(RACustom, "regalloccustom", "Custom Register Allocator",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LiveDebugVariablesWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(SlotIndexesWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(RegisterCoalescerLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineSchedulerLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveStacksWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(VirtRegMapWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrixWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
#if USE_GREEDY_OUTPUT
INITIALIZE_PASS_DEPENDENCY(RAGreedyLegacy)
#endif
INITIALIZE_PASS_END(RACustom, "regalloccustom", "Custom Register Allocator", false,
                    false)

bool RACustom::LRE_CanEraseVirtReg(Register VirtReg) {
  LiveInterval &LI = LIS->getInterval(VirtReg);
  if (VRM->hasPhys(VirtReg)) {
    Matrix->unassign(LI);
    aboutToRemoveInterval(LI);
    return true;
  }
  // Unassigned virtreg is probably in the priority queue.
  // RegAllocBase will erase it after dequeueing.
  // Nonetheless, clear the live-range so that the debug
  // dump will show the right state for that VirtReg.
  LI.clear();
  return false;
}

void RACustom::LRE_WillShrinkVirtReg(Register VirtReg) {
  if (!VRM->hasPhys(VirtReg))
    return;

  // Register is assigned, put it back on the queue for reassignment.
  LiveInterval &LI = LIS->getInterval(VirtReg);
  Matrix->unassign(LI);
  enqueue(&LI);
}

RACustom::RACustom(const char *AlgoID, RegAllocFilterFunc F)
    : MachineFunctionPass(ID), RegAllocBase(F), AlgorithmID(AlgoID)
{
    srand(time(0));
}

void RACustom::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  AU.addRequired<LiveIntervalsWrapperPass>();
  AU.addPreserved<LiveIntervalsWrapperPass>();
  AU.addPreserved<SlotIndexesWrapperPass>();
  AU.addRequired<LiveDebugVariablesWrapperLegacy>();
  AU.addPreserved<LiveDebugVariablesWrapperLegacy>();
  AU.addRequired<LiveStacksWrapperLegacy>();
  AU.addPreserved<LiveStacksWrapperLegacy>();
  AU.addRequired<ProfileSummaryInfoWrapperPass>();
  AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
  AU.addPreserved<MachineBlockFrequencyInfoWrapperPass>();
  AU.addRequired<MachineDominatorTreeWrapperPass>();
  AU.addRequiredID(MachineDominatorsID);
  AU.addPreservedID(MachineDominatorsID);
  AU.addRequired<MachineLoopInfoWrapperPass>();
  AU.addPreserved<MachineLoopInfoWrapperPass>();
  AU.addRequired<VirtRegMapWrapperLegacy>();
  AU.addPreserved<VirtRegMapWrapperLegacy>();
  AU.addRequired<LiveRegMatrixWrapperLegacy>();
  AU.addPreserved<LiveRegMatrixWrapperLegacy>();
#if USE_GREEDY_OUTPUT
  AU.addRequiredID(RAGreedyLegacyID);
  AU.addPreservedID(RAGreedyLegacyID);
#endif
  MachineFunctionPass::getAnalysisUsage(AU);
}

void RACustom::releaseMemory() {
  SpillerInstance.reset();
}


// Spill or split all live virtual registers currently unified under PhysReg
// that interfere with VirtReg. The newly spilled or split live intervals are
// returned by appending them to SplitVRegs.
bool RACustom::spillInterferences(const LiveInterval &VirtReg,
                                 MCRegister PhysReg,
                                 SmallVectorImpl<Register> &SplitVRegs) {
  // Record each interference and determine if all are spillable before mutating
  // either the union or live intervals.
  SmallVector<const LiveInterval *, 8> Intfs;

  // Collect interferences assigned to any alias of the physical register.
  for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
    LiveIntervalUnion::Query &Q = Matrix->query(VirtReg, Unit);
    for (const auto *Intf : reverse(Q.interferingVRegs())) {
      if (!Intf->isSpillable() || Intf->weight() > VirtReg.weight())
        return false;
      Intfs.push_back(Intf);
    }
  }
  LLVM_DEBUG(dbgs() << "spilling " << printReg(PhysReg, TRI)
                    << " interferences with " << VirtReg << "\n");
  assert(!Intfs.empty() && "expected interference");

  // Spill each interfering vreg allocated to PhysReg or an alias.
  for (const LiveInterval *Spill : Intfs) {
    // Skip duplicates.
    if (!VRM->hasPhys(Spill->reg()))
      continue;

    // Deallocate the interfering vreg by removing it from the union.
    // A LiveInterval instance may not be in a union during modification!
    Matrix->unassign(*Spill);

    // Spill the extracted interval.
    RegAllocCounter::addSpillage(Spill);
    LiveRangeEdit LRE(Spill, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
    spiller().spill(LRE);
  }
  return true;
}

// Driver for the register assignment and splitting heuristics.
// Manages iteration over the LiveIntervalUnions.
//
// This is a minimal implementation of register assignment and splitting that
// spills whenever we run out of registers.
//
// selectOrSplit can only be called once per live virtual register. We then do a
// single interference test for each register the correct class until we find an
// available register. So, the number of interference tests in the worst case is
// |vregs| * |machineregs|. And since the number of interference tests is
// minimal, there is no value in caching them outside the scope of
// selectOrSplit().
MCRegister RACustom::selectOrSplit(const LiveInterval &VirtReg,
                                  SmallVectorImpl<Register> &SplitVRegs) {
  if(!StopAlgorithm)
  {
      enqueue(&VirtReg);
      if(iterateSolution(SplitVRegs))
          return 0;
      else
          std::cout << "Sutoppu" << std::endl;
  }

  bool StoppedThisIteration = !StopAlgorithm;
  StopAlgorithm = true;

  if(StoppedThisIteration)
      return 0;

  // Populate a list of physical register spill candidates.
  SmallVector<MCRegister, 8> PhysRegSpillCands;

  // Check for an available register in this class.
  auto Order =
      AllocationOrder::create(VirtReg.reg(), *VRM, RegClassInfo, Matrix);
  for (MCRegister PhysReg : Order) {
    assert(PhysReg.isValid());
    // Check for interference in PhysReg
    switch (Matrix->checkInterference(VirtReg, PhysReg)) {
    case LiveRegMatrix::IK_Free:
      // PhysReg is available, allocate it.
      return PhysReg;

    case LiveRegMatrix::IK_VirtReg:
      // Only virtual registers in the way, we may be able to spill them.
      PhysRegSpillCands.push_back(PhysReg);
      continue;

    default:
      // RegMask or RegUnit interference.
      continue;
    }
  }

  // Try to spill another interfering reg with less spill weight.
  for (MCRegister &PhysReg : PhysRegSpillCands) {
    if (!spillInterferences(VirtReg, PhysReg, SplitVRegs))
      continue;

    StopAlgorithm = false;

    assert(!Matrix->checkInterference(VirtReg, PhysReg) &&
           "Interference after spill.");
    // Tell the caller to allocate to this newly freed physical register.
    return PhysReg;
  }

  // No other spill candidates were found, so spill the current VirtReg.
  LLVM_DEBUG(dbgs() << "spilling: " << VirtReg << '\n');
  if (!VirtReg.isSpillable())
    return ~0u;

  RegAllocCounter::addSpillage(&VirtReg);
  LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  spiller().spill(LRE);

  // The live virtual register requesting allocation was spilled, so tell
  // the caller not to allocate anything during this round.
  return 0;
}

#if 0
#include "llvm/Support/Timer.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

const float Hysteresis = (2007 / 2048.0f); // 0.97998046875

void RACustom::calcGapWeights(MCRegister PhysReg,
                              SmallVectorImpl<float> &GapWeight) {
  assert(SA->getUseBlocks().size() == 1 && "Not a local interval");
  const SplitAnalysis::BlockInfo &BI = SA->getUseBlocks().front();
  ArrayRef<SlotIndex> Uses = SA->getUseSlots();
  const unsigned NumGaps = Uses.size()-1;

  // Start and end points for the interference check.
  SlotIndex StartIdx =
    BI.LiveIn ? BI.FirstInstr.getBaseIndex() : BI.FirstInstr;
  SlotIndex StopIdx =
    BI.LiveOut ? BI.LastInstr.getBoundaryIndex() : BI.LastInstr;

  GapWeight.assign(NumGaps, 0.0f);

  // Add interference from each overlapping register.
  for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
    if (!Matrix->query(const_cast<LiveInterval &>(SA->getParent()), Unit)
             .checkInterference())
      continue;

    // We know that VirtReg is a continuous interval from FirstInstr to
    // LastInstr, so we don't need InterferenceQuery.
    //
    // Interference that overlaps an instruction is counted in both gaps
    // surrounding the instruction. The exception is interference before
    // StartIdx and after StopIdx.
    //
    LiveIntervalUnion::SegmentIter IntI =
        Matrix->getLiveUnions()[Unit].find(StartIdx);
    for (unsigned Gap = 0; IntI.valid() && IntI.start() < StopIdx; ++IntI) {
      // Skip the gaps before IntI.
      while (Uses[Gap+1].getBoundaryIndex() < IntI.start())
        if (++Gap == NumGaps)
          break;
      if (Gap == NumGaps)
        break;

      // Update the gaps covered by IntI.
      const float weight = IntI.value()->weight();
      for (; Gap != NumGaps; ++Gap) {
        GapWeight[Gap] = std::max(GapWeight[Gap], weight);
        if (Uses[Gap+1].getBaseIndex() >= IntI.stop())
          break;
      }
      if (Gap == NumGaps)
        break;
    }
  }

  // Add fixed interference.
  for (MCRegUnit Unit : TRI->regunits(PhysReg)) {
    const LiveRange &LR = LIS->getRegUnit(Unit);
    LiveRange::const_iterator I = LR.find(StartIdx);
    LiveRange::const_iterator E = LR.end();

    // Same loop as above. Mark any overlapped gaps as HUGE_VALF.
    for (unsigned Gap = 0; I != E && I->start < StopIdx; ++I) {
      while (Uses[Gap+1].getBoundaryIndex() < I->start)
        if (++Gap == NumGaps)
          break;
      if (Gap == NumGaps)
        break;

      for (; Gap != NumGaps; ++Gap) {
        GapWeight[Gap] = huge_valf;
        if (Uses[Gap+1].getBaseIndex() >= I->end)
          break;
      }
      if (Gap == NumGaps)
        break;
    }
  }
}

MCRegister RACustom::tryLocalSplit(const LiveInterval &VirtReg,
                                   AllocationOrder &Order,
                                   SmallVectorImpl<Register> &NewVRegs) {
  // TODO: the function currently only handles a single UseBlock; it should be
  // possible to generalize.
  if (SA->getUseBlocks().size() != 1)
    return MCRegister();

  const SplitAnalysis::BlockInfo &BI = SA->getUseBlocks().front();

  // Note that it is possible to have an interval that is live-in or live-out
  // while only covering a single block - A phi-def can use undef values from
  // predecessors, and the block could be a single-block loop.
  // We don't bother doing anything clever about such a case, we simply assume
  // that the interval is continuous from FirstInstr to LastInstr. We should
  // make sure that we don't do anything illegal to such an interval, though.

  ArrayRef<SlotIndex> Uses = SA->getUseSlots();
  if (Uses.size() <= 2)
    return MCRegister();
  const unsigned NumGaps = Uses.size()-1;

  LLVM_DEBUG({
    dbgs() << "tryLocalSplit: ";
    for (const auto &Use : Uses)
      dbgs() << ' ' << Use;
    dbgs() << '\n';
  });

  // If VirtReg is live across any register mask operands, compute a list of
  // gaps with register masks.
  SmallVector<unsigned, 8> RegMaskGaps;
  if (Matrix->checkRegMaskInterference(VirtReg)) {
    // Get regmask slots for the whole block.
    ArrayRef<SlotIndex> RMS = LIS->getRegMaskSlotsInBlock(BI.MBB->getNumber());
    LLVM_DEBUG(dbgs() << RMS.size() << " regmasks in block:");
    // Constrain to VirtReg's live range.
    unsigned RI =
        llvm::lower_bound(RMS, Uses.front().getRegSlot()) - RMS.begin();
    unsigned RE = RMS.size();
    for (unsigned I = 0; I != NumGaps && RI != RE; ++I) {
      // Look for Uses[I] <= RMS <= Uses[I + 1].
      assert(!SlotIndex::isEarlierInstr(RMS[RI], Uses[I]));
      if (SlotIndex::isEarlierInstr(Uses[I + 1], RMS[RI]))
        continue;
      // Skip a regmask on the same instruction as the last use. It doesn't
      // overlap the live range.
      if (SlotIndex::isSameInstr(Uses[I + 1], RMS[RI]) && I + 1 == NumGaps)
        break;
      LLVM_DEBUG(dbgs() << ' ' << RMS[RI] << ':' << Uses[I] << '-'
                        << Uses[I + 1]);
      RegMaskGaps.push_back(I);
      // Advance ri to the next gap. A regmask on one of the uses counts in
      // both gaps.
      while (RI != RE && SlotIndex::isEarlierInstr(RMS[RI], Uses[I + 1]))
        ++RI;
    }
    LLVM_DEBUG(dbgs() << '\n');
  }

  // Since we allow local split results to be split again, there is a risk of
  // creating infinite loops. It is tempting to require that the new live
  // ranges have less instructions than the original. That would guarantee
  // convergence, but it is too strict. A live range with 3 instructions can be
  // split 2+3 (including the COPY), and we want to allow that.
  //
  // Instead we use these rules:
  //
  // 1. Allow any split for ranges with getStage() < RS_Split2. (Except for the
  //    noop split, of course).
  // 2. Require progress be made for ranges with getStage() == RS_Split2. All
  //    the new ranges must have fewer instructions than before the split.
  // 3. New ranges with the same number of instructions are marked RS_Split2,
  //    smaller ranges are marked RS_New.
  //
  // These rules allow a 3 -> 2+3 split once, which we need. They also prevent
  // excessive splitting and infinite loops.
  //
  bool ProgressRequired = ExtraInfo->getStage(VirtReg) >= RS_Split2;

  // Best split candidate.
  unsigned BestBefore = NumGaps;
  unsigned BestAfter = 0;
  float BestDiff = 0;

  const float blockFreq =
      SpillPlacer->getBlockFrequency(BI.MBB->getNumber()).getFrequency() *
      (1.0f / MBFI->getEntryFreq().getFrequency());
  SmallVector<float, 8> GapWeight;

  for (MCPhysReg PhysReg : Order) {
    assert(PhysReg);
    // Keep track of the largest spill weight that would need to be evicted in
    // order to make use of PhysReg between UseSlots[I] and UseSlots[I + 1].
    calcGapWeights(PhysReg, GapWeight);

    // Remove any gaps with regmask clobbers.
    if (Matrix->checkRegMaskInterference(VirtReg, PhysReg))
      for (unsigned Gap : RegMaskGaps)
        GapWeight[Gap] = huge_valf;

    // Try to find the best sequence of gaps to close.
    // The new spill weight must be larger than any gap interference.

    // We will split before Uses[SplitBefore] and after Uses[SplitAfter].
    unsigned SplitBefore = 0, SplitAfter = 1;

    // MaxGap should always be max(GapWeight[SplitBefore..SplitAfter-1]).
    // It is the spill weight that needs to be evicted.
    float MaxGap = GapWeight[0];

    while (true) {
      // Live before/after split?
      const bool LiveBefore = SplitBefore != 0 || BI.LiveIn;
      const bool LiveAfter = SplitAfter != NumGaps || BI.LiveOut;

      LLVM_DEBUG(dbgs() << printReg(PhysReg, TRI) << ' ' << Uses[SplitBefore]
                        << '-' << Uses[SplitAfter] << " I=" << MaxGap);

      // Stop before the interval gets so big we wouldn't be making progress.
      if (!LiveBefore && !LiveAfter) {
        LLVM_DEBUG(dbgs() << " all\n");
        break;
      }
      // Should the interval be extended or shrunk?
      bool Shrink = true;

      // How many gaps would the new range have?
      unsigned NewGaps = LiveBefore + SplitAfter - SplitBefore + LiveAfter;

      // Legally, without causing looping?
      bool Legal = !ProgressRequired || NewGaps < NumGaps;

      if (Legal && MaxGap < huge_valf) {
        // Estimate the new spill weight. Each instruction reads or writes the
        // register. Conservatively assume there are no read-modify-write
        // instructions.
        //
        // Try to guess the size of the new interval.
        const float EstWeight = normalizeSpillWeight(
            blockFreq * (NewGaps + 1),
            Uses[SplitBefore].distance(Uses[SplitAfter]) +
                (LiveBefore + LiveAfter) * SlotIndex::InstrDist,
            1);
        // Would this split be possible to allocate?
        // Never allocate all gaps, we wouldn't be making progress.
        LLVM_DEBUG(dbgs() << " w=" << EstWeight);
        if (EstWeight * Hysteresis >= MaxGap) {
          Shrink = false;
          float Diff = EstWeight - MaxGap;
          if (Diff > BestDiff) {
            LLVM_DEBUG(dbgs() << " (best)");
            BestDiff = Hysteresis * Diff;
            BestBefore = SplitBefore;
            BestAfter = SplitAfter;
          }
        }
      }

      // Try to shrink.
      if (Shrink) {
        if (++SplitBefore < SplitAfter) {
          LLVM_DEBUG(dbgs() << " shrink\n");
          // Recompute the max when necessary.
          if (GapWeight[SplitBefore - 1] >= MaxGap) {
            MaxGap = GapWeight[SplitBefore];
            for (unsigned I = SplitBefore + 1; I != SplitAfter; ++I)
              MaxGap = std::max(MaxGap, GapWeight[I]);
          }
          continue;
        }
        MaxGap = 0;
      }

      // Try to extend the interval.
      if (SplitAfter >= NumGaps) {
        LLVM_DEBUG(dbgs() << " end\n");
        break;
      }

      LLVM_DEBUG(dbgs() << " extend\n");
      MaxGap = std::max(MaxGap, GapWeight[SplitAfter++]);
    }
  }

  // Didn't find any candidates?
  if (BestBefore == NumGaps)
    return MCRegister();

  LLVM_DEBUG(dbgs() << "Best local split range: " << Uses[BestBefore] << '-'
                    << Uses[BestAfter] << ", " << BestDiff << ", "
                    << (BestAfter - BestBefore + 1) << " instrs\n");

  LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  SE->reset(LREdit);

  SE->openIntv();
  SlotIndex SegStart = SE->enterIntvBefore(Uses[BestBefore]);
  SlotIndex SegStop  = SE->leaveIntvAfter(Uses[BestAfter]);
  SE->useIntv(SegStart, SegStop);
  SmallVector<unsigned, 8> IntvMap;
  SE->finish(&IntvMap);
  DebugVars->splitRegister(VirtReg.reg(), LREdit.regs(), *LIS);
  // If the new range has the same number of instructions as before, mark it as
  // RS_Split2 so the next split will be forced to make progress. Otherwise,
  // leave the new intervals as RS_New so they can compete.
  bool LiveBefore = BestBefore != 0 || BI.LiveIn;
  bool LiveAfter = BestAfter != NumGaps || BI.LiveOut;
  unsigned NewGaps = LiveBefore + BestAfter - BestBefore + LiveAfter;
  if (NewGaps >= NumGaps) {
    LLVM_DEBUG(dbgs() << "Tagging non-progress ranges:");
    assert(!ProgressRequired && "Didn't make progress when it was required.");
    for (unsigned I = 0, E = IntvMap.size(); I != E; ++I)
      if (IntvMap[I] == 1) {
        ExtraInfo->setStage(LIS->getInterval(LREdit.get(I)), RS_Split2);
        LLVM_DEBUG(dbgs() << ' ' << printReg(LREdit.get(I)));
      }
    LLVM_DEBUG(dbgs() << '\n');
  }
  //++NumLocalSplits;

  return MCRegister();
}

static unsigned getNumAllocatableRegsForConstraints(
    const MachineInstr *MI, Register Reg, const TargetRegisterClass *SuperRC,
    const TargetInstrInfo *TII, const TargetRegisterInfo *TRI,
    const RegisterClassInfo &RCI) {
  assert(SuperRC && "Invalid register class");

  const TargetRegisterClass *ConstrainedRC =
      MI->getRegClassConstraintEffectForVReg(Reg, SuperRC, TII, TRI,
                                             /* ExploreBundle */ true);
  if (!ConstrainedRC)
    return 0;
  return RCI.getNumAllocatableRegs(ConstrainedRC);
}

static bool readsLaneSubset(const MachineRegisterInfo &MRI,
                            const MachineInstr *MI, const LiveInterval &VirtReg,
                            const TargetRegisterInfo *TRI, SlotIndex Use,
                            const TargetInstrInfo *TII) {
  // Early check the common case. Beware of the semi-formed bundles SplitKit
  // creates by setting the bundle flag on copies without a matching BUNDLE.

  auto DestSrc = TII->isCopyInstr(*MI);
  if (DestSrc && !MI->isBundled() &&
      DestSrc->Destination->getSubReg() == DestSrc->Source->getSubReg())
    return false;

  // FIXME: We're only considering uses, but should be consider defs too?
  LaneBitmask ReadMask = getInstReadLaneMask(MRI, *TRI, *MI, VirtReg.reg());

  LaneBitmask LiveAtMask;
  for (const LiveInterval::SubRange &S : VirtReg.subranges()) {
    if (S.liveAt(Use))
      LiveAtMask |= S.LaneMask;
  }

  // If the live lanes aren't different from the lanes used by the instruction,
  // this doesn't help.
  return (ReadMask & ~(LiveAtMask & TRI->getCoveringLanes())).any();
}

MCRegister RACustom::tryInstructionSplit(const LiveInterval &VirtReg,
                                         AllocationOrder &Order,
                                         SmallVectorImpl<Register> &NewVRegs) {
  const TargetRegisterClass *CurRC = MRI->getRegClass(VirtReg.reg());
  // There is no point to this if there are no larger sub-classes.

  bool SplitSubClass = true;
  if (!RegClassInfo.isProperSubClass(CurRC)) {
    if (!VirtReg.hasSubRanges())
      return MCRegister();
    SplitSubClass = false;
  }

  // Always enable split spill mode, since we're effectively spilling to a
  // register.
  LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  SE->reset(LREdit, SplitEditor::SM_Size);

  ArrayRef<SlotIndex> Uses = SA->getUseSlots();
  if (Uses.size() <= 1)
    return MCRegister();

  LLVM_DEBUG(dbgs() << "Split around " << Uses.size()
                    << " individual instrs.\n");

  const TargetRegisterClass *SuperRC =
      TRI->getLargestLegalSuperClass(CurRC, *MF);
  unsigned SuperRCNumAllocatableRegs =
      RegClassInfo.getNumAllocatableRegs(SuperRC);
  // Split around every non-copy instruction if this split will relax
  // the constraints on the virtual register.
  // Otherwise, splitting just inserts uncoalescable copies that do not help
  // the allocation.
  for (const SlotIndex Use : Uses) {
    if (const MachineInstr *MI = Indexes->getInstructionFromIndex(Use)) {
      if (TII->isFullCopyInstr(*MI) ||
          (SplitSubClass &&
           SuperRCNumAllocatableRegs ==
               getNumAllocatableRegsForConstraints(MI, VirtReg.reg(), SuperRC,
                                                   TII, TRI, RegClassInfo)) ||
          // TODO: Handle split for subranges with subclass constraints?
          (!SplitSubClass && VirtReg.hasSubRanges() &&
           !readsLaneSubset(*MRI, MI, VirtReg, TRI, Use, TII))) {
        LLVM_DEBUG(dbgs() << "    skip:\t" << Use << '\t' << *MI);
        continue;
      }
    }
    SE->openIntv();
    SlotIndex SegStart = SE->enterIntvBefore(Use);
    SlotIndex SegStop = SE->leaveIntvAfter(Use);
    SE->useIntv(SegStart, SegStop);
  }

  if (LREdit.empty()) {
    LLVM_DEBUG(dbgs() << "All uses were copies.\n");
    return MCRegister();
  }

  SmallVector<unsigned, 8> IntvMap;
  SE->finish(&IntvMap);
  DebugVars->splitRegister(VirtReg.reg(), LREdit.regs(), *LIS);
  // Assign all new registers to RS_Spill. This was the last chance.
  ExtraInfo->setStage(LREdit.begin(), LREdit.end(), RS_Spill);
  return MCRegister();
}

MCRegister RACustom::tryRegionSplit(const LiveInterval &VirtReg,
                                    AllocationOrder &Order,
                                    SmallVectorImpl<Register> &NewVRegs) {
  if (!TRI->shouldRegionSplitForVirtReg(*MF, VirtReg))
    return MCRegister::NoRegister;
  unsigned NumCands = 0;
  BlockFrequency SpillCost = calcSpillCost();
  BlockFrequency BestCost;

  // Check if we can split this live range around a compact region.
  bool HasCompact = calcCompactRegion(GlobalCand.front());
  if (HasCompact) {
    // Yes, keep GlobalCand[0] as the compact region candidate.
    NumCands = 1;
    BestCost = BlockFrequency::max();
  } else {
    // No benefit from the compact region, our fallback will be per-block
    // splitting. Make sure we find a solution that is cheaper than spilling.
    BestCost = SpillCost;
    LLVM_DEBUG(dbgs() << "Cost of isolating all blocks = "
                      << printBlockFreq(*MBFI, BestCost) << '\n');
  }

  unsigned BestCand = calculateRegionSplitCost(VirtReg, Order, BestCost,
                                               NumCands, false /*IgnoreCSR*/);

  // No solutions found, fall back to single block splitting.
  if (!HasCompact && BestCand == NoCand)
    return MCRegister::NoRegister;

  return doRegionSplit(VirtReg, BestCand, HasCompact, NewVRegs);
}

MCRegister RACustom::trySplit(const LiveInterval &VirtReg,
                              AllocationOrder &Order,
                              SmallVectorImpl<Register> &NewVRegs,
                              const SmallVirtRegSet &FixedRegisters) {
  // Ranges must be Split2 or less.
  if (ExtraInfo->getStage(VirtReg) >= RS_Spill)
    return MCRegister();

  // Local intervals are handled separately.
  if (LIS->intervalIsInOneMBB(VirtReg)) {
    NamedRegionTimer T("local_split", "Local Splitting", TimerGroupName,
                       TimerGroupDescription, TimePassesIsEnabled);
    SA->analyze(&VirtReg);
    MCRegister PhysReg = tryLocalSplit(VirtReg, Order, NewVRegs);
    if (PhysReg || !NewVRegs.empty())
      return PhysReg;
    return tryInstructionSplit(VirtReg, Order, NewVRegs);
  }

  NamedRegionTimer T("global_split", "Global Splitting", TimerGroupName,
                     TimerGroupDescription, TimePassesIsEnabled);

  SA->analyze(&VirtReg);

  // First try to split around a region spanning multiple blocks. RS_Split2
  // ranges already made dubious progress with region splitting, so they go
  // straight to single block splitting.
  if (ExtraInfo->getStage(VirtReg) < RS_Split2) {
    MCRegister PhysReg = tryRegionSplit(VirtReg, Order, NewVRegs);
    if (PhysReg || !NewVRegs.empty())
      return PhysReg;
  }

  // Then isolate blocks.
  return tryBlockSplit(VirtReg, Order, NewVRegs);
}

void RACustom::onUnassigned(LiveInterval &VirtReg,
                            SmallVectorImpl<Register> &NewVRegs,
                            SmallVirtRegSet &FixedRegisters)
{
  auto Order =
      AllocationOrder::create(VirtReg.reg(), *VRM, RegClassInfo, Matrix);

  LiveRangeStage Stage = ExtraInfo->getStage(VirtReg);

  // The first time we see a live range, don't try to split or spill.
  // Wait until the second time, when all smaller ranges have been allocated.
  // This gives a better picture of the interference to split around.
  if (Stage < RS_Split) {
    ExtraInfo->setStage(VirtReg, RS_Split);
    LLVM_DEBUG(dbgs() << "wait for second round\n");
    NewVRegs.push_back(VirtReg.reg());
    //return MCRegister();
  }

  if (Stage < RS_Spill && !VirtReg.empty()) {
    // Try splitting VirtReg or interferences.
    unsigned NewVRegSizeBefore = NewVRegs.size();
    MCRegister PhysReg = trySplit(VirtReg, Order, NewVRegs, FixedRegisters);
    //if (PhysReg || (NewVRegs.size() - NewVRegSizeBefore))
      //return PhysReg;
  }

  // If we couldn't allocate a register from spilling, there is probably some
  // invalid inline assembly. The base class will report it.
  if (Stage >= RS_Done || !VirtReg.isSpillable()) {
    return tryLastChanceRecoloring(VirtReg, Order, NewVRegs, FixedRegisters,
                                   RecolorStack, Depth);
  }

  // Finally spill VirtReg itself.
  NamedRegionTimer T("spill", "Spiller", TimerGroupName,
                     TimerGroupDescription, TimePassesIsEnabled);
  //RegAllocCounter::addSpillage(&VirtReg);
  LiveRangeEdit LRE(&VirtReg, NewVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  spiller().spill(LRE, &Order);
  ExtraInfo->setStage(NewVRegs.begin(), NewVRegs.end(), RS_Done);

  // Tell LiveDebugVariables about the new ranges. Ranges not being covered by
  // the new regs are kept in LDV (still mapping to the old register), until
  // we rewrite spilled locations in LDV at a later stage.
  for (Register r : spiller().getSpilledRegs())
    DebugVars->splitRegister(r, LRE.regs(), *LIS);
  for (Register r : spiller().getReplacedRegs())
    DebugVars->splitRegister(r, LRE.regs(), *LIS);

  if (VerifyEnabled)
    MF->verify(LIS, Indexes, "After spilling", &errs());
}
#endif

bool RACustom::iterateSolution(SmallVectorImpl<Register> &SplitVRegs) {
  std::vector<MCRegister> PhysRegs;
  std::vector<const LiveInterval *> VirtRegIntervals;

  ++IterationCount;

  //Matrix->invalidateVirtRegs();

  while(true)
  {
      const LiveInterval *Interval = dequeue();
      if(!Interval) break;
      if(MRI->reg_nodbg_empty(Interval->reg()))
      {
          aboutToRemoveInterval(*Interval);
          LIS->removeInterval(Interval->reg());
          //QueueBack.push_back(Interval);
      }
      else if(VRM->hasPhys(Interval->reg()))
      {
      }
      else
      {
          VirtRegIntervals.push_back(Interval);
      }
  }

  if(VirtRegIntervals.empty()) return true;

  for(const LiveInterval *Interval : VirtRegIntervals)
  {
      Register VirtReg = Interval->reg();

      AllocationOrder PhysRegsToAdd =
          AllocationOrder::create(VirtReg, *VRM, RegClassInfo, Matrix);

      for(MCRegister PhysRegToAdd : PhysRegsToAdd)
      {
          if(Matrix->checkInterference(*Interval, PhysRegToAdd) ==
             LiveRegMatrix::IK_Free)
          {
              MCRegister SuperRegister = PhysRegToAdd;
              auto SuperRegisters = TRI->superregs(SuperRegister);
              while(!SuperRegisters.empty())
              {
                  SuperRegister = *SuperRegisters.begin();
                  SuperRegisters = TRI->superregs(SuperRegister);
              }

              bool Exists = false;
              for(MCRegister PhysRegToCheck : PhysRegs)
              {
                  if(PhysRegToCheck.id() == SuperRegister.id())
                  {
                      Exists = true;
                      break;
                  }
              }

              if(!Exists)
              {
                  PhysRegs.push_back(SuperRegister);
                  //PhysRegNames.push_back(TRI->getName(SuperRegister));
              }
          }
      }
  }

  unsigned PhysRegCount = PhysRegs.size();
  unsigned VirtRegCount = VirtRegIntervals.size();

  if(PhysRegCount == 0)
  {
      for(const LiveInterval *Interval : VirtRegIntervals)
      {
          enqueue(Interval);
      }

      return false;
  }

  RegInterferenceGraph Graph(PhysRegCount, VirtRegCount);

  for(unsigned VirtRegIndex = 0;
      VirtRegIndex < VirtRegCount;
      ++VirtRegIndex)
  {
      Graph.setWeight(VirtRegIndex, VirtRegIntervals[VirtRegIndex]->weight());
      Graph.setSpillable(VirtRegIndex, VirtRegIntervals[VirtRegIndex]->isSpillable());
  }

  for(unsigned PhysRegIndexA = 0;
      PhysRegIndexA < PhysRegCount;
      ++PhysRegIndexA)
  {
      unsigned VertIndexA = Graph.physIndexToVertIndex(PhysRegIndexA);

      for(unsigned PhysRegIndexB = PhysRegIndexA + 1;
          PhysRegIndexB < PhysRegCount;
          ++PhysRegIndexB)
      {
          unsigned VertIndexB = Graph.physIndexToVertIndex(PhysRegIndexB);

          Graph.addEdge(VertIndexA, VertIndexB);
      }
  }

  for(unsigned VirtRegIndexA = 0;
      VirtRegIndexA < VirtRegCount;
      ++VirtRegIndexA)
  {
      unsigned VertIndexA = Graph.virtIndexToVertIndex(VirtRegIndexA);

      for(unsigned VirtRegIndexB = VirtRegIndexA + 1;
          VirtRegIndexB < VirtRegCount;
          ++VirtRegIndexB)
      {
          unsigned VertIndexB = Graph.virtIndexToVertIndex(VirtRegIndexB);

          if(VirtRegIntervals[VirtRegIndexA]->empty() ||
             VirtRegIntervals[VirtRegIndexB]->empty() ||
             VirtRegIntervals[VirtRegIndexA]->overlaps(*VirtRegIntervals[VirtRegIndexB]))
          {
              Graph.addEdge(VertIndexA, VertIndexB);
          }
      }
  }

  for(unsigned VirtRegIndex = 0;
      VirtRegIndex < VirtRegCount;
      ++VirtRegIndex)
  {
      unsigned VertIndexA = Graph.virtIndexToVertIndex(VirtRegIndex);

      for(unsigned PhysRegIndex = 0;
          PhysRegIndex < PhysRegCount;
          ++PhysRegIndex)
      {
          bool CanAssign = false;
          bool IsHint = false;

          AllocationOrder PhysRegsAvailable = AllocationOrder::create(VirtRegIntervals[VirtRegIndex]->reg(),
                                                                      *VRM, RegClassInfo, Matrix);
          for(MCRegister PhysReg : PhysRegsAvailable)
          {
              if(TRI->isSuperRegisterEq(PhysReg, PhysRegs[PhysRegIndex]))
              {
                  if(Matrix->checkInterference(*VirtRegIntervals[VirtRegIndex], PhysReg) ==
                     LiveRegMatrix::IK_Free)
                  {
                      if(PhysRegsAvailable.isHint(PhysReg))
                      {
                          IsHint = true;
                      }

                      CanAssign = true;
                      break;
                  }
              }
          }

          if(!CanAssign)
          {
              unsigned VertIndexB = Graph.physIndexToVertIndex(PhysRegIndex);

              Graph.addEdge(VertIndexA, VertIndexB);

              if(IsHint)
              {
                  Graph.addHint(VertIndexA, VertIndexB);
              }
          }
      }
  }

#if 0
  if(IterationCount == 1)
  {
      std::string ModuleName = MF->getFunction().getParent()->getName().str();
      Graph.saveAs(ModuleName + "_" + MF->getName().str());
  }
#endif

  regalloc_graph_solver *Solver = RegAllocBitEASolver;

#define REGALLOC_GRAPH_SOLVER_ENTRY(Name, ID) \
  if(AlgorithmID == #ID) Solver = Name;
  REGALLOC_GRAPH_SOLVER_TABLE
#undef REGALLOC_GRAPH_SOLVER_ENTRY

  std::vector<int> Solution = Solver(Graph);

  unsigned NSpilled = 0;
  unsigned NAssigned = 0;

  for(unsigned VirtIndex = 0;
      VirtIndex < VirtRegCount;
      ++VirtIndex)
  {
      const LiveInterval *VirtReg = VirtRegIntervals[VirtIndex];
      if(Solution[VirtIndex] != -1)
      {
          if(true || NSpilled == 0)
          {
              MCRegister SuperReg = PhysRegs[Solution[VirtIndex]];

              AllocationOrder Order =
                  AllocationOrder::create(VirtReg->reg(), *VRM, RegClassInfo, Matrix);

              for(MCRegister PhysReg : Order)
              {
                  if(TRI->isSuperRegisterEq(PhysReg, SuperReg))
                  {
                      Matrix->assign(*VirtReg, PhysReg);
                      ++NAssigned;
                      break;
                  }
              }
          }
          else
          {
              enqueue(VirtReg);
          }
      }
  }

  for(unsigned VirtIndex = 0;
      VirtIndex < VirtRegCount;
      ++VirtIndex)
  {
      const LiveInterval *VirtReg = VirtRegIntervals[VirtIndex];
      if(!VRM->hasPhys(VirtReg->reg()))
      {
          if(VirtReg->isSpillable())
          {
              RegAllocCounter::addSpillage(VirtReg);
              LiveRangeEdit LRE(VirtReg, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
              spiller().spill(LRE);
              ++NSpilled;
          }
          else
          {
              enqueue(VirtReg);
          }
      }
  }

  std::cout << NAssigned << "/" << VirtRegCount << ", " << NSpilled << std::endl;

  RegAllocCounter::count(MRI, LIS, VRM);

  return ((NSpilled != 0) || (NAssigned != 0));
}

bool RACustom::runOnMachineFunction(MachineFunction &mf) {
  LLVM_DEBUG(dbgs() << "********** BASIC REGISTER ALLOCATION **********\n"
                    << "********** Function: " << mf.getName() << '\n');

  MF = &mf;
  //TII = MF->getSubtarget().getInstrInfo();

  auto &MBFI = getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  auto &LiveStks = getAnalysis<LiveStacksWrapperLegacy>().getLS();
  auto &MDT = getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();

  RegAllocBase::init(getAnalysis<VirtRegMapWrapperLegacy>().getVRM(),
                     getAnalysis<LiveIntervalsWrapperPass>().getLIS(),
                     getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM());
  VirtRegAuxInfo VRAI(*MF, *LIS, *VRM,
                      getAnalysis<MachineLoopInfoWrapperPass>().getLI(), MBFI,
                      &getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI());

#if USE_GREEDY_OUTPUT
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    Register Reg = Register::index2VirtReg(i);
    if (MRI->reg_nodbg_empty(Reg))
      continue;
    if(VRM->hasPhys(Reg))
    {
        Matrix->unassign(LIS->getInterval(Reg));
    }
  }
#endif

  //ExtraInfo.emplace();

  VRAI.calculateSpillWeightsAndHints();

  /*
  SA.reset(new SplitAnalysis(*VRM, *LIS, *Loops));
  SE.reset(new SplitEditor(*SA, *LIS, *VRM, *DomTree, *MBFI, *VRAI));

  SpillPlacer = &MFAM.getResult<SpillPlacementAnalysis>(MF);
  MBFI = &MFAM.getResult<MachineBlockFrequencyAnalysis>(MF);
  DebugVars = &MFAM.getResult<LiveDebugVariablesAnalysis>(MF);
  Indexes = &MFAM.getResult<SlotIndexesAnalysis>(MF);
  */

  SpillerInstance.reset(
      createInlineSpiller({*LIS, LiveStks, MDT, MBFI}, *MF, *VRM, VRAI));

  RegAllocCounter::startFunction(MF);
  std::cout << MF->getName().str() << std::endl;

  StopAlgorithm = false;
  IterationCount = 0;

  /*
  iterateSolution();
  while(iterateSolution());
  while(dequeue());
  */

  allocatePhysRegs();
  postOptimization();

  RegAllocCounter::count(MRI, LIS, VRM);

  // Diagnostic output before rewriting
  LLVM_DEBUG(dbgs() << "Post alloc VirtRegMap:\n" << *VRM << "\n");

  releaseMemory();

  return true;
}
