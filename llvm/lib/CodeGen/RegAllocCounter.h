#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/CodeGen/MachineFunction.h"

class RegAllocCounter
{
    struct RegCount
    {
        unsigned VirtRegCount;
        unsigned AssignedCount;
        unsigned TotalInterval;
        unsigned AssignedInterval;
        float TotalWeight;
        float AssignedWeight;
    };

    static RegCount getRegCount(llvm::MachineRegisterInfo *MRI, llvm::LiveIntervals *LIS, llvm::VirtRegMap *VRM);

    static RegAllocCounter Instance;
    static std::ofstream FileOut;
    static RegCount TotalRegCount;
    static RegCount LastRegCount;

    RegAllocCounter();

public:
    static void startFunction(const llvm::MachineFunction *MF);
    static void count(llvm::MachineRegisterInfo *MRI, llvm::LiveIntervals *LIS, llvm::VirtRegMap *VRM);
    static void updateSpillage(int SpillCount, float SpillWeight);
    ~RegAllocCounter();
};
