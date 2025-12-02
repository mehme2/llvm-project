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

#include <iostream>

using namespace llvm;

#define DEBUG_TYPE "regalloc"

#define REGALLOC_GRAPH_SOLVER_ENTRY(Name, ID) \
static FunctionPass* createCustomRegisterAllocator_##ID() { \
  return new RACustom(#ID); \
} \
static RegisterRegAlloc CustomRegAlloc_##ID(#ID, #ID, createCustomRegisterAllocator_##ID);
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
    : MachineFunctionPass(ID), RegAllocBase(F), AlgorithmID(AlgoID) {}

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
    ++TotalSpillCount;
    TotalSpillWeight += VirtReg.weight();
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
  enqueue(&VirtReg);
  
  if(!StopAlgorithm && iterateSolution(SplitVRegs) && false)
      return 0;

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

  ++TotalSpillCount;
  TotalSpillWeight += VirtReg.weight();
  LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  spiller().spill(LRE);

  // The live virtual register requesting allocation was spilled, so tell
  // the caller not to allocate anything during this round.
  return 0;
}

bool RACustom::iterateSolution(SmallVectorImpl<Register> &SplitVRegs) {
  std::vector<MCRegister> PhysRegs;
  std::vector<const LiveInterval *> VirtRegIntervals;

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

  unsigned NAssigned = 0;
  unsigned NSpilled = 0;

  if(PhysRegCount)
  {
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

              AllocationOrder PhysRegsAvailable = AllocationOrder::create(VirtRegIntervals[VirtRegIndex]->reg(),
                                                                          *VRM, RegClassInfo, Matrix);
              for(MCRegister PhysReg : PhysRegsAvailable)
              {
                  if(TRI->isSuperRegisterEq(PhysReg, PhysRegs[PhysRegIndex]))
                  {
                      if(Matrix->checkInterference(*VirtRegIntervals[VirtRegIndex], PhysReg) ==
                         LiveRegMatrix::IK_Free)
                      {
                          CanAssign = true;
                          break;
                      }
                  }
              }

              if(!CanAssign)
              {
                  unsigned VertIndexB = Graph.physIndexToVertIndex(PhysRegIndex);

                  Graph.addEdge(VertIndexA, VertIndexB);
              }
          }
      }

      regalloc_graph_solver *Solver = RegAllocBitEASolver;

#define REGALLOC_GRAPH_SOLVER_ENTRY(Name, ID) \
      if(AlgorithmID == #ID) Solver = Name;
      REGALLOC_GRAPH_SOLVER_TABLE
#undef REGALLOC_GRAPH_SOLVER_ENTRY

          std::vector<int> Solution = Solver(Graph);

      for(unsigned VirtIndex = 0;
          VirtIndex < VirtRegCount;
          ++VirtIndex)
      {
          const LiveInterval *VirtReg = VirtRegIntervals[VirtIndex];
          if(Solution[VirtIndex] != -1)
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
              ++TotalSpillCount;
              TotalSpillWeight += VirtReg->weight();
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

  std::cout << NAssigned << std::endl;

  RegAllocCounter::count(MRI, LIS, VRM);
  RegAllocCounter::updateSpillage(TotalSpillCount, TotalSpillWeight);

  return ((NSpilled != 0) || (NAssigned != 0));
}

bool RACustom::runOnMachineFunction(MachineFunction &mf) {
  LLVM_DEBUG(dbgs() << "********** BASIC REGISTER ALLOCATION **********\n"
                    << "********** Function: " << mf.getName() << '\n');

  MF = &mf;
  auto &MBFI = getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  auto &LiveStks = getAnalysis<LiveStacksWrapperLegacy>().getLS();
  auto &MDT = getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();

  RegAllocBase::init(getAnalysis<VirtRegMapWrapperLegacy>().getVRM(),
                     getAnalysis<LiveIntervalsWrapperPass>().getLIS(),
                     getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM());
  VirtRegAuxInfo VRAI(*MF, *LIS, *VRM,
                      getAnalysis<MachineLoopInfoWrapperPass>().getLI(), MBFI,
                      &getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI());
  VRAI.calculateSpillWeightsAndHints();

  SpillerInstance.reset(
      createInlineSpiller({*LIS, LiveStks, MDT, MBFI}, *MF, *VRM, VRAI));

  RegAllocCounter::startFunction(MF);

  TotalSpillWeight = 0.0f;
  TotalSpillCount = 0;

  StopAlgorithm = false;

  /*
  iterateSolution();
  while(iterateSolution());
  while(dequeue());
  */

  allocatePhysRegs();
  postOptimization();

  RegAllocCounter::count(MRI, LIS, VRM);
  RegAllocCounter::updateSpillage(TotalSpillCount, TotalSpillWeight);

  // Diagnostic output before rewriting
  LLVM_DEBUG(dbgs() << "Post alloc VirtRegMap:\n" << *VRM << "\n");

  releaseMemory();

  return true;
}
