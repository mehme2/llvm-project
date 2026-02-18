#include "RegAllocGreedy.h"
#include "RegAllocCounter.h"
#include "RegInterferenceGraph.h"
#include "AllocationOrder.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/Support/Timer.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/EdgeBundles.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "iostream"
#include "llvm/Support/CommandLine.h"
#include <cassert>

#include "RegAllocGraphSolvers.h"

namespace llvm
{

enum RAGraphUncoloredBehavior
{
    SpillOnly,
    Split,
    Greedy
};

static cl::opt<RAGraphUncoloredBehavior> UncoloredBehavior("uncolored-behavior", cl::Hidden,
                                                           cl::values(clEnumValN(RAGraphUncoloredBehavior::SpillOnly, "spill-only", "Split only"),
                                                                      clEnumValN(RAGraphUncoloredBehavior::Split, "split", "Try to split first"),
                                                                      clEnumValN(RAGraphUncoloredBehavior::Greedy, "greedy", "Greedy allocator behavior")),
                                                           cl::init(RAGraphUncoloredBehavior::SpillOnly));

static llvm::cl::opt<bool> UnassignUntilComplete("unassign-until-complete", llvm::cl::init(false), llvm::cl::Hidden, llvm::cl::desc("unassign every virtual before reconstructing graph"));

static llvm::cl::opt<unsigned> RegallocSeed("regalloc-seed", llvm::cl::init(0), llvm::cl::Hidden, llvm::cl::desc("srand seed for every function's register allocation"));

struct RAGreedy::RequiredAnalyses {
  VirtRegMap *VRM = nullptr;
  LiveIntervals *LIS = nullptr;
  LiveRegMatrix *LRM = nullptr;
  SlotIndexes *Indexes = nullptr;
  MachineBlockFrequencyInfo *MBFI = nullptr;
  MachineDominatorTree *DomTree = nullptr;
  MachineLoopInfo *Loops = nullptr;
  MachineOptimizationRemarkEmitter *ORE = nullptr;
  EdgeBundles *Bundles = nullptr;
  SpillPlacement *SpillPlacer = nullptr;
  LiveDebugVariables *DebugVars = nullptr;

  // Used by InlineSpiller
  LiveStacks *LSS;
  // Proxies for eviction and priority advisors
  RegAllocEvictionAdvisorProvider *EvictProvider;
  RegAllocPriorityAdvisorProvider *PriorityProvider;

  RequiredAnalyses() = delete;
  RequiredAnalyses(Pass &P);
  RequiredAnalyses(MachineFunction &MF, MachineFunctionAnalysisManager &MFAM);
};

class RAGraphInit : public MachineFunctionPass {
  RegAllocFilterFunc F;

  const char *AlgorithmID;

public:
  RAGraphInit(const char *AlgoID, const RegAllocFilterFunc F = nullptr);

  static char ID;
  /// Return the pass name.
  StringRef getPassName() const override { return "Graph Register Allocator"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  /// Perform register allocation.
  bool runOnMachineFunction(MachineFunction &mf) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().setNoPHIs();
  }

