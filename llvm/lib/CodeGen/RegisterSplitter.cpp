#include "RegisterSplitter.h"
#include "llvm/Support/Debug.h"
#include "llvm/CodeGen/EdgeBundles.h"
#include "AllocationOrder.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "RegAllocBase.h"
#include "llvm/CodeGen/CalcSpillWeights.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

using namespace llvm;

#define DEBUG_TYPE "regalloc"

static cl::opt<SplitEditor::ComplementSpillMode> SplitSpillMode(
    "rs-split-spill-mode", cl::Hidden,
    cl::desc("Spill mode for splitting live ranges"),
    cl::values(clEnumValN(SplitEditor::SM_Partition, "default", "Default"),
               clEnumValN(SplitEditor::SM_Size, "size", "Optimize for size"),
               clEnumValN(SplitEditor::SM_Speed, "speed", "Optimize for speed")),
    cl::init(SplitEditor::SM_Speed));

static cl::opt<unsigned long> GrowRegionComplexityBudget(
    "rs-grow-region-complexity-budget",
    cl::desc("growRegion() does not scale with the number of BB edges, so "
             "limit its budget and bail out once we reach the limit."),
    cl::init(10000), cl::Hidden);

const float Hysteresis = (2007 / 2048.0f); // 0.97998046875

RegisterSplitter::RegisterSplitter(
    ExtraRegInfo *_ExtraInfo,
    LiveIntervals *_LIS,
    SplitAnalysis *_SA,
    SplitEditor *_SE,
    const TargetRegisterInfo *_TRI,
    MachineFunction *_MF,
    SpillPlacement *_SpillPlacer,
    EdgeBundles *_Bundles,
    SlotIndexes *_Indexes,
    MachineLoopInfo *_Loops,
    MachineBlockFrequencyInfo *_MBFI,
    VirtRegMap *_VRM,
    LiveRangeEdit::Delegate *_LREDelegate,
    RegisterClassInfo *_RegClassInfo,
    MachineRegisterInfo *_MRI,
    LiveDebugVariables *_DebugVars,
    LiveRegMatrix *_Matrix,
    const TargetInstrInfo *_TII,
    SmallPtrSet<MachineInstr *, 32> *_DeadRemats
)
{
    ExtraInfo = _ExtraInfo;
    LIS = _LIS;
    SA = _SA;
    SE = _SE;
    TRI = _TRI;
    MF = _MF;
    SpillPlacer = _SpillPlacer;
    Bundles = _Bundles;
    Indexes = _Indexes;
    Loops = _Loops;
    MBFI = _MBFI;
    VRM = _VRM;
    LREDelegate = _LREDelegate;
    RegClassInfo = _RegClassInfo;
    MRI = _MRI;
    DebugVars = _DebugVars;
    Matrix = _Matrix;
    TII = _TII;
    DeadRemats = _DeadRemats;

    IntfCache.init(MF, Matrix->getLiveUnions(), Indexes, LIS, TRI);
    GlobalCand.resize(32);  // This will grow as needed.
}

/// calcSpillCost - Compute how expensive it would be to split the live range in
/// SA around all use blocks instead of forming bundle regions.
BlockFrequency RegisterSplitter::calcSpillCost() {
    BlockFrequency Cost = BlockFrequency(0);
    ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();
    for (const SplitAnalysis::BlockInfo &BI : UseBlocks) {
        unsigned Number = BI.MBB->getNumber();
        // We normally only need one spill instruction - a load or a store.
        Cost += SpillPlacer->getBlockFrequency(Number);

        // Unless the value is redefined in the block.
        if (BI.LiveIn && BI.LiveOut && BI.FirstDef)
            Cost += SpillPlacer->getBlockFrequency(Number);
    }
    return Cost;
}

