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
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "RegisterSplitter.h"
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

static llvm::cl::opt<bool> UnassignUntilComplete("unassign-until-complete", llvm::cl::init(true), llvm::cl::Hidden, llvm::cl::desc("unassign every virtual before reconstructing graph"));

static llvm::cl::opt<unsigned> RegallocSeed("regalloc-seed", llvm::cl::init(0), llvm::cl::Hidden, llvm::cl::desc("srand seed for every function's register allocation"));

static llvm::cl::opt<float> UnassignThreshold("unassign-threshold", llvm::cl::init(1.0f), llvm::cl::Hidden, llvm::cl::desc("unassign threshold"));

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

}

using namespace llvm;

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

namespace llvm {

class RAGraph : private LiveRangeEdit::Delegate
{
    MachineFunction *MF = nullptr;
    const TargetInstrInfo *TII = nullptr;
    MachineRegisterInfo *MRI = nullptr;
    VirtRegMap *VRM = nullptr;
    LiveIntervals *LIS = nullptr;
    LiveRegMatrix *Matrix = nullptr;
    const TargetRegisterInfo *TRI = nullptr;
    LiveDebugVariables *DebugVars = nullptr;
    SlotIndexes *Indexes = nullptr;
    LiveStacks *LSS = nullptr;
    MachineDominatorTree *DomTree = nullptr;
    MachineBlockFrequencyInfo *MBFI = nullptr;
    VirtRegAuxInfo *VRAI = nullptr;
    MachineLoopInfo *Loops = nullptr;
    SpillPlacement *SpillPlacer = nullptr;
    EdgeBundles *Bundles = nullptr;
    SplitAnalysis *SA = nullptr;
    SplitEditor *SE = nullptr;
    RegisterSplitter *Splitter = nullptr;
    Spiller *SpillerInstance = nullptr;

    /// Inst which is a def of an original reg and whose defs are already all
    /// dead after remat is saved in DeadRemats. The deletion of such inst is
    /// postponed till all the allocations are done, so its remat expr is
    /// always available for the remat of all the siblings of the original reg.
    SmallPtrSet<MachineInstr *, 32> DeadRemats;

    ExtraRegInfo *ExtraInfo;

    RegisterClassInfo RegClassInfo;

    regalloc_graph_solver *Solver = RegAllocBitEASolver;

    bool iterate();

    bool LRE_CanEraseVirtReg(Register) override;
    void LRE_WillShrinkVirtReg(Register) override;
    void LRE_DidCloneVirtReg(Register, Register) override;

    bool shouldAssign(Register VirtReg);

    RegInterferenceGraph buildGraph(std::vector<Register> &VirtRegs,
                                    std::vector<MCRegister> &PhysRegs);