  MachineFunctionProperties getClearedProperties() const override {
    return MachineFunctionProperties().setIsSSA();
  }
};

RAGraphInit::RAGraphInit(const char *AlgoID, const RegAllocFilterFunc F)
    : MachineFunctionPass(ID), F(std::move(F)), AlgorithmID(AlgoID) {
  initializeRAGraphInitPass(*PassRegistry::getPassRegistry());
}

void RAGraphInit::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
  AU.addPreserved<MachineBlockFrequencyInfoWrapperPass>();
  AU.addRequired<LiveIntervalsWrapperPass>();
  AU.addPreserved<LiveIntervalsWrapperPass>();
  AU.addRequired<SlotIndexesWrapperPass>();
  AU.addPreserved<SlotIndexesWrapperPass>();
  AU.addRequired<LiveDebugVariablesWrapperLegacy>();
  AU.addPreserved<LiveDebugVariablesWrapperLegacy>();
  AU.addRequired<LiveStacksWrapperLegacy>();
  AU.addPreserved<LiveStacksWrapperLegacy>();
  AU.addRequired<MachineDominatorTreeWrapperPass>();
  AU.addPreserved<MachineDominatorTreeWrapperPass>();
  AU.addRequired<MachineLoopInfoWrapperPass>();
  AU.addPreserved<MachineLoopInfoWrapperPass>();
  AU.addRequired<VirtRegMapWrapperLegacy>();
  AU.addPreserved<VirtRegMapWrapperLegacy>();
  AU.addRequired<LiveRegMatrixWrapperLegacy>();
  AU.addPreserved<LiveRegMatrixWrapperLegacy>();
  AU.addRequired<EdgeBundlesWrapperLegacy>();
  AU.addRequired<SpillPlacementWrapperLegacy>();
  AU.addRequired<MachineOptimizationRemarkEmitterPass>();
  AU.addRequired<RegAllocEvictionAdvisorAnalysisLegacy>();
  AU.addRequired<RegAllocPriorityAdvisorAnalysisLegacy>();
  MachineFunctionPass::getAnalysisUsage(AU);
}

#define REGALLOC_GRAPH_SOLVER_ENTRY(Name, ID) \
static FunctionPass* createCustomRegisterAllocator_##ID() { \
  return new RAGraphInit(#ID); \
} \
static RegisterRegAlloc CustomRegAlloc_##ID(#ID, #ID, createCustomRegisterAllocator_##ID);
  REGALLOC_GRAPH_SOLVER_TABLE
#undef REGALLOC_GRAPH_SOLVER_ENTRY

char RAGraphInit::ID = 0;