unsigned RegisterSplitter::calculateRegionSplitCostAroundReg(MCRegister PhysReg,
                                                             AllocationOrder &Order,
                                                             BlockFrequency &BestCost,
                                                             unsigned &NumCands,
                                                             unsigned &BestCand) {
  // Discard bad candidates before we run out of interference cache cursors.
  // This will only affect register classes with a lot of registers (>32).
  if (NumCands == IntfCache.getMaxCursors()) {
    unsigned WorstCount = ~0u;
    unsigned Worst = 0;
    for (unsigned CandIndex = 0; CandIndex != NumCands; ++CandIndex) {
      if (CandIndex == BestCand || !GlobalCand[CandIndex].PhysReg)
        continue;
      unsigned Count = GlobalCand[CandIndex].LiveBundles.count();
      if (Count < WorstCount) {
        Worst = CandIndex;
        WorstCount = Count;
      }
    }
    --NumCands;
    GlobalCand[Worst] = GlobalCand[NumCands];
    if (BestCand == NumCands)
      BestCand = Worst;
  }

  if (GlobalCand.size() <= NumCands)
    GlobalCand.resize(NumCands+1);
  GlobalSplitCandidate &Cand = GlobalCand[NumCands];
  Cand.reset(IntfCache, PhysReg);

  SpillPlacer->prepare(Cand.LiveBundles);
  BlockFrequency Cost;
  if (!addSplitConstraints(Cand.Intf, Cost)) {
    LLVM_DEBUG(dbgs() << printReg(PhysReg, TRI) << "\tno positive bundles\n");
    return BestCand;
  }
  LLVM_DEBUG(dbgs() << printReg(PhysReg, TRI)
                    << "\tstatic = " << printBlockFreq(*MBFI, Cost));
  if (Cost >= BestCost) {
    LLVM_DEBUG({
      if (BestCand == NoCand)
        dbgs() << " worse than no bundles\n";
      else
        dbgs() << " worse than "
               << printReg(GlobalCand[BestCand].PhysReg, TRI) << '\n';
    });
    return BestCand;
  }
  if (!growRegion(Cand)) {
    LLVM_DEBUG(dbgs() << ", cannot spill all interferences.\n");
    return BestCand;
  }

  SpillPlacer->finish();

  // No live bundles, defer to splitSingleBlocks().
  if (!Cand.LiveBundles.any()) {
    LLVM_DEBUG(dbgs() << " no bundles.\n");
    return BestCand;
  }

  Cost += calcGlobalSplitCost(Cand, Order);
  LLVM_DEBUG({
    dbgs() << ", total = " << printBlockFreq(*MBFI, Cost) << " with bundles";
    for (int I : Cand.LiveBundles.set_bits())
      dbgs() << " EB#" << I;
    dbgs() << ".\n";
  });
  if (Cost < BestCost) {
    BestCand = NumCands;
    BestCost = Cost;
  }
  ++NumCands;

  return BestCand;
}

/// calcGlobalSplitCost - Return the global split cost of following the split
/// pattern in LiveBundles. This cost should be added to the local cost of the
/// interference pattern in SplitConstraints.
///
BlockFrequency RegisterSplitter::calcGlobalSplitCost(GlobalSplitCandidate &Cand,
                                                     const AllocationOrder &Order) {
  BlockFrequency GlobalCost = BlockFrequency(0);
  const BitVector &LiveBundles = Cand.LiveBundles;
  ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();
  for (unsigned I = 0; I != UseBlocks.size(); ++I) {
    const SplitAnalysis::BlockInfo &BI = UseBlocks[I];
    SpillPlacement::BlockConstraint &BC = SplitConstraints[I];
    bool RegIn  = LiveBundles[Bundles->getBundle(BC.Number, false)];
    bool RegOut = LiveBundles[Bundles->getBundle(BC.Number, true)];
    unsigned Ins = 0;

    Cand.Intf.moveToBlock(BC.Number);

    if (BI.LiveIn)
      Ins += RegIn != (BC.Entry == SpillPlacement::PrefReg);
    if (BI.LiveOut)
      Ins += RegOut != (BC.Exit == SpillPlacement::PrefReg);
    while (Ins--)
      GlobalCost += SpillPlacer->getBlockFrequency(BC.Number);
  }

  for (unsigned Number : Cand.ActiveBlocks) {
    bool RegIn  = LiveBundles[Bundles->getBundle(Number, false)];
    bool RegOut = LiveBundles[Bundles->getBundle(Number, true)];
    if (!RegIn && !RegOut)
      continue;
    if (RegIn && RegOut) {
      // We need double spill code if this block has interference.
      Cand.Intf.moveToBlock(Number);
      if (Cand.Intf.hasInterference()) {
        GlobalCost += SpillPlacer->getBlockFrequency(Number);
        GlobalCost += SpillPlacer->getBlockFrequency(Number);
      }
      continue;
    }
    // live-in / stack-out or stack-in live-out.
    GlobalCost += SpillPlacer->getBlockFrequency(Number);
  }
  return GlobalCost;
}

