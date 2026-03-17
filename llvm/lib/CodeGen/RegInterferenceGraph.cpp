#include "RegInterferenceGraph.h"
#include "llvm/ADT/bit.h"

RegInterferenceGraph::RegInterferenceGraph(unsigned PhysRegisterCount, unsigned VirtRegisterCount) :
    VirtRegWeights(VirtRegisterCount, 1.0f),
    VirtRegSpillable(VirtRegisterCount, false),
    EdgeCounts(PhysRegisterCount + VirtRegisterCount, 0)
{
    VirtRegCount = VirtRegisterCount;
    PhysRegCount = PhysRegisterCount;

    TotalVertexCount = VirtRegCount + PhysRegCount;
    WordBitMask = 8*sizeof(uint64_t) - 1;
    WordBitCount = llvm::countr_one(WordBitMask);

    AdjacencyWordCount = ((TotalVertexCount >> WordBitCount) +
                          ((TotalVertexCount & WordBitMask) ? 1 : 0));
    AdjacencyBitMask = 8*AdjacencyWordCount*sizeof(uint64_t) - 1;
    AdjacencyBitCount = llvm::countr_one(AdjacencyBitMask);

    AdjacencyMatrix = std::vector<uint64_t>(TotalVertexCount*AdjacencyWordCount, 0);
    HintAdjacencyMatrix = std::vector<uint64_t>(TotalVertexCount*AdjacencyWordCount, 0);
}

void RegInterferenceGraph::setWeight(unsigned VirtRegIndex, float Weight)
{
    VirtRegWeights[VirtRegIndex] = Weight;
}

void RegInterferenceGraph::setSpillable(unsigned VirtRegIndex, bool Spillable)
{
    VirtRegSpillable[VirtRegIndex] = Spillable;
}

void RegInterferenceGraph::addEdge(unsigned VertexIndexA, unsigned VertexIndexB)
{
    unsigned WordIndexA = VertexIndexA >> WordBitCount;
    unsigned BitIndexA = VertexIndexA & AdjacencyBitMask;

    unsigned WordIndexB = VertexIndexB >> WordBitCount;
    unsigned BitIndexB = VertexIndexB & AdjacencyBitMask;

    AdjacencyMatrix[VertexIndexA*AdjacencyWordCount + WordIndexB] |= (1LL << BitIndexB);
    AdjacencyMatrix[VertexIndexB*AdjacencyWordCount + WordIndexA] |= (1LL << BitIndexA);

    ++EdgeCounts[VertexIndexA];
    ++EdgeCounts[VertexIndexB];
}

void RegInterferenceGraph::addHint(unsigned VertexIndexA, unsigned VertexIndexB)
{
    unsigned WordIndexA = VertexIndexA >> WordBitCount;
    unsigned BitIndexA = VertexIndexA & AdjacencyBitMask;

    unsigned WordIndexB = VertexIndexB >> WordBitCount;
    unsigned BitIndexB = VertexIndexB & AdjacencyBitMask;

    HintAdjacencyMatrix[VertexIndexA*AdjacencyWordCount + WordIndexB] |= (1LL << BitIndexB);
    HintAdjacencyMatrix[VertexIndexB*AdjacencyWordCount + WordIndexA] |= (1LL << BitIndexA);
}

bool RegInterferenceGraph::isHint(unsigned VertexIndexA, unsigned VertexIndexB) const
{
    unsigned WordIndexB = VertexIndexB >> WordBitCount;
    unsigned BitIndexB = VertexIndexB & AdjacencyBitMask;

    return ((HintAdjacencyMatrix[VertexIndexA*AdjacencyWordCount + WordIndexB] & (1LL << BitIndexB)) != 0);
}