    void assignSolution(std::vector<int> &Solution,
                        std::vector<Register> &VirtRegs,
                        std::vector<MCRegister> &PhysRegs);

public:
    RAGraph(std::string AlgorithmID, Pass *P, const RegAllocFilterFunc F = nullptr);
    bool run(MachineFunction &mf);
    void onUnassigned(Register VirtReg);
};

bool RAGraphInit::runOnMachineFunction(MachineFunction &MF) {
  RAGraph Impl(AlgorithmID, this, F);
  return Impl.run(MF);
}

RAGraph::RAGraph(std::string AlgorithmID, Pass *P, const RegAllocFilterFunc F)
{
    VRM = &P->getAnalysis<VirtRegMapWrapperLegacy>().getVRM();
    LIS = &P->getAnalysis<LiveIntervalsWrapperPass>().getLIS();
    Matrix = &P->getAnalysis<LiveRegMatrixWrapperLegacy>().getLRM();
    DebugVars = &P->getAnalysis<LiveDebugVariablesWrapperLegacy>().getLDV();
    Indexes = &P->getAnalysis<SlotIndexesWrapperPass>().getSI();
    LSS = &P->getAnalysis<LiveStacksWrapperLegacy>().getLS();
    DomTree = &P->getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
    MBFI = &P->getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
    Loops = &P->getAnalysis<MachineLoopInfoWrapperPass>().getLI();
    SpillPlacer = &P->getAnalysis<SpillPlacementWrapperLegacy>().getResult();
    Bundles = &P->getAnalysis<EdgeBundlesWrapperLegacy>().getEdgeBundles();

    if(RegallocSeed)
    {
        srand(RegallocSeed);
    }
    else
    {
        srand(time(0));
    }

#define REGALLOC_GRAPH_SOLVER_ENTRY(Name, ID) \
    if(AlgorithmID == #ID) Solver = Name;
    REGALLOC_GRAPH_SOLVER_TABLE
#undef REGALLOC_GRAPH_SOLVER_ENTRY
}

bool RAGraph::LRE_CanEraseVirtReg(Register VirtReg) {
  LiveInterval &LI = LIS->getInterval(VirtReg);
  if (VRM->hasPhys(VirtReg)) {
    Matrix->unassign(LI);
    return true;
  }
  // Unassigned virtreg is probably in the priority queue.
  // RegAllocBase will erase it after dequeueing.
  // Nonetheless, clear the live-range so that the debug
  // dump will show the right state for that VirtReg.
  LI.clear();
  return false;
}

void RAGraph::LRE_WillShrinkVirtReg(Register VirtReg) {
  if (!VRM->hasPhys(VirtReg))
    return;

  // Register is assigned, put it back on the queue for reassignment.
  LiveInterval &LI = LIS->getInterval(VirtReg);
  Matrix->unassign(LI);
}

void RAGraph::LRE_DidCloneVirtReg(Register New, Register Old) {
  ExtraInfo->LRE_DidCloneVirtReg(New, Old);
}

bool RAGraph::shouldAssign(Register VirtReg)
{
    bool Result = (VirtReg.isValid() && VirtReg.isVirtual() &&
                   !MRI->reg_nodbg_empty(VirtReg));

    return Result;
}

RegInterferenceGraph RAGraph::buildGraph(std::vector<Register> &VirtRegs,
                                         std::vector<MCRegister> &PhysRegs)
{
    VirtRegs.resize(0);
    PhysRegs.resize(0);

    Matrix->invalidateVirtRegs();
    unsigned AllVirtRegs = MRI->getNumVirtRegs();

    for(unsigned VirtIndex = 0;
        VirtIndex < AllVirtRegs;
        ++VirtIndex)
    {
        Register VirtReg = Register::index2VirtReg(VirtIndex);
        if(shouldAssign(VirtReg) && !VRM->hasPhys(VirtReg))
        {
            auto Stage = ExtraInfo->getOrInitStage(VirtReg);
            if (Stage == RS_New) {
                Stage = RS_Assign;
                ExtraInfo->setStage(VirtReg, Stage);
            }
            VirtRegs.push_back(VirtReg);
        }
    }

    for(Register VirtReg : VirtRegs)
    {
        AllocationOrder Order = AllocationOrder::create(VirtReg, *VRM, RegClassInfo, Matrix);

        LiveInterval *Interval = &LIS->getInterval(VirtReg);

        for(MCRegister PhysRegToAdd : Order)
        {
          if(Matrix->checkInterference(*Interval, PhysRegToAdd) ==
             LiveRegMatrix::IK_Free)
          {
              /*
              MCRegister SuperRegister = PhysRegToAdd;
              auto SuperRegisters = TRI->superregs(SuperRegister);
              while(!SuperRegisters.empty())
              {
                  SuperRegister = *SuperRegisters.begin();
                  SuperRegisters = TRI->superregs(SuperRegister);
              }
              */

              bool Exists = false;
              for(MCRegister PhysRegToCheck : PhysRegs)
              {
                  if(TRI->regsOverlap(PhysRegToCheck, PhysRegToAdd))
                  {
                      Exists = true;
                      break;
                  }
              }

              if(!Exists)
              {
                  PhysRegs.push_back(PhysRegToAdd);
              }
          }
        }
    }

    unsigned PhysRegCount = PhysRegs.size();
    unsigned VirtRegCount = VirtRegs.size();

    RegInterferenceGraph Graph(PhysRegCount, VirtRegCount);

    for(unsigned VirtRegIndex = 0;
        VirtRegIndex < VirtRegCount;
        ++VirtRegIndex)
    {
        Register VirtReg = VirtRegs[VirtRegIndex];
        LiveInterval *Interval = &LIS->getInterval(VirtReg);
        Graph.setWeight(VirtRegIndex, Interval->weight());
        bool Spillable = (Interval->isSpillable() && (ExtraInfo->getStage(VirtReg) < RS_Done));
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

            LiveInterval *IntervalA = &LIS->getInterval(VirtRegs[VirtRegIndexA]);
            LiveInterval *IntervalB = &LIS->getInterval(VirtRegs[VirtRegIndexB]);

            if(!(IntervalA->empty() || IntervalB->empty()) &&
               IntervalA->overlaps(*IntervalB))
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

        Register VirtReg = VirtRegs[VirtRegIndex];
        LiveInterval *Interval = &LIS->getInterval(VirtReg);

        AllocationOrder Order = AllocationOrder::create(VirtReg, *VRM, RegClassInfo, Matrix);

        for(unsigned PhysRegIndex = 0;
            PhysRegIndex < PhysRegCount;
            ++PhysRegIndex)
        {
            bool CanAssign = false;
            bool IsHint = false;

            for(MCRegister PhysReg : Order)
            {
                if(TRI->regsOverlap(PhysReg, PhysRegs[PhysRegIndex]))
                {
                    if(Matrix->checkInterference(*Interval, PhysReg) == LiveRegMatrix::IK_Free)
                    {
                        if(Order.isHint(PhysReg))
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

    return Graph;
}

void RAGraph::assignSolution(std::vector<int> &Solution,
                             std::vector<Register> &VirtRegs,
                             std::vector<MCRegister> &PhysRegs)
{
    unsigned VirtRegCount = VirtRegs.size();

    for(unsigned VirtIndex = 0;
        VirtIndex < VirtRegCount;
        ++VirtIndex)
    {
        Register VirtReg = VirtRegs[VirtIndex];
        if(Solution[VirtIndex] != -1)
        {
            MCRegister SuperReg = PhysRegs[Solution[VirtIndex]];

            AllocationOrder Order = AllocationOrder::create(VirtReg, *VRM, RegClassInfo, Matrix);

            bool IsAssigned = false;

            for(MCRegister PhysReg : Order)
            {
                if(TRI->regsOverlap(PhysReg, SuperReg))
                {
                    assert(PhysReg.isValid());
                    //std::cerr << TRI->getName(PhysReg) << ',' << TRI->getName(SuperReg) << std::endl;
                    LiveInterval *Interval = &LIS->getInterval(VirtReg);
                    Matrix->assign(*Interval, PhysReg);
                    IsAssigned = true;
                    break;
                }
            }

            assert(IsAssigned);
        }
    }
}

void RAGraph::onUnassigned(Register VirtReg)
{
  SmallVirtRegSet FixedRegisters;
  using VirtRegVec = SmallVector<Register, 4>;
  VirtRegVec NewVRegs;

  LiveInterval *Interval = &LIS->getInterval(VirtReg);

  if(UncoloredBehavior == RAGraphUncoloredBehavior::Greedy)
  {
      assert("greedy option unsupported");
      /*
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
      */
  }

  auto Order = AllocationOrder::create(VirtReg, *VRM, RegClassInfo, Matrix);

  LiveRangeStage Stage = ExtraInfo->getStage(VirtReg);

  if(UncoloredBehavior == RAGraphUncoloredBehavior::Split)
  {
      // The first time we see a live range, don't try to split or spill.
      // Wait until the second time, when all smaller ranges have been allocated.
      // This gives a better picture of the interference to split around.
      if (Stage < RS_Split) {
          ExtraInfo->setStage(VirtReg, RS_Split);
          //LLVM_DEBUG(dbgs() << "wait for second round\n");
          return;
      }

      if (Stage < RS_Spill && !Interval->empty()) {
          // Try splitting VirtReg or interferences.
          MCRegister PhysReg = Splitter->trySplit(*Interval, Order, NewVRegs, FixedRegisters);

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

          return;
      }
  }

  if(Interval->empty())
  {
      ExtraInfo->setStage(VirtReg, RS_Done);
      Stage = RS_Done;
  }

  // If we couldn't allocate a register from spilling, there is probably some
  // invalid inline assembly. The base class will report it.
  if ((Stage != RS_Spill) || (!Interval->isSpillable())) {
      /*
         return tryLastChanceRecoloring(VirtReg, Order, NewVRegs, FixedRegisters,
         RecolorStack, Depth);
         */
      if(Stage < RS_Spill)
      {
          ExtraInfo->setStage(VirtReg, RS_Spill);
      }
      return;
  }

  // Finally spill VirtReg itself.
  /*
  NamedRegionTimer T("spill", "Spiller", TimerGroupName,
                     TimerGroupDescription, TimePassesIsEnabled);
                     */
  //std::cout << "Spilling " << VirtReg << std::endl;
  RegAllocCounter::addSpillage(Interval);
  LiveRangeEdit LRE(Interval, NewVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  SpillerInstance->spill(LRE, &Order);
  ExtraInfo->setStage(NewVRegs.begin(), NewVRegs.end(), RS_Done);

  // Tell LiveDebugVariables about the new ranges. Ranges not being covered by
  // the new regs are kept in LDV (still mapping to the old register), until
  // we rewrite spilled locations in LDV at a later stage.
  for (Register r : SpillerInstance->getSpilledRegs())
    DebugVars->splitRegister(r, LRE.regs(), *LIS);
  for (Register r : SpillerInstance->getReplacedRegs())
    DebugVars->splitRegister(r, LRE.regs(), *LIS);

  if (RegAllocBase::VerifyEnabled)
    MF->verify(LIS, Indexes, "After spilling", &errs());
}

bool RAGraph::iterate()
{
    std::vector<Register> VirtRegs;
    std::vector<MCRegister> PhysRegs;

    RegInterferenceGraph Graph = buildGraph(VirtRegs, PhysRegs);

    unsigned PhysRegCount = PhysRegs.size();
    unsigned VirtRegCount = VirtRegs.size();

    if(VirtRegCount == 0) return false;

    if(PhysRegCount == 0)
    {
        for(Register VirtReg : VirtRegs)
        {
            onUnassigned(VirtReg);
        }

        return true;
    }

    unsigned NAssigned = 0;
    unsigned NUncolored = VirtRegCount;

#if 0
    if(false)
    {
        RegInterferenceGraph RestrictedGraph = Graph;
        RestrictedGraph.addHintsAsEdges();

        std::vector<int> Solution = Solver(RestrictedGraph);

        unsigned IterationAssignCount = 0;
        for(int Index : Solution) if(Index != -1) ++IterationAssignCount;

        assignSolution(Solution, VirtRegs, PhysRegs);

        std::cout << "Assigned " << IterationAssignCount << " hints" << std::endl;

        NAssigned += IterationAssignCount;
        NUncolored -= IterationAssignCount;

        if(IterationAssignCount)
        {
            Graph = buildGraph(VirtRegs, PhysRegs);

            PhysRegCount = PhysRegs.size();
            VirtRegCount = VirtRegs.size();
        }

        if(VirtRegCount == 0) return false;

        if(PhysRegCount == 0)
        {
            for(Register VirtReg : VirtRegs)
            {
                onUnassigned(VirtReg);
            }

            return true;
        }
    }
#endif

    std::vector<int> Solution = Solver(Graph);

    unsigned IterationAssignCount = 0;
    for(int Index : Solution) if(Index != -1) ++IterationAssignCount;

    NAssigned += IterationAssignCount;
    NUncolored -= IterationAssignCount;

#if 0
    if(NUncolored == 0)
    {
        for(unsigned PhysIndex = 0;
            PhysIndex < PhysRegCount;
            ++PhysIndex)
        {
            MCRegister SuperReg = PhysRegs[PhysIndex];
            MCRegister CSRAlias = RegClassInfo.getLastCalleeSavedAlias(SuperReg);
            if(CSRAlias)
            {
                std::cout << "Trying removing CSR" << std::endl;
                RegInterferenceGraph RestrictedGraph = Graph;
                RestrictedGraph.blockVertex(RestrictedGraph.physIndexToVertIndex(PhysIndex));

                std::vector<int> RestrictedSolution = Solver(RestrictedGraph);

                unsigned RestrictedAssignCount = 0;
                for(int Index : RestrictedSolution) if(Index != -1) ++RestrictedAssignCount;

                if(RestrictedAssignCount == IterationAssignCount)
                {
                    std::cout << "Success\n";
                    Solution = RestrictedSolution;
                    Graph = RestrictedGraph;
                }
            }
        }
    }
#endif

    assignSolution(Solution, VirtRegs, PhysRegs);

    unsigned AllVirtRegs = MRI->getNumVirtRegs();

    for(unsigned VirtIndex = 0;
        VirtIndex < AllVirtRegs;
        ++VirtIndex)
    {
        Register VirtReg = Register::index2VirtReg(VirtIndex);
        if(shouldAssign(VirtReg))
        {
            if(!VRM->hasPhys(VirtReg))
            {
                onUnassigned(VirtReg);
            }
        }
    }

    bool DidUnassign = false;

    if(UnassignUntilComplete &&
       (NUncolored != 0) &&
       ((float)NAssigned < (UnassignThreshold*(float)Solution.size())))
    {
        unsigned AllVirtRegs = MRI->getNumVirtRegs();

        for(unsigned VirtIndex = 0;
            VirtIndex < AllVirtRegs;
            ++VirtIndex)
        {
            Register VirtReg = Register::index2VirtReg(VirtIndex);
            if(shouldAssign(VirtReg))
            {
                if(VRM->hasPhys(VirtReg))
                {
                    LiveInterval *Interval = &LIS->getInterval(VirtReg);
                    Matrix->unassign(*Interval);
                    DidUnassign = true;
                }
            }
        }
    }

    std::cout << NAssigned << "/" << VirtRegCount << std::endl;

    RegAllocCounter::count(MRI, LIS, VRM);

    if (RegAllocBase::VerifyEnabled)
        MF->verify(LIS, Indexes, "After iteration", &errs());

    return ((NUncolored != 0) || DidUnassign);
}

bool RAGraph::run(MachineFunction &mf)
{
    MF = &mf;

    RegAllocCounter::startFunction(MF);

    std::cout << mf.getName().str() << std::endl;

    TII = MF->getSubtarget().getInstrInfo();
    TRI = &VRM->getTargetRegInfo();
    MRI = &VRM->getRegInfo();
    MRI->freezeReservedRegs();
    RegClassInfo.runOnMachineFunction(VRM->getMachineFunction());

    Indexes->packIndexes();

    VRAI = new VirtRegAuxInfo(*MF, *LIS, *VRM, *Loops, *MBFI);
    VRAI->calculateSpillWeightsAndHints();

    SpillerInstance = createInlineSpiller({*LIS, *LSS, *DomTree, *MBFI}, *MF,
                                          *VRM, *VRAI, Matrix);

    SA = new SplitAnalysis(*VRM, *LIS, *Loops);
    SE = new SplitEditor(*SA, *LIS, *VRM, *DomTree, *MBFI, *VRAI);

    DeadRemats.clear();

    ExtraInfo = new ExtraRegInfo();

    Splitter = new RegisterSplitter(ExtraInfo,
                                    LIS,
                                    SA,
                                    SE,
                                    TRI,
                                    MF,
                                    SpillPlacer,
                                    Bundles,
                                    Indexes,
                                    Loops,
                                    MBFI,
                                    VRM,
                                    this,
                                    &RegClassInfo,
                                    MRI,
                                    DebugVars,
                                    Matrix,
                                    TII,
                                    &DeadRemats
                                   );

    while(iterate());

    RegAllocCounter::count(MRI, LIS, VRM);

    SpillerInstance->postOptimization();
    for (auto *DeadInst : DeadRemats) {
        LIS->RemoveMachineInstrFromMaps(*DeadInst);
        DeadInst->eraseFromParent();
    }

    delete VRAI;
    delete SA;
    delete Splitter;
    delete SpillerInstance;
    delete ExtraInfo;

    return true;
}

}