unsigned RegisterSplitter::calculateRegionSplitCost(const LiveInterval &VirtReg,
                                                    AllocationOrder &Order,
                                                    BlockFrequency &BestCost,
                                                    unsigned &NumCands,
                                                    bool IgnoreCSR) {
  unsigned BestCand = NoCand;
  for (MCRegister PhysReg : Order) {
    assert(PhysReg);

    bool IsUnusedCalleeSavedReg = false;
    MCRegister CSR = RegClassInfo->getLastCalleeSavedAlias(PhysReg);
    if (CSR)
    {
        IsUnusedCalleeSavedReg =  !Matrix->isPhysRegUsed(PhysReg);
    }

    if (IgnoreCSR && IsUnusedCalleeSavedReg)
      continue;

    calculateRegionSplitCostAroundReg(PhysReg, Order, BestCost, NumCands,
                                      BestCand);
  }

  return BestCand;
}

/// calcCompactRegion - Compute the set of edge bundles that should be live
/// when splitting the current live range into compact regions.  Compact
/// regions can be computed without looking at interference.  They are the
/// regions formed by removing all the live-through blocks from the live range.
///
/// Returns false if the current live range is already compact, or if the
/// compact regions would form single block regions anyway.
bool RegisterSplitter::calcCompactRegion(GlobalSplitCandidate &Cand) {
  // Without any through blocks, the live range is already compact.
  if (!SA->getNumThroughBlocks())
    return false;

  // Compact regions don't correspond to any physreg.
  Cand.reset(IntfCache, MCRegister::NoRegister);

  LLVM_DEBUG(dbgs() << "Compact region bundles");

  // Use the spill placer to determine the live bundles. GrowRegion pretends
  // that all the through blocks have interference when PhysReg is unset.
  SpillPlacer->prepare(Cand.LiveBundles);

  // The static split cost will be zero since Cand.Intf reports no interference.
  BlockFrequency Cost;
  if (!addSplitConstraints(Cand.Intf, Cost)) {
    LLVM_DEBUG(dbgs() << ", none.\n");
    return false;
  }

  if (!growRegion(Cand)) {
    LLVM_DEBUG(dbgs() << ", cannot spill all interferences.\n");
    return false;
  }

  SpillPlacer->finish();

  if (!Cand.LiveBundles.any()) {
    LLVM_DEBUG(dbgs() << ", none.\n");
    return false;
  }

  LLVM_DEBUG({
    for (int I : Cand.LiveBundles.set_bits())
      dbgs() << " EB#" << I;
    dbgs() << ".\n";
  });
  return true;
}

/// addThroughConstraints - Add constraints and links to SpillPlacer from the
/// live-through blocks in Blocks.
bool RegisterSplitter::addThroughConstraints(InterferenceCache::Cursor Intf,
                                             ArrayRef<unsigned> Blocks) {
  const unsigned GroupSize = 8;
  SpillPlacement::BlockConstraint BCS[GroupSize];
  unsigned TBS[GroupSize];
  unsigned B = 0, T = 0;

  for (unsigned Number : Blocks) {
    Intf.moveToBlock(Number);

    if (!Intf.hasInterference()) {
      assert(T < GroupSize && "Array overflow");
      TBS[T] = Number;
      if (++T == GroupSize) {
        SpillPlacer->addLinks(ArrayRef(TBS, T));
        T = 0;
      }
      continue;
    }

    assert(B < GroupSize && "Array overflow");
    BCS[B].Number = Number;

    // Abort if the spill cannot be inserted at the MBB' start
    MachineBasicBlock *MBB = MF->getBlockNumbered(Number);
    auto FirstNonDebugInstr = MBB->getFirstNonDebugInstr();
    if (FirstNonDebugInstr != MBB->end() &&
        SlotIndex::isEarlierInstr(LIS->getInstructionIndex(*FirstNonDebugInstr),
                                  SA->getFirstSplitPoint(Number)))
      return false;
    // Interference for the live-in value.
    if (Intf.first() <= Indexes->getMBBStartIdx(Number))
      BCS[B].Entry = SpillPlacement::MustSpill;
    else
      BCS[B].Entry = SpillPlacement::PrefSpill;

    // Interference for the live-out value.
    if (Intf.last() >= SA->getLastSplitPoint(Number))
      BCS[B].Exit = SpillPlacement::MustSpill;
    else
      BCS[B].Exit = SpillPlacement::PrefSpill;

    if (++B == GroupSize) {
      SpillPlacer->addConstraints(ArrayRef(BCS, B));
      B = 0;
    }
  }

  SpillPlacer->addConstraints(ArrayRef(BCS, B));
  SpillPlacer->addLinks(ArrayRef(TBS, T));
  return true;
}