bool RegInterferenceGraph::hasEdge(unsigned VertexIndexA, unsigned VertexIndexB) const
{
    unsigned WordIndexB = VertexIndexB >> WordBitCount;
    unsigned BitIndexB = VertexIndexB & AdjacencyBitMask;

    return ((AdjacencyMatrix[VertexIndexA*AdjacencyWordCount + WordIndexB] & (1LL << BitIndexB)) != 0);
}

bool RegInterferenceGraph::isSpillable(unsigned VirtIndex) const
{
    return (VirtRegSpillable[VirtIndex] != 0);
}

float RegInterferenceGraph::getWeight(unsigned VirtIndex) const
{
    return VirtRegWeights[VirtIndex];
}

unsigned RegInterferenceGraph::getEdgeCount(unsigned VertIndex) const
{
    return EdgeCounts[VertIndex];
}

unsigned RegInterferenceGraph::physIndexToVertIndex(unsigned PhysIndex) const
{
    return PhysIndex;
}

unsigned RegInterferenceGraph::virtIndexToVertIndex(unsigned VirtIndex) const
{
    return VirtIndex + PhysRegCount;
}

unsigned RegInterferenceGraph::vertIndexToPhysIndex(unsigned VertexIndex) const
{
    return VertexIndex;
}

unsigned RegInterferenceGraph::vertIndexToVirtIndex(unsigned VertexIndex) const
{
    return VertexIndex - PhysRegCount;
}

bool RegInterferenceGraph::isPhys(unsigned VertexIndex) const
{
    return (VertexIndex < PhysRegCount);
}

unsigned RegInterferenceGraph::getPhysRegCount() const
{
    return PhysRegCount;
}

unsigned RegInterferenceGraph::getVirtRegCount() const
{
    return VirtRegCount;
}

unsigned RegInterferenceGraph::getVertexCount() const
{
    return (PhysRegCount + VirtRegCount);
}

const uint64_t *RegInterferenceGraph::getAdjacencyMatrix() const
{
    return AdjacencyMatrix.data();
}

const uint64_t *RegInterferenceGraph::getHintAdjacencyMatrix() const
{
    return HintAdjacencyMatrix.data();
}

/*
int RegInterferenceGraph::vertexWeightAsInteger(unsigned VertIndex) const
{
    uint64_t WeightRaw;
    if(isPhys(VertIndex))
    {
        WeightRaw = ~(0LLu);
    }
    else
    {
        int VirtIndex = vertIndexToVirtIndex(VertIndex);
        WeightRaw = isSpillable(VirtIndex) ? (1000000LL * getWeight(VirtIndex)) : 10000000000LL;
    }

    int WeightTruncated = (sizeof(WeightRaw)*8)- llvm::countl_zero(WeightRaw);

    return WeightTruncated;
}
*/

int RegInterferenceGraph::vertexWeightAsInteger(unsigned VertIndex) const
{
    int Weight;
    if(isPhys(VertIndex))
    {
        Weight = 2000000000;
    }
    else
    {
        int VirtIndex = vertIndexToVirtIndex(VertIndex);
        Weight = isSpillable(VirtIndex) ? (1000000 * getWeight(VirtIndex)) : 10000000;
    }

    return Weight;
}

#include <fstream>

void RegInterferenceGraph::saveAs(std::string Name) const
{
    unsigned VertexCount = getVertexCount();

    std::ofstream FileOut;
    FileOut.open(Name + ".edgelist");

    for(unsigned VertexA = 0;
        VertexA < VertexCount;
        ++VertexA)
    {
        for(unsigned VertexB = VertexA + 1;
            VertexB < VertexCount;
            ++VertexB)
        {
            if(hasEdge(VertexA, VertexB))
            {
                FileOut << VertexA << " " << VertexB << std::endl;
            }
        }
    }

    FileOut.close();

    FileOut.open(Name + ".col.w");

    for(unsigned Vertex = 0;
        Vertex < VertexCount;
        ++Vertex)
    {
        int Weight = vertexWeightAsInteger(Vertex);

        FileOut << Weight << std::endl;
    }

    FileOut.close();
}