INITIALIZE_PASS_BEGIN(RAGraphInit, "graph", "Graph Register Allocator",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LiveDebugVariablesWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(SlotIndexesWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervalsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(RegisterCoalescerLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineSchedulerLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveStacksWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(VirtRegMapWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrixWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(EdgeBundlesWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(SpillPlacementWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineOptimizationRemarkEmitterPass)
INITIALIZE_PASS_DEPENDENCY(RegAllocEvictionAdvisorAnalysisLegacy)
INITIALIZE_PASS_DEPENDENCY(RegAllocPriorityAdvisorAnalysisLegacy)
INITIALIZE_PASS_END(RAGraphInit, "graph", "Graph Register Allocator",
                    false, false)

class RAGraph : public RAGreedy
{
public:
  RAGraph(const char *ID, RequiredAnalyses &Analyses, const RegAllocFilterFunc F = nullptr);

  MCRegister selectOrSplit(const LiveInterval &VirtReg,
                           SmallVectorImpl<Register> &SplitVRegs) override;

  bool run(MachineFunction &mf);

private:
  bool iterateSolution(SmallVectorImpl<Register> &SplitVRegs);

  MCRegister onUnassigned(const LiveInterval &VirtReg);

  bool StopAlgorithm;
  int IterationCount;

  std::string AlgorithmID;

  MachineFunction *MF;
};

bool RAGraphInit::runOnMachineFunction(MachineFunction &MF) {
  RAGreedy::RequiredAnalyses Analyses(*this);
  RAGraph Impl(AlgorithmID, Analyses, F);
  return Impl.run(MF);
}

RAGraph::RAGraph(const char *AlgoID, RequiredAnalyses &Analyses, RegAllocFilterFunc F)
    : RAGreedy(Analyses, F), AlgorithmID(AlgoID)
{
    if(RegallocSeed)
    {
        srand(RegallocSeed);
    }
    else
    {
        srand(time(0));
    }
}

MCRegister RAGraph::onUnassigned(const LiveInterval &VirtReg)
{

  SmallVirtRegSet FixedRegisters;
  using VirtRegVec = SmallVector<Register, 4>;
  VirtRegVec NewVRegs;

  if(UncoloredBehavior == RAGraphUncoloredBehavior::Greedy)
  {
      MCRegister PhysReg = RAGreedy::selectOrSplit(VirtReg, NewVRegs);
      if(PhysReg && (PhysReg != ~0u))
      {
          Matrix->assign(VirtReg, PhysReg);
      }
      else
      {
          //RegAllocBase::enqueue(&VirtReg);
      }

      for(Register Reg : NewVRegs)
      {
          RegAllocBase::enqueue(&LIS->getInterval(Reg));
      }
      return MCRegister();
  }

  auto Order =
      AllocationOrder::create(VirtReg.reg(), *VRM, RegClassInfo, Matrix);

  LiveRangeStage Stage = ExtraInfo->getStage(VirtReg);

  if(UncoloredBehavior == RAGraphUncoloredBehavior::Split)
  {
      // The first time we see a live range, don't try to split or spill.
      // Wait until the second time, when all smaller ranges have been allocated.
      // This gives a better picture of the interference to split around.
      if (Stage < RS_Split) {
          ExtraInfo->setStage(VirtReg, RS_Split);
          //LLVM_DEBUG(dbgs() << "wait for second round\n");
          RegAllocBase::enqueue(&VirtReg);
          return MCRegister();
      }

      if (Stage < RS_Spill && !VirtReg.empty()) {
          // Try splitting VirtReg or interferences.
          unsigned NewVRegSizeBefore = NewVRegs.size();
          MCRegister PhysReg = trySplit(VirtReg, Order, NewVRegs, FixedRegisters);
          for(Register Reg : NewVRegs)
          {
              RegAllocBase::enqueue(&LIS->getInterval(Reg));
          }

          LiveRangeStage NewStage = ExtraInfo->getStage(VirtReg);
          if(NewStage == Stage)
          {
              LiveRangeStage StageToSet = RS_Done;
              if(NewStage == RS_Split)
              {
                  StageToSet = RS_Split2;
              }
              if(NewStage == RS_Split2)
              {
                  StageToSet = RS_Spill;
              }
              ExtraInfo->setStage(VirtReg, StageToSet);
          }

          if((NewVRegs.size() == NewVRegSizeBefore))
          {
              RegAllocBase::enqueue(&VirtReg);
          }

          return MCRegister();
      }
  }

  if(VirtReg.empty())
  {
      ExtraInfo->setStage(VirtReg, RS_Done);
      Stage = RS_Done;
  }

  // If we couldn't allocate a register from spilling, there is probably some
  // invalid inline assembly. The base class will report it.
  if ((Stage != RS_Spill || !VirtReg.isSpillable())) {
      /*
         return tryLastChanceRecoloring(VirtReg, Order, NewVRegs, FixedRegisters,
         RecolorStack, Depth);
         */
      if(Stage < RS_Spill)
      {
          ExtraInfo->setStage(VirtReg, RS_Spill);
      }
      NewVRegs.push_back(VirtReg.reg());
      for(Register Reg : NewVRegs)
      {
          RegAllocBase::enqueue(&LIS->getInterval(Reg));
      }
      return MCRegister();
  }

  // Finally spill VirtReg itself.
  NamedRegionTimer T("spill", "Spiller", TimerGroupName,
                     TimerGroupDescription, TimePassesIsEnabled);
  RegAllocCounter::addSpillage(&VirtReg);
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

  for(Register Reg : NewVRegs)
  {
      RegAllocBase::enqueue(&LIS->getInterval(Reg));
  }
  return MCRegister();
}

bool RAGraph::iterateSolution(SmallVectorImpl<Register> &SplitVRegs) {
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
      /*
      else if((!Interval->reg().isVirtual()) || VRM->hasPhys(Interval->reg()))
      {
      }
      */
      else
      {
          bool Exists = false;
          for(auto *Test : VirtRegIntervals)
          {
              if(Test == Interval)
              {
                  Exists = true;
              }
          }
          if(!Exists)
          {
              VirtRegIntervals.push_back(Interval);
          }
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
      int NSpilled = 0;
      for(const LiveInterval *Interval : VirtRegIntervals)
      {
          if(false && !Interval->isSpillable())
          {
              RegAllocBase::enqueue(Interval);
          }
          else
          {
              MCRegister PhysReg = onUnassigned(*Interval);
              if(PhysReg) Matrix->assign(*Interval, PhysReg);
              ++NSpilled;
          }
      }

      //std::cout << VirtRegIntervals.size() << "!" << std::endl;

      return (NSpilled != 0);
  }

  RegInterferenceGraph Graph(PhysRegCount, VirtRegCount);

  for(unsigned VirtRegIndex = 0;
      VirtRegIndex < VirtRegCount;
      ++VirtRegIndex)
  {
      auto *Interval = VirtRegIntervals[VirtRegIndex];
      Graph.setWeight(VirtRegIndex, Interval->weight());
      bool Spillable = (Interval->isSpillable() && (ExtraInfo->getStage(Interval->reg()) < RS_Done));
      Graph.setSpillable(VirtRegIndex, Spillable);
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

          if(!(VirtRegIntervals[VirtRegIndexA]->empty() ||
               VirtRegIntervals[VirtRegIndexB]->empty()) &&
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
          MCRegister SuperReg = PhysRegs[Solution[VirtIndex]];

          AllocationOrder Order =
              AllocationOrder::create(VirtReg->reg(), *VRM, RegClassInfo, Matrix);

          bool IsAssigned = false;

          for(MCRegister PhysReg : Order)
          {
              if(TRI->isSuperRegisterEq(PhysReg, SuperReg))
              {
                  assert(PhysReg.isValid());
                  Matrix->assign(*VirtReg, PhysReg);
                  ++NAssigned;
                  IsAssigned = true;
                  break;
              }
          }

          assert(IsAssigned);
      }
  }

  for(unsigned VirtIndex = 0;
      VirtIndex < VirtRegCount;
      ++VirtIndex)
  {
      const LiveInterval *VirtReg = VirtRegIntervals[VirtIndex];
      if(Solution[VirtIndex] == -1)
      {
          /*
             RegAllocCounter::addSpillage(VirtReg);
             LiveRangeEdit LRE(VirtReg, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
             spiller().spill(LRE);
             */
          if(true || VirtReg->isSpillable())
          {
              MCRegister PhysReg = onUnassigned(*VirtReg);
              if(PhysReg) Matrix->assign(*VirtReg, PhysReg);
              ++NSpilled;
          }
          else
          {
              RegAllocBase::enqueue(VirtReg);
          }
      }
  }

  if(UnassignUntilComplete && (NSpilled != 0))
  {
      for(unsigned VirtIndex = 0;
          VirtIndex < VirtRegCount;
          ++VirtIndex)
      {
          const LiveInterval *VirtReg = VirtRegIntervals[VirtIndex];
          if(Solution[VirtIndex] != -1)
          {
              if(VirtReg->reg().isVirtual() && VRM->hasPhys(VirtReg->reg()))
              {
                  Matrix->unassign(*VirtReg);
                  RegAllocBase::enqueue(VirtReg);
              }
          }
      }
  }

  std::cout << NAssigned << "/" << VirtRegCount << ", " << NSpilled << std::endl;

  RegAllocCounter::count(MRI, LIS, VRM);

  return ((NSpilled != 0) || (NAssigned != 0));
}

MCRegister RAGraph::selectOrSplit(const LiveInterval &VirtReg,
                                  SmallVectorImpl<Register> &SplitVRegs) {
  if(!StopAlgorithm)
  {
      RegAllocBase::enqueue(&VirtReg);
      if(iterateSolution(SplitVRegs))
          return 0;

      std::cout << "Stop" << std::endl;
  }

  bool StoppedThisIteration = !StopAlgorithm;
  StopAlgorithm = true;

  if(StoppedThisIteration)
      return 0;

  return RAGreedy::selectOrSplit(VirtReg, SplitVRegs);
}

bool RAGraph::run(MachineFunction &mf)
{
  MF = &mf;

  //RegAllocCounter::startFunction(&mf);
  std::cout << mf.getName().str() << std::endl;

  StopAlgorithm = false;
  IterationCount = 0;

  return RAGreedy::run(mf);
}

}