bool RegisterSplitter::growRegion(GlobalSplitCandidate &Cand) {
  // Keep track of through blocks that have not been added to SpillPlacer.
  BitVector Todo = SA->getThroughBlocks();
  SmallVectorImpl<unsigned> &ActiveBlocks = Cand.ActiveBlocks;
  unsigned AddedTo = 0;
#ifndef NDEBUG
  unsigned Visited = 0;
#endif

  unsigned long Budget = GrowRegionComplexityBudget;
  while (true) {
    ArrayRef<unsigned> NewBundles = SpillPlacer->getRecentPositive();
    // Find new through blocks in the periphery of PrefRegBundles.
    for (unsigned Bundle : NewBundles) {
      // Look at all blocks connected to Bundle in the full graph.
      ArrayRef<unsigned> Blocks = Bundles->getBlocks(Bundle);
      // Limit compilation time by bailing out after we use all our budget.
      if (Blocks.size() >= Budget)
        return false;
      Budget -= Blocks.size();
      for (unsigned Block : Blocks) {
        if (!Todo.test(Block))
          continue;
        Todo.reset(Block);
        // This is a new through block. Add it to SpillPlacer later.
        ActiveBlocks.push_back(Block);
#ifndef NDEBUG
        ++Visited;
#endif
      }
    }
    // Any new blocks to add?
    if (ActiveBlocks.size() == AddedTo)
      break;

    // Compute through constraints from the interference, or assume that all
    // through blocks prefer spilling when forming compact regions.
    auto NewBlocks = ArrayRef(ActiveBlocks).slice(AddedTo);
    if (Cand.PhysReg) {
      if (!addThroughConstraints(Cand.Intf, NewBlocks))
        return false;
    } else {
      // Providing that the variable being spilled does not look like a loop
      // induction variable, which is expensive to spill around and better
      // pushed into a condition inside the loop if possible, provide a strong
      // negative bias on through blocks to prevent unwanted liveness on loop
      // backedges.
      bool PrefSpill = true;
      if (SA->looksLikeLoopIV() && NewBlocks.size() >= 2) {
        // Check that the current bundle is adding a Header + start+end of
        // loop-internal blocks. If the block is indeed a header, don't make
        // the NewBlocks as PrefSpill to allow the variable to be live in
        // Header<->Latch.
        MachineLoop *L = Loops->getLoopFor(MF->getBlockNumbered(NewBlocks[0]));
        if (L && L->getHeader()->getNumber() == (int)NewBlocks[0] &&
            all_of(NewBlocks.drop_front(), [&](unsigned Block) {
              return L == Loops->getLoopFor(MF->getBlockNumbered(Block));
            }))
          PrefSpill = false;
      }
      if (PrefSpill)
        SpillPlacer->addPrefSpill(NewBlocks, /* Strong= */ true);
    }
    AddedTo = ActiveBlocks.size();

    // Perhaps iterating can enable more bundles?
    SpillPlacer->iterate();
  }
  LLVM_DEBUG(dbgs() << ", v=" << Visited);
  return true;
}


