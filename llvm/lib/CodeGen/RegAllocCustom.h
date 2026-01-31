#ifndef LLVM_CODEGEN_REGALLOCCUSTOM_H
#define LLVM_CODEGEN_REGALLOCCUSTOM_H

#include "RegAllocBase.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Spiller.h"
#include <queue>

#include "llvm/CodeGen/RegAllocEvictionAdvisor.h"
#include "SplitKit.h"
#include "llvm/CodeGen/SpillPlacement.h"
#include "llvm/CodeGen/LiveDebugVariables.h"

namespace llvm {

struct CompSpillWeight {
  bool operator()(const LiveInterval *A, const LiveInterval *B) const {
    return A->weight() < B->weight();
  }
};

class LLVM_LIBRARY_VISIBILITY RACustom : public MachineFunctionPass,
                                         public RegAllocBase,
                                         private LiveRangeEdit::Delegate {
  // context
  MachineFunction *MF = nullptr;

  // state
  std::unique_ptr<Spiller> SpillerInstance;
  std::priority_queue<const LiveInterval *, std::vector<const LiveInterval *>,
                      CompSpillWeight>
      Queue;

  // Scratch space.  Allocated here to avoid repeated malloc calls in
  // selectOrSplit().
  BitVector UsableRegs;

  bool LRE_CanEraseVirtReg(Register) override;
  void LRE_WillShrinkVirtReg(Register) override;

  bool iterateSolution(SmallVectorImpl<Register> &SplitVRegs);

  std::string AlgorithmID;

  bool StopAlgorithm;

  int IterationCount;

public:
  RACustom(const char *ID, const RegAllocFilterFunc F = nullptr);

  /// Return the pass name.
  StringRef getPassName() const override { return "Custom Register Allocator"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override;

  Spiller &spiller() override { return *SpillerInstance; }

  void enqueueImpl(const LiveInterval *LI) override { Queue.push(LI); }

  const LiveInterval *dequeue() override {
    if (Queue.empty())
      return nullptr;
    const LiveInterval *LI = Queue.top();
    Queue.pop();
    return LI;
  }

  MCRegister selectOrSplit(const LiveInterval &VirtReg,
                           SmallVectorImpl<Register> &SplitVRegs) override;

  /// Perform register allocation.
  bool runOnMachineFunction(MachineFunction &mf) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoPHIs);
  }

  MachineFunctionProperties getClearedProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::IsSSA);
  }

  // Helper for spilling all live virtual registers currently unified under preg
  // that interfere with the most recently queried lvr.  Return true if spilling
  // was successful, and append any new spilled/split intervals to splitLVRs.
  bool spillInterferences(const LiveInterval &VirtReg, MCRegister PhysReg,
                          SmallVectorImpl<Register> &SplitVRegs);

  static char ID;

  /*
private:
  class ExtraRegInfo final {
    // RegInfo - Keep additional information about each live range.
    struct RegInfo {
      LiveRangeStage Stage = RS_New;

      // Cascade - Eviction loop prevention. See
      // canEvictInterferenceBasedOnCost().
      unsigned Cascade = 0;

      RegInfo() = default;
    };

    IndexedMap<RegInfo, VirtReg2IndexFunctor> Info;
    unsigned NextCascade = 1;

  public:
    ExtraRegInfo() {}
    ExtraRegInfo(const ExtraRegInfo &) = delete;

    LiveRangeStage getStage(Register Reg) const { return Info[Reg].Stage; }

    LiveRangeStage getStage(const LiveInterval &VirtReg) const {
      return getStage(VirtReg.reg());
    }

    void setStage(Register Reg, LiveRangeStage Stage) {
      Info.grow(Reg.id());
      Info[Reg].Stage = Stage;
    }

    void setStage(const LiveInterval &VirtReg, LiveRangeStage Stage) {
      setStage(VirtReg.reg(), Stage);
    }

    /// Return the current stage of the register, if present, otherwise
    /// initialize it and return that.
    LiveRangeStage getOrInitStage(Register Reg) {
      Info.grow(Reg.id());
      return getStage(Reg);
    }

    unsigned getCascade(Register Reg) const { return Info[Reg].Cascade; }

    void setCascade(Register Reg, unsigned Cascade) {
      Info.grow(Reg.id());
      Info[Reg].Cascade = Cascade;
    }

    unsigned getOrAssignNewCascade(Register Reg) {
      unsigned Cascade = getCascade(Reg);
      if (!Cascade) {
        Cascade = NextCascade++;
        setCascade(Reg, Cascade);
      }
      return Cascade;
    }

    unsigned getCascadeOrCurrentNext(Register Reg) const {
      unsigned Cascade = getCascade(Reg);
      if (!Cascade)
        Cascade = NextCascade;
      return Cascade;
    }

    template <typename Iterator>
    void setStage(Iterator Begin, Iterator End, LiveRangeStage NewStage) {
      for (; Begin != End; ++Begin) {
        Register Reg = *Begin;
        Info.grow(Reg.id());
        if (Info[Reg].Stage == RS_New)
          Info[Reg].Stage = NewStage;
      }
    }
    void LRE_DidCloneVirtReg(Register New, Register Old);
  };

  std::optional<ExtraRegInfo> ExtraInfo;
  std::unique_ptr<SplitAnalysis> SA;
  std::unique_ptr<SplitEditor> SE;
  SpillPlacement *SpillPlacer = nullptr;
  MachineBlockFrequencyInfo *MBFI = nullptr;
  LiveDebugVariables *DebugVars = nullptr;
  SlotIndexes *Indexes = nullptr;
  const TargetInstrInfo *TII = nullptr;

  MCRegister trySplit(const LiveInterval &, AllocationOrder &,
                      SmallVectorImpl<Register> &, const SmallVirtRegSet &);
  MCRegister tryLocalSplit(const LiveInterval &, AllocationOrder &,
                           SmallVectorImpl<Register> &);
  void calcGapWeights(MCRegister, SmallVectorImpl<float> &);
  MCRegister tryInstructionSplit(const LiveInterval &, AllocationOrder &,
                                 SmallVectorImpl<Register> &);
  */
};
} // namespace llvm
#endif
