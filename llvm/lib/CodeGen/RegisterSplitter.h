#ifndef LLVM_CODEGEN_REGISTERSPLITTER_H_
#define LLVM_CODEGEN_REGISTERSPLITTER_H_

#include "ExtraRegInfo.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "SplitKit.h"
#include "llvm/CodeGen/SpillPlacement.h"
#include "InterferenceCache.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveRegMatrix.h"

namespace llvm
{

class RegisterSplitter
{
    /// Global live range splitting candidate info.
    struct GlobalSplitCandidate {
        // Register intended for assignment, or 0.
        MCRegister PhysReg;

        // SplitKit interval index for this candidate.
        unsigned IntvIdx;

        // Interference for PhysReg.
        InterferenceCache::Cursor Intf;

        // Bundles where this candidate should be live.
        BitVector LiveBundles;
        SmallVector<unsigned, 8> ActiveBlocks;

        void reset(InterferenceCache &Cache, MCRegister Reg) {
            PhysReg = Reg;
            IntvIdx = 0;
            Intf.setPhysReg(Cache, Reg);
            LiveBundles.clear();
            ActiveBlocks.clear();
        }

        // Set B[I] = C for every live bundle where B[I] was NoCand.
        unsigned getBundles(SmallVectorImpl<unsigned> &B, unsigned C) {
            unsigned Count = 0;
            for (unsigned I : LiveBundles.set_bits())
                if (B[I] == NoCand) {
                    B[I] = C;
                    Count++;
                }
            return Count;
        }
    };

    ExtraRegInfo *ExtraInfo = nullptr;
    LiveIntervals *LIS = nullptr;
    SplitAnalysis *SA = nullptr;
    SplitEditor *SE = nullptr;
    const TargetRegisterInfo *TRI = nullptr;
    MachineFunction *MF = nullptr;
    SpillPlacement *SpillPlacer = nullptr;
    EdgeBundles *Bundles = nullptr;
    SlotIndexes *Indexes = nullptr;
    MachineLoopInfo *Loops = nullptr;
    MachineBlockFrequencyInfo *MBFI = nullptr;
    VirtRegMap *VRM = nullptr;
    LiveRangeEdit::Delegate *LREDelegate = nullptr;
    RegisterClassInfo *RegClassInfo = nullptr;
    MachineRegisterInfo *MRI = nullptr;
    LiveDebugVariables *DebugVars = nullptr;
    LiveRegMatrix *Matrix = nullptr;
    const TargetInstrInfo *TII = nullptr;

    /// Candidate map. Each edge bundle is assigned to a GlobalCand entry, or to
    /// NoCand which indicates the stack interval.
    SmallVector<unsigned, 32> BundleCand;

    SmallPtrSet<MachineInstr *, 32> *DeadRemats = nullptr;

    /// All basic blocks where the current register has uses.
    SmallVector<SpillPlacement::BlockConstraint, 8> SplitConstraints;

    /// Cached per-block interference maps
    InterferenceCache IntfCache;

    /// Candidate info for each PhysReg in AllocationOrder.
    /// This vector never shrinks, but grows to the size of the largest register
    /// class.
    SmallVector<GlobalSplitCandidate, 32> GlobalCand;

    enum : unsigned { NoCand = ~0u };

    bool addSplitConstraints(InterferenceCache::Cursor, BlockFrequency &);
    bool addThroughConstraints(InterferenceCache::Cursor, ArrayRef<unsigned>);
    BlockFrequency calcSpillCost();
    bool calcCompactRegion(GlobalSplitCandidate &);
    bool growRegion(GlobalSplitCandidate &Cand);
    void calcGapWeights(MCRegister, SmallVectorImpl<float> &);
    BlockFrequency calcGlobalSplitCost(GlobalSplitCandidate &,
                                       const AllocationOrder &Order);
    /// Calculate cost of region splitting around the specified register.
    unsigned calculateRegionSplitCostAroundReg(MCRegister PhysReg,
                                               AllocationOrder &Order,
                                               BlockFrequency &BestCost,
                                               unsigned &NumCands,
                                               unsigned &BestCand);
    /// Calculate cost of region splitting.
    unsigned calculateRegionSplitCost(const LiveInterval &VirtReg,
                                      AllocationOrder &Order,
                                      BlockFrequency &BestCost,
                                      unsigned &NumCands, bool IgnoreCSR);
    void splitAroundRegion(LiveRangeEdit &, ArrayRef<unsigned>);
    /// Perform region splitting.
    MCRegister doRegionSplit(const LiveInterval &VirtReg, unsigned BestCand,
                             bool HasCompact,
                             SmallVectorImpl<Register> &NewVRegs);
    MCRegister tryRegionSplit(const LiveInterval &, AllocationOrder &,
                              SmallVectorImpl<Register> &);
    MCRegister tryBlockSplit(const LiveInterval &, AllocationOrder &,
                             SmallVectorImpl<Register> &);
    MCRegister tryInstructionSplit(const LiveInterval &, AllocationOrder &,
                                   SmallVectorImpl<Register> &);
    MCRegister tryLocalSplit(const LiveInterval &, AllocationOrder &,
                             SmallVectorImpl<Register> &);

public:
    RegisterSplitter(
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
            );

    MCRegister trySplit(const LiveInterval &, AllocationOrder &,
                        SmallVectorImpl<Register> &, const SmallVirtRegSet &);
};

}

#endif