/// addSplitConstraints - Fill out the SplitConstraints vector based on the
/// interference pattern in Physreg and its aliases. Add the constraints to
/// SpillPlacement and return the static cost of this split in Cost, assuming
/// that all preferences in SplitConstraints are met.
/// Return false if there are no bundles with positive bias.
bool RegisterSplitter::addSplitConstraints(InterferenceCache::Cursor Intf,
                                           BlockFrequency &Cost) {
  ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();

  // Reset interference dependent info.
  SplitConstraints.resize(UseBlocks.size());
  BlockFrequency StaticCost = BlockFrequency(0);
  for (unsigned I = 0; I != UseBlocks.size(); ++I) {
    const SplitAnalysis::BlockInfo &BI = UseBlocks[I];
    SpillPlacement::BlockConstraint &BC = SplitConstraints[I];

    BC.Number = BI.MBB->getNumber();
    Intf.moveToBlock(BC.Number);
    BC.Entry = BI.LiveIn ? SpillPlacement::PrefReg : SpillPlacement::DontCare;
    BC.Exit = (BI.LiveOut &&
               !LIS->getInstructionFromIndex(BI.LastInstr)->isImplicitDef())
                  ? SpillPlacement::PrefReg
                  : SpillPlacement::DontCare;
    BC.ChangesValue = BI.FirstDef.isValid();

    if (!Intf.hasInterference())
      continue;

    // Number of spill code instructions to insert.
    unsigned Ins = 0;

    // Interference for the live-in value.
    if (BI.LiveIn) {
      if (Intf.first() <= Indexes->getMBBStartIdx(BC.Number)) {
        BC.Entry = SpillPlacement::MustSpill;
        ++Ins;
      } else if (Intf.first() < BI.FirstInstr) {
        BC.Entry = SpillPlacement::PrefSpill;
        ++Ins;
      } else if (Intf.first() < BI.LastInstr) {
        ++Ins;
      }

      // Abort if the spill cannot be inserted at the MBB' start
      if (((BC.Entry == SpillPlacement::MustSpill) ||
           (BC.Entry == SpillPlacement::PrefSpill)) &&
          SlotIndex::isEarlierInstr(BI.FirstInstr,
                                    SA->getFirstSplitPoint(BC.Number)))
        return false;
    }

    // Interference for the live-out value.
    if (BI.LiveOut) {
      if (Intf.last() >= SA->getLastSplitPoint(BC.Number)) {
        BC.Exit = SpillPlacement::MustSpill;
        ++Ins;
      } else if (Intf.last() > BI.LastInstr) {
        BC.Exit = SpillPlacement::PrefSpill;
        ++Ins;
      } else if (Intf.last() > BI.FirstInstr) {
        ++Ins;
      }
    }

    // Accumulate the total frequency of inserted spill code.
    while (Ins--)
      StaticCost += SpillPlacer->getBlockFrequency(BC.Number);
  }
  Cost = StaticCost;

  // Add constraints for use-blocks. Note that these are the only constraints
  // that may add a positive bias, it is downhill from here.
  SpillPlacer->addConstraints(SplitConstraints);
  return SpillPlacer->scanActiveBundles();
}

