#include "RegAllocCounter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"

#include <iostream>
#include <fstream>

std::ofstream RegAllocCounter::FileOut;
RegAllocCounter RegAllocCounter::Instance;
static std::string CurrentModule;
RegAllocCounter::RegCount RegAllocCounter::TotalRegCount = {};
RegAllocCounter::RegCount RegAllocCounter::LastRegCount = {};

float TotalSpillWeight = 0;
int TotalSpillCount = 0;
float LastSpillWeight = 0;
int LastSpillCount = 0;

RegAllocCounter::RegCount RegAllocCounter::getRegCount(llvm::MachineRegisterInfo *MRI, llvm::LiveIntervals *LIS, llvm::VirtRegMap *VRM)
{
    RegAllocCounter::RegCount Result = {};
    unsigned VirtRegCountAll = MRI->getNumVirtRegs();
    for(unsigned VirtRegIndex = 0;
        VirtRegIndex < VirtRegCountAll;
        ++VirtRegIndex)
    {
        llvm::Register VirtReg = llvm::Register::index2VirtReg(VirtRegIndex);

        if(!MRI->reg_nodbg_empty(VirtReg) &&
           LIS->hasInterval(VirtReg))
        {
            ++Result.VirtRegCount;
            auto &LI = LIS->getInterval(VirtReg);
            unsigned Interval = LI.getSize();
            float Weight = LI.weight();
            Result.TotalInterval += Interval;
            if(LI.isSpillable())
            {
                Result.TotalWeight += Weight;
            }
            if(VRM->hasPhys(VirtReg))
            {
                ++Result.AssignedCount;
                Result.AssignedInterval += Interval;
                if(LI.isSpillable())
                {
                    Result.AssignedWeight += Weight;
                }
            }
        }
    }

    return Result;
}

void RegAllocCounter::startFunction(const llvm::MachineFunction *MF)
{
    std::string ModuleName = MF->getFunction().getParent()->getName().str();
    if(CurrentModule.compare(ModuleName))
    {
        CurrentModule = ModuleName;
        FileOut.open(ModuleName + "_regcount.txt");
    }

    FileOut << "=====================================" << std::endl;
    FileOut << MF->getName().str() << std::endl;

    TotalRegCount.VirtRegCount += LastRegCount.VirtRegCount;
    TotalRegCount.AssignedCount += LastRegCount.AssignedCount;
    TotalRegCount.AssignedWeight += LastRegCount.AssignedWeight;
    TotalSpillCount += LastSpillCount;
    TotalSpillWeight += LastSpillWeight;

    LastRegCount = {};
    LastSpillCount = 0;
    LastSpillWeight = 0;
}

void RegAllocCounter::count(llvm::MachineRegisterInfo *MRI, llvm::LiveIntervals *LIS, llvm::VirtRegMap *VRM)
{
    RegCount Result = getRegCount(MRI, LIS, VRM);
    LastRegCount = Result;

    FileOut << "-------------------------------------" << std::endl;
    FileOut << " - Total Register Count: " << Result.VirtRegCount << std::endl;
    FileOut << " - Assigned Register Count: " << Result.AssignedCount << std::endl;
    FileOut << " - Assigned Weight: " << Result.AssignedWeight << std::endl;
}

void RegAllocCounter::updateSpillage(int SpillCount, float SpillWeight)
{
    LastSpillCount = SpillCount;
    LastSpillWeight = SpillWeight;
    FileOut << " - Total Spill Count: " << SpillCount << std::endl;
    FileOut << " - Total Spill Weight: " << SpillWeight << std::endl;
}

RegAllocCounter::RegAllocCounter()
{
}

RegAllocCounter::~RegAllocCounter()
{
    TotalRegCount.VirtRegCount += LastRegCount.VirtRegCount;
    TotalRegCount.AssignedCount += LastRegCount.AssignedCount;
    TotalRegCount.AssignedWeight += LastRegCount.AssignedWeight;
    TotalSpillCount += LastSpillCount;
    TotalSpillWeight += LastSpillWeight;

    FileOut << "=====================================" << std::endl;
    FileOut << "Total" << std::endl;
    FileOut << "-------------------------------------" << std::endl;
    FileOut << " - Total Register Count: " << TotalRegCount.VirtRegCount << std::endl;
    FileOut << " - Assigned Register Count: " << TotalRegCount.AssignedCount << std::endl;
    FileOut << " - Assigned Weight: " << TotalRegCount.AssignedWeight << std::endl;
    FileOut << " - Total Spill Count: " << TotalSpillCount << std::endl;
    FileOut << " - Total Spill Weight: " << TotalSpillWeight << std::endl;
    FileOut.close();
}
