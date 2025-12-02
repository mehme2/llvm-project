#include "RegAllocGraphSolvers.h"
#include "RegAllocBitEA.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/CommandLine.h"

#include <iostream>
#include <fstream>

static llvm::cl::opt<uint32_t> IterationCount("bitea-iterations", llvm::cl::init(10000), llvm::cl::Hidden, llvm::cl::desc("bitea iteration count"));
static llvm::cl::opt<uint32_t> PopulationCount("bitea-population", llvm::cl::init(100), llvm::cl::Hidden, llvm::cl::desc("bitea population count"));

void SaveEdgesAndWeights(RegInterferenceGraph Graph, std::vector<int> &Weights)
{
    unsigned VertexCount = Graph.getVertexCount();
    std::ofstream FileOut;
    FileOut.open("example.edgelist");
    for(unsigned VertexA = 0;
        VertexA < VertexCount;
        ++VertexA)
    {
        for(unsigned VertexB = VertexA + 1;
            VertexB < VertexCount;
            ++VertexB)
        {
            if(Graph.hasEdge(VertexA, VertexB))
            {
                FileOut << VertexA << " " << VertexB << std::endl;
            }
        }
    }
    FileOut.close();
    FileOut.open("example.col.w");
    for(int Weight : Weights)
    {
        FileOut << Weight << std::endl;
    }
    FileOut.close();
}

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
    int BestFitness;
    int UncoloredCount;

    std::vector<int> Weights(VertexCount);

    for(unsigned PhysIndex = 0;
        PhysIndex < PhysCount;
        ++PhysIndex)
    {
        Weights[Graph.physIndexToVertIndex(PhysIndex)] = 10000000;
    }

    for(unsigned VirtIndex = 0;
        VirtIndex < VirtCount;
        ++VirtIndex)
    {
        Weights[Graph.virtIndexToVertIndex(VirtIndex)] =
            Graph.isSpillable(VirtIndex) ? 1000000 * Graph.getWeight(VirtIndex) : 10000000;
    }

    int ColorCount = BitEA(
        VertexCount,
        Graph.getAdjacencyMatrix(),
        Weights.data(),
        PopulationCount,
        PhysCount,
        IterationCount,
        BestSolution.data(),
        &BestFitness,
        &BestSolutionTime,
        &UncoloredCount);

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
                        if((PhysRegIndex != -1) &&
                           Graph.hasEdge(VertIndex,
                                         Graph.physIndexToVertIndex(PhysRegIndex)))
                        {
                        }
                        else
                        {
                            Solution[VirtRegIndex] = PhysRegIndex;
                        }
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