/// splitAroundRegion - Split the current live range around the regions
/// determined by BundleCand and GlobalCand.
///
/// Before calling this function, GlobalCand and BundleCand must be initialized
/// so each bundle is assigned to a valid candidate, or NoCand for the
/// stack-bound bundles.  The shared SA/SE SplitAnalysis and SplitEditor
/// objects must be initialized for the current live range, and intervals
/// created for the used candidates.
///
/// @param LREdit    The LiveRangeEdit object handling the current split.
/// @param UsedCands List of used GlobalCand entries. Every BundleCand value
///                  must appear in this list.
void RegisterSplitter::splitAroundRegion(LiveRangeEdit &LREdit,
                                         ArrayRef<unsigned> UsedCands) {
  // These are the intervals created for new global ranges. We may create more
  // intervals for local ranges.
  const unsigned NumGlobalIntvs = LREdit.size();
  LLVM_DEBUG(dbgs() << "splitAroundRegion with " << NumGlobalIntvs
                    << " globals.\n");
  assert(NumGlobalIntvs && "No global intervals configured");

  // Isolate even single instructions when dealing with a proper sub-class.
  // That guarantees register class inflation for the stack interval because it
  // is all copies.
  Register Reg = SA->getParent().reg();
  bool SingleInstrs = RegClassInfo->isProperSubClass(MRI->getRegClass(Reg));

  // First handle all the blocks with uses.
  ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();
  for (const SplitAnalysis::BlockInfo &BI : UseBlocks) {
    unsigned Number = BI.MBB->getNumber();
    unsigned IntvIn = 0, IntvOut = 0;
    SlotIndex IntfIn, IntfOut;
    if (BI.LiveIn) {
      unsigned CandIn = BundleCand[Bundles->getBundle(Number, false)];
      if (CandIn != NoCand) {
        GlobalSplitCandidate &Cand = GlobalCand[CandIn];
        IntvIn = Cand.IntvIdx;
        Cand.Intf.moveToBlock(Number);
        IntfIn = Cand.Intf.first();
      }
    }
    if (BI.LiveOut) {
      unsigned CandOut = BundleCand[Bundles->getBundle(Number, true)];
      if (CandOut != NoCand) {
        GlobalSplitCandidate &Cand = GlobalCand[CandOut];
        IntvOut = Cand.IntvIdx;
        Cand.Intf.moveToBlock(Number);
        IntfOut = Cand.Intf.last();
      }
    }

    // Create separate intervals for isolated blocks with multiple uses.
    if (!IntvIn && !IntvOut) {
      LLVM_DEBUG(dbgs() << printMBBReference(*BI.MBB) << " isolated.\n");
      if (SA->shouldSplitSingleBlock(BI, SingleInstrs))
        SE->splitSingleBlock(BI);
      continue;
    }

    if (IntvIn && IntvOut)
      SE->splitLiveThroughBlock(Number, IntvIn, IntfIn, IntvOut, IntfOut);
    else if (IntvIn)
      SE->splitRegInBlock(BI, IntvIn, IntfIn);
    else
      SE->splitRegOutBlock(BI, IntvOut, IntfOut);
  }

  // Handle live-through blocks. The relevant live-through blocks are stored in
  // the ActiveBlocks list with each candidate. We need to filter out
  // duplicates.
  BitVector Todo = SA->getThroughBlocks();
  for (unsigned UsedCand : UsedCands) {
    ArrayRef<unsigned> Blocks = GlobalCand[UsedCand].ActiveBlocks;
    for (unsigned Number : Blocks) {
      if (!Todo.test(Number))
        continue;
      Todo.reset(Number);

      unsigned IntvIn = 0, IntvOut = 0;
      SlotIndex IntfIn, IntfOut;

      unsigned CandIn = BundleCand[Bundles->getBundle(Number, false)];
      if (CandIn != NoCand) {
        GlobalSplitCandidate &Cand = GlobalCand[CandIn];
        IntvIn = Cand.IntvIdx;
        Cand.Intf.moveToBlock(Number);
        IntfIn = Cand.Intf.first();
      }

      unsigned CandOut = BundleCand[Bundles->getBundle(Number, true)];
      if (CandOut != NoCand) {
        GlobalSplitCandidate &Cand = GlobalCand[CandOut];
        IntvOut = Cand.IntvIdx;
        Cand.Intf.moveToBlock(Number);
        IntfOut = Cand.Intf.last();
      }
      if (!IntvIn && !IntvOut)
        continue;
      SE->splitLiveThroughBlock(Number, IntvIn, IntfIn, IntvOut, IntfOut);
    }
  }

  //++NumGlobalSplits;
  ++NumTotalSplits;

  SmallVector<unsigned, 8> IntvMap;
  SE->finish(&IntvMap);
  DebugVars->splitRegister(Reg, LREdit.regs(), *LIS);

  unsigned OrigBlocks = SA->getNumLiveBlocks();

  // Sort out the new intervals created by splitting. We get four kinds:
  // - Remainder intervals should not be split again.
  // - Candidate intervals can be assigned to Cand.PhysReg.
  // - Block-local splits are candidates for local splitting.
  // - DCE leftovers should go back on the queue.
  for (unsigned I = 0, E = LREdit.size(); I != E; ++I) {
    const LiveInterval &Reg = LIS->getInterval(LREdit.get(I));

    // Ignore old intervals from DCE.
    if (ExtraInfo->getOrInitStage(Reg.reg()) != RS_New)
      continue;

    // Remainder interval. Don't try splitting again, spill if it doesn't
    // allocate.
    if (IntvMap[I] == 0) {
      ExtraInfo->setStage(Reg, RS_Spill);
      continue;
    }

    // Global intervals. Allow repeated splitting as long as the number of live
    // blocks is strictly decreasing.
    if (IntvMap[I] < NumGlobalIntvs) {
      if (SA->countLiveBlocks(&Reg) >= OrigBlocks) {
        LLVM_DEBUG(dbgs() << "Main interval covers the same " << OrigBlocks
                          << " blocks as original.\n");
        // Don't allow repeated splitting as a safe guard against looping.
        ExtraInfo->setStage(Reg, RS_Split2);
      }
      continue;
    }

    // Other intervals are treated as new. This includes local intervals created
    // for blocks with multiple uses, and anything created by DCE.
  }

  if (RegAllocBase::VerifyEnabled)
    MF->verify(LIS, Indexes, "After splitting live range around region",
               &errs());
}

