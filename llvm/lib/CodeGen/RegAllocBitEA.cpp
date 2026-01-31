#include "RegAllocGraphSolvers.h"
#include "RegAllocBitEA.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/CommandLine.h"

#include <iostream>

static llvm::cl::opt<uint32_t> IterationCount("bitea-iterations", llvm::cl::init(10000), llvm::cl::Hidden, llvm::cl::desc("bitea iteration count"));
static llvm::cl::opt<uint32_t> PopulationCount("bitea-population", llvm::cl::init(100), llvm::cl::Hidden, llvm::cl::desc("bitea population count"));
static llvm::cl::opt<bool> UseHints("bitea-use-hints", llvm::cl::init(false), llvm::cl::Hidden, llvm::cl::desc("use hints in bitea"));

REGALLOC_GRAPH_SOLVER(RegAllocBitEASolver)
{
    unsigned VertexCount = Graph.getVertexCount();
    unsigned PhysCount = Graph.getPhysRegCount();
    unsigned VirtCount = Graph.getVirtRegCount();

    unsigned WordBitCount = 8*(sizeof(uint64_t));
    unsigned WordBitMask = WordBitCount - 1;
    unsigned WordsPerColor = VertexCount / WordBitCount;
    if(VertexCount & WordBitMask)
    {
        ++WordsPerColor;
    }

    std::vector<uint64_t> BestSolution(PhysCount*WordsPerColor);
    float BestSolutionTime;
    int64_t BestFitness;
    int UncoloredCount;

    std::vector<int> Weights(VertexCount);

    for(unsigned VertIndex = 0;
        VertIndex < VertexCount;
        ++VertIndex)
    {
        Weights[VertIndex] = Graph.vertexWeightAsInteger(VertIndex);
    }

    int ColorCount = BitEA(
        VertexCount,
        Graph.getAdjacencyMatrix(),
        UseHints ? Graph.getHintAdjacencyMatrix() : 0,
        Weights.data(),
        PopulationCount,
        PhysCount,
        IterationCount,
        BestSolution.data(),
        &BestFitness,
        &BestSolutionTime,
        &UncoloredCount);

    int RemovedVertices = 0;
    for(int ColorIndex = 0;
        ColorIndex < ColorCount;
        ++ColorIndex)
    {
        bool HasConflicts = true;
        while(HasConflicts)
        {
            HasConflicts = false;
            int VertexToRemove = -1;
            int64_t HighestConflictWeight = 0;
            unsigned PhysIndex;
            for(PhysIndex = 0;
                PhysIndex < PhysCount;
                ++PhysIndex)
            {
                unsigned WordIndex = (PhysIndex / WordBitCount);
                unsigned BitIndex = (PhysIndex % WordBitCount);
                if(BestSolution[WordsPerColor*ColorIndex + WordIndex] & (1LL << BitIndex)) break;
            }
            for(unsigned VertIndexA = Graph.virtIndexToVertIndex(0);
                VertIndexA < VertexCount;
                ++VertIndexA)
            {
                unsigned WordIndexA = (VertIndexA / WordBitCount);
                unsigned BitIndexA = (VertIndexA % WordBitCount);
                if((BestSolution[WordsPerColor*ColorIndex + WordIndexA] & (1LL << BitIndexA)) == 0) continue;
                int64_t ConflictWeight = 0;
                if(Graph.isHint(PhysIndex, VertIndexA))
                {
                    ConflictWeight -= Weights[VertIndexA];
                }
                for(unsigned VertIndexB = 0;
                    VertIndexB < VertexCount;
                    ++VertIndexB)
                {
                    if(VertIndexA == VertIndexB) continue;
                    if(!Graph.hasEdge(VertIndexA, VertIndexB)) continue;
                    unsigned WordIndexB = (VertIndexB / WordBitCount);
                    unsigned BitIndexB = (VertIndexB % WordBitCount);
                    if((BestSolution[WordsPerColor*ColorIndex + WordIndexB] & (1LL << BitIndexB)) == 0) continue;
                    //std::cout << VertIndexA << " " << VertIndexB << std::endl;
                    HasConflicts = true;
                    ConflictWeight += Weights[VertIndexB];
                    if(Graph.isHint(PhysIndex, VertIndexB))
                    {
                        ConflictWeight += Weights[VertIndexB];
                    }
                }
                if(ConflictWeight > HighestConflictWeight)
                {
                    HighestConflictWeight = ConflictWeight;
                    VertexToRemove = VertIndexA;
                }
            }
            if(HasConflicts)
            {
                unsigned WordIndex = (VertexToRemove / WordBitCount);
                unsigned BitIndex = (VertexToRemove % WordBitCount);
                //std::cout << VertexToRemove << std::endl;
                BestSolution[WordsPerColor*ColorIndex + WordIndex] &= ~(1LL << BitIndex);
                ++RemovedVertices;
            }
        }
    }

    /*
    std::cout << "Uncolored Count: " << UncoloredCount << std::endl;
    if(RemovedVertices)
    {
        std::cout << "Removed " << RemovedVertices << " Vertices" << std::endl;
    }
    */

    /*
    int TotalConflicts = 0;
    std::vector<uint64_t> Pool(WordsPerColor, 0);
    for(int ColorIndex = 0;
        ColorIndex < ColorCount;
        ++ColorIndex)
    {
        std::vector<int> ConflictCount(VertexCount, 0);
        int TotalConflictsForColor = count_conflicts(
            VertexCount,
            BestSolution.data() + ColorIndex*WordsPerColor,
            Graph.getAdjacencyMatrix(),
            ConflictCount.data());;

        TotalConflicts += TotalConflictsForColor;

        int PoolTotal = 0;

        fix_conflicts(
            VertexCount,
            Graph.getAdjacencyMatrix(),
            Weights.data(),
            ConflictCount.data(),
            &TotalConflictsForColor,
            BestSolution.data() + ColorIndex*WordsPerColor,
            Pool.data(),
            &PoolTotal);
    }
    */

    std::vector<int> Solution(VirtCount, -1);

    for(int ColorIndex = 0;
        ColorIndex < ColorCount;
        ++ColorIndex)
    {
        int PhysRegIndex = -1;
        for(unsigned WordIndex = 0;
            WordIndex < WordsPerColor;
            ++WordIndex)
        {
            uint64_t Word = BestSolution[ColorIndex*WordsPerColor + WordIndex];
            while(true)
            {
                unsigned BitIndex = llvm::countr_zero(Word);
                if(BitIndex < WordBitCount)
                {
                    Word &= ~(((uint64_t)1) << BitIndex);
                    unsigned VertIndex = BitIndex + WordBitCount*WordIndex;
                    if(Graph.isPhys(VertIndex))
                    {
                        PhysRegIndex = Graph.vertIndexToPhysIndex(VertIndex);
                    }
                    else
                    {
                        unsigned VirtRegIndex = Graph.vertIndexToVirtIndex(VertIndex);
                        /*
                        if(PhysRegIndex == -1)
                        {
                            for(unsigned WordIndex = 0;
                                WordIndex < WordsPerColor;
                                ++WordIndex)
                            {
                                for(unsigned BitIndex = 0;
                                    BitIndex < 64;
                                    ++BitIndex)
                                {
                                    std::cout << ((BestSolution[ColorIndex*WordsPerColor + WordIndex] >> BitIndex) & 1);
                                }
                                std::cout << std::endl;
                            }
                        }
                        assert(PhysRegIndex != -1);
                        */
                        if(PhysRegIndex == -1)
                        {
                            std::cout << "Warning: Color with no physical." << std::endl;
                            break;
                        }
                        assert(!Graph.hasEdge(VertIndex,
                                         Graph.physIndexToVertIndex(PhysRegIndex)));
                        Solution[VirtRegIndex] = PhysRegIndex;
                    }
                }
                else
                {
                    break;
                }
            }
        }
    }

    return Solution;
}
