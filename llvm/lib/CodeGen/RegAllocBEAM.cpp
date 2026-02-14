#include "RegAllocGraphSolvers.h"
#include "RegAllocBEAM.h"
#include "llvm/Support/CommandLine.h"
#include <iostream>

static llvm::cl::opt<uint32_t> IterationCount("beam-iterations", llvm::cl::init(10000), llvm::cl::Hidden, llvm::cl::desc("beam iteration count"));
static llvm::cl::opt<uint32_t> PopulationCount("beam-population", llvm::cl::init(16), llvm::cl::Hidden, llvm::cl::desc("beam population count"));
static llvm::cl::opt<uint32_t> ThreadCount("beam-threads", llvm::cl::init(1), llvm::cl::Hidden, llvm::cl::desc("beam thread count"));

REGALLOC_GRAPH_SOLVER(RegAllocBEAMSolver)
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

    std::vector<weight_t> Weights(VertexCount);

    for(unsigned VertIndex = 0;
        VertIndex < VertexCount;
        ++VertIndex)
    {
        if(Graph.isPhys(VertIndex))
        {
            //Weights[VertIndex] = llvm::huge_valf;
            Weights[VertIndex] = 10000000.0f;
        }
        else
        {
            unsigned VirtIndex = Graph.vertIndexToVirtIndex(VertIndex);
            if(Graph.isSpillable(VirtIndex))
            {
                Weights[VertIndex] = Graph.getWeight(VirtIndex);
            }
            else Weights[VertIndex] = 10000.0f;
        }
    }

    beam_graph_t BEAMGraph;
    BEAMGraph.edge_mat = (uint64_t *)Graph.getAdjacencyMatrix();
    BEAMGraph.weights = Weights.data();
    BEAMGraph.size = VertexCount;

    beam_individual_t BEAMSolution;

    graph_color_beam (
        &BEAMGraph,
        PopulationCount,
        PhysCount, 
        IterationCount, 
        ThreadCount,
        &BEAMSolution
        );

    std::cout << "Best Fitness: " << BEAMSolution.fitness << std::endl;

    /*
    int RemovedVertices = 0;
    for(int ColorIndex = 0;
        ColorIndex < BEAMSolution.color_num;
        ++ColorIndex)
    {
        bool HasConflicts = true;
        while(HasConflicts)
        {
            HasConflicts = false;
            int VertexToRemove = -1;
            weight_t HighestConflictWeight = 0;
            unsigned PhysIndex;
            for(PhysIndex = 0;
                PhysIndex < PhysCount;
                ++PhysIndex)
            {
                unsigned WordIndex = (PhysIndex / WordBitCount);
                unsigned BitIndex = (PhysIndex % WordBitCount);
                if(BEAMSolution.color_mat[WordsPerColor*ColorIndex + WordIndex] & (1LL << BitIndex)) break;
            }
            if(PhysIndex == PhysCount) continue;
            for(unsigned VertIndexA = Graph.virtIndexToVertIndex(0);
                VertIndexA < VertexCount;
                ++VertIndexA)
            {
                unsigned WordIndexA = (VertIndexA / WordBitCount);
                unsigned BitIndexA = (VertIndexA % WordBitCount);
                if((BEAMSolution.color_mat[WordsPerColor*ColorIndex + WordIndexA] & (1LL << BitIndexA)) == 0) continue;
                weight_t ConflictWeight = 0;
                if(UseHints && Graph.isHint(PhysIndex, VertIndexA))
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
                    if((BEAMSolution.color_mat[WordsPerColor*ColorIndex + WordIndexB] & (1LL << BitIndexB)) == 0) continue;
                    //std::cout << VertIndexA << " " << VertIndexB << std::endl;
                    HasConflicts = true;
                    ConflictWeight += Weights[VertIndexB];
                    if(UseHints && Graph.isHint(PhysIndex, VertIndexB))
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
                BEAMSolution.color_mat[WordsPerColor*ColorIndex + WordIndex] &= ~(1LL << BitIndex);
                ++RemovedVertices;
            }
        }
    }
    */

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
        ColorIndex < BEAMSolution.color_num;
        ++ColorIndex)
    {
        int PhysRegIndex = -1;
        for(unsigned WordIndex = 0;
            WordIndex < WordsPerColor;
            ++WordIndex)
        {
            uint64_t Word = BEAMSolution.color_mat[ColorIndex*WordsPerColor + WordIndex];
            while(true)
            {
                unsigned BitIndex = llvm::countr_zero(Word);
                if(BitIndex < WordBitCount)
                {
                    Word &= ~(((uint64_t)1) << BitIndex);
                    unsigned VertIndex = BitIndex + WordBitCount*WordIndex;
                    if(VertIndex >= VertexCount) continue;
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