MCRegister RegisterSplitter::doRegionSplit(const LiveInterval &VirtReg,
                                           unsigned BestCand, bool HasCompact,
                                           SmallVectorImpl<Register> &NewVRegs) {
  SmallVector<unsigned, 8> UsedCands;
  // Prepare split editor.
  LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, LREDelegate, DeadRemats);
  SE->reset(LREdit, SplitSpillMode);

  // Assign all edge bundles to the preferred candidate, or NoCand.
  BundleCand.assign(Bundles->getNumBundles(), NoCand);

  // Assign bundles for the best candidate region.
  if (BestCand != NoCand) {
    GlobalSplitCandidate &Cand = GlobalCand[BestCand];
    if (unsigned B = Cand.getBundles(BundleCand, BestCand)) {
      UsedCands.push_back(BestCand);
      Cand.IntvIdx = SE->openIntv();
      LLVM_DEBUG(dbgs() << "Split for " << printReg(Cand.PhysReg, TRI) << " in "
                        << B << " bundles, intv " << Cand.IntvIdx << ".\n");
      (void)B;
    }
  }

  // Assign bundles for the compact region.
  if (HasCompact) {
    GlobalSplitCandidate &Cand = GlobalCand.front();
    assert(!Cand.PhysReg && "Compact region has no physreg");
    if (unsigned B = Cand.getBundles(BundleCand, 0)) {
      UsedCands.push_back(0);
      Cand.IntvIdx = SE->openIntv();
      LLVM_DEBUG(dbgs() << "Split for compact region in " << B
                        << " bundles, intv " << Cand.IntvIdx << ".\n");
      (void)B;
    }
  }

  splitAroundRegion(LREdit, UsedCands);
  return MCRegister();
}

MCRegister RegisterSplitter::tryRegionSplit(const LiveInterval &VirtReg,
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

/// calcGapWeights - Compute the maximum spill weight that needs to be evicted
/// in order to use PhysReg between two entries in SA->UseSlots.
///
/// GapWeight[I] represents the gap between UseSlots[I] and UseSlots[I + 1].
///
void RegisterSplitter::calcGapWeights(MCRegister PhysReg,
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
        Matrix->getLiveUnions()[static_cast<unsigned>(Unit)].find(StartIdx);
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

/// tryLocalSplit - Try to split VirtReg into smaller intervals inside its only
/// basic block.
///
MCRegister RegisterSplitter::tryLocalSplit(const LiveInterval &VirtReg,
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

  for (MCRegister PhysReg : Order) {
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

  LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, LREDelegate, DeadRemats);
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
  ++NumTotalSplits;

  return MCRegister();
}

/// Get the number of allocatable registers that match the constraints of \p Reg
/// on \p MI and that are also in \p SuperRC.
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

static LaneBitmask getInstReadLaneMask(const MachineRegisterInfo &MRI,
                                       const TargetRegisterInfo &TRI,
                                       const MachineInstr &FirstMI,
                                       Register Reg) {
  LaneBitmask Mask;
  SmallVector<std::pair<MachineInstr *, unsigned>, 8> Ops;
  (void)AnalyzeVirtRegInBundle(const_cast<MachineInstr &>(FirstMI), Reg, &Ops);

  for (auto [MI, OpIdx] : Ops) {
    const MachineOperand &MO = MI->getOperand(OpIdx);
    assert(MO.isReg() && MO.getReg() == Reg);
    unsigned SubReg = MO.getSubReg();
    if (SubReg == 0 && MO.isUse()) {
      if (MO.isUndef())
        continue;
      return MRI.getMaxLaneMaskForVReg(Reg);
    }

    LaneBitmask SubRegMask = TRI.getSubRegIndexLaneMask(SubReg);
    if (MO.isDef()) {
      if (!MO.isUndef())
        Mask |= ~SubRegMask;
    } else
      Mask |= SubRegMask;
  }

  return Mask;
}

/// Return true if \p MI at \P Use reads a subset of the lanes live in \p
/// VirtReg.
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


/// tryInstructionSplit - Split a live range around individual instructions.
/// This is normally not worthwhile since the spiller is doing essentially the
/// same thing. However, when the live range is in a constrained register
/// class, it may help to insert copies such that parts of the live range can
/// be moved to a larger register class.
///
/// This is similar to spilling to a larger register class.
MCRegister RegisterSplitter::tryInstructionSplit(const LiveInterval &VirtReg,
                                                 AllocationOrder &Order,
                                                 SmallVectorImpl<Register> &NewVRegs) {
  const TargetRegisterClass *CurRC = MRI->getRegClass(VirtReg.reg());
  // There is no point to this if there are no larger sub-classes.

  bool SplitSubClass = true;
  if (!RegClassInfo->isProperSubClass(CurRC)) {
    if (!VirtReg.hasSubRanges())
      return MCRegister();
    SplitSubClass = false;
  }

  // Always enable split spill mode, since we're effectively spilling to a
  // register.
  LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, LREDelegate, DeadRemats);
  SE->reset(LREdit, SplitEditor::SM_Size);

  ArrayRef<SlotIndex> Uses = SA->getUseSlots();
  if (Uses.size() <= 1)
    return MCRegister();

  LLVM_DEBUG(dbgs() << "Split around " << Uses.size()
                    << " individual instrs.\n");

  const TargetRegisterClass *SuperRC =
      TRI->getLargestLegalSuperClass(CurRC, *MF);
  unsigned SuperRCNumAllocatableRegs =
      RegClassInfo->getNumAllocatableRegs(SuperRC);
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
                                                   TII, TRI, *RegClassInfo)) ||
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

