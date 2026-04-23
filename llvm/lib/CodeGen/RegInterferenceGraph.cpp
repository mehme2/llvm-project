#include "RegInterferenceGraph.h"
#include "llvm/ADT/bit.h"
#include <cassert>

RegInterferenceGraph::RegInterferenceGraph(unsigned PhysRegisterCount, unsigned VirtRegisterCount) :
    VirtRegWeights(VirtRegisterCount, 1.0f),
    VirtRegSpillable(VirtRegisterCount, false)
{
    VirtRegCount = VirtRegisterCount;
    PhysRegCount = PhysRegisterCount;

    TotalVertexCount = VirtRegCount + PhysRegCount;
    WordBitMask = 8*sizeof(uint64_t) - 1;
    WordBitCount = llvm::countr_one(WordBitMask);

    AdjacencyWordCount = ((TotalVertexCount >> WordBitCount) +
                          ((TotalVertexCount & WordBitMask) ? 1 : 0));

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
    unsigned BitIndexA = VertexIndexA & WordBitMask;

    unsigned WordIndexB = VertexIndexB >> WordBitCount;
    unsigned BitIndexB = VertexIndexB & WordBitMask;

    AdjacencyMatrix[VertexIndexA*AdjacencyWordCount + WordIndexB] |= (1LL << BitIndexB);
    AdjacencyMatrix[VertexIndexB*AdjacencyWordCount + WordIndexA] |= (1LL << BitIndexA);
}

void RegInterferenceGraph::addHint(unsigned VertexIndexA, unsigned VertexIndexB)
{
    unsigned WordIndexA = VertexIndexA >> WordBitCount;
    unsigned BitIndexA = VertexIndexA & WordBitMask;

    unsigned WordIndexB = VertexIndexB >> WordBitCount;
    unsigned BitIndexB = VertexIndexB & WordBitMask;

    HintAdjacencyMatrix[VertexIndexA*AdjacencyWordCount + WordIndexB] |= (1LL << BitIndexB);
    HintAdjacencyMatrix[VertexIndexB*AdjacencyWordCount + WordIndexA] |= (1LL << BitIndexA);
}

void RegInterferenceGraph::addHintsAsEdges()
{
    unsigned VertCount = getVertexCount();

    for(unsigned VertIndexA = 0;
        VertIndexA < VertCount;
        ++VertIndexA)
    {
        for(unsigned VertIndexB = 0;
            VertIndexB < VertCount;
            ++VertIndexB)
        {
            if((VertIndexA != VertIndexB) && !isHint(VertIndexA, VertIndexB))
            {
                addEdge(VertIndexA, VertIndexB);
            }
        }
    }
}

void RegInterferenceGraph::blockVertex(unsigned VertexIndex)
{
    unsigned VertCount = getVertexCount();

    for(unsigned VertIndexA = 0;
        VertIndexA < VertCount;
        ++VertIndexA)
    {
        if(VertexIndex != VertIndexA)
        {
            addEdge(VertexIndex, VertIndexA);
        }
    }
}

bool RegInterferenceGraph::isHint(unsigned VertexIndexA, unsigned VertexIndexB) const
{
    unsigned WordIndexB = VertexIndexB >> WordBitCount;
    unsigned BitIndexB = VertexIndexB & WordBitMask;

    return ((HintAdjacencyMatrix[VertexIndexA*AdjacencyWordCount + WordIndexB] & (1LL << BitIndexB)) != 0);
}

bool RegInterferenceGraph::hasEdge(unsigned VertexIndexA, unsigned VertexIndexB) const
{
    unsigned WordIndexA = VertexIndexA >> WordBitCount;
    unsigned BitIndexA = VertexIndexA & WordBitMask;

    unsigned WordIndexB = VertexIndexB >> WordBitCount;
    unsigned BitIndexB = VertexIndexB & WordBitMask;

    assert(((AdjacencyMatrix[VertexIndexA*AdjacencyWordCount + WordIndexB] & (1LL << BitIndexB)) != 0) ==
           ((AdjacencyMatrix[VertexIndexB*AdjacencyWordCount + WordIndexA] & (1LL << BitIndexA)) != 0));

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

unsigned RegInterferenceGraph::physIndexToVertIndex(unsigned PhysIndex) const
{
    assert(PhysIndex < PhysRegCount);
    return PhysIndex;
}

unsigned RegInterferenceGraph::virtIndexToVertIndex(unsigned VirtIndex) const
{
    assert(VirtIndex < VirtRegCount);
    return VirtIndex + PhysRegCount;
}

unsigned RegInterferenceGraph::vertIndexToPhysIndex(unsigned VertexIndex) const
{
    assert(VertexIndex < PhysRegCount);
    return VertexIndex;
}

unsigned RegInterferenceGraph::vertIndexToVirtIndex(unsigned VertexIndex) const
{
    assert(VertexIndex >= PhysRegCount);
    assert(VertexIndex < TotalVertexCount);
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
