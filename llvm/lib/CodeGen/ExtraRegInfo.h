#ifndef LLVM_CODEGEN_EXTRAREGINFO_H_
#define LLVM_CODEGEN_EXTRAREGINFO_H_

#include "llvm/CodeGen/RegAllocEvictionAdvisor.h"
#include "llvm/ADT/IndexedMap.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/LiveInterval.h"

namespace llvm
{

class ExtraRegInfo {
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

}

#endif