/// tryBlockSplit - Split a global live range around every block with uses. This
/// creates a lot of local live ranges, that will be split by tryLocalSplit if
/// they don't allocate.
MCRegister RegisterSplitter::tryBlockSplit(const LiveInterval &VirtReg,
                                           AllocationOrder &Order,
                                           SmallVectorImpl<Register> &NewVRegs) {
  assert(&SA->getParent() == &VirtReg && "Live range wasn't analyzed");
  Register Reg = VirtReg.reg();
  bool SingleInstrs = RegClassInfo->isProperSubClass(MRI->getRegClass(Reg));
  LiveRangeEdit LREdit(&VirtReg, NewVRegs, *MF, *LIS, VRM, LREDelegate, DeadRemats);
  SE->reset(LREdit, SplitSpillMode);
  ArrayRef<SplitAnalysis::BlockInfo> UseBlocks = SA->getUseBlocks();
  for (const SplitAnalysis::BlockInfo &BI : UseBlocks) {
    if (SA->shouldSplitSingleBlock(BI, SingleInstrs))
      SE->splitSingleBlock(BI);
  }
  // No blocks were split.
  if (LREdit.empty())
    return MCRegister();

  // We did split for some blocks.
  SmallVector<unsigned, 8> IntvMap;
  SE->finish(&IntvMap);

  // Tell LiveDebugVariables about the new ranges.
  DebugVars->splitRegister(Reg, LREdit.regs(), *LIS);

  // Sort out the new intervals created by splitting. The remainder interval
  // goes straight to spilling, the new local ranges get to stay RS_New.
  for (unsigned I = 0, E = LREdit.size(); I != E; ++I) {
    const LiveInterval &LI = LIS->getInterval(LREdit.get(I));
    if (ExtraInfo->getOrInitStage(LI.reg()) == RS_New && IntvMap[I] == 0)
      ExtraInfo->setStage(LI, RS_Spill);
  }

  if (RegAllocBase::VerifyEnabled)
    MF->verify(LIS, Indexes, "After splitting live range around basic blocks",
               &errs());
  return MCRegister();
}

/// trySplit - Try to split VirtReg or one of its interferences, making it
/// assignable.
/// @return Physreg when VirtReg may be assigned and/or new NewVRegs.
MCRegister RegisterSplitter::trySplit(const LiveInterval &VirtReg,
                                      AllocationOrder &Order,
                                      SmallVectorImpl<Register> &NewVRegs,
                                      const SmallVirtRegSet &FixedRegisters) {
  // Ranges must be Split2 or less.
  if (ExtraInfo->getStage(VirtReg) >= RS_Spill)
    return MCRegister();

  // Local intervals are handled separately.
  if (LIS->intervalIsInOneMBB(VirtReg)) {
      /*
    NamedRegionTimer T("local_split", "Local Splitting", TimerGroupName,
                       TimerGroupDescription, TimePassesIsEnabled);
                       */
    SA->analyze(&VirtReg);
    MCRegister PhysReg = tryLocalSplit(VirtReg, Order, NewVRegs);
    if (PhysReg || !NewVRegs.empty())
      return PhysReg;
    return tryInstructionSplit(VirtReg, Order, NewVRegs);
  }

  /*
  NamedRegionTimer T("global_split", "Global Splitting", TimerGroupName,
                     TimerGroupDescription, TimePassesIsEnabled);
                     */

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
