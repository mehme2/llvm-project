#include "RegInterferenceGraph.h"
#include "llvm/ADT/bit.h"

RegInterferenceGraph::RegInterferenceGraph(unsigned PhysRegisterCount, unsigned VirtRegisterCount)
{
    VirtRegCount = VirtRegisterCount;
    PhysRegCount = PhysRegisterCount;

    VirtRegWeights = std::vector<float>(VirtRegCount, 1.0f);
    VirtRegSpillable = std::vector<unsigned>(VirtRegCount, false);

    TotalVertexCount = VirtRegCount + PhysRegCount;
    WordBitMask = 8*sizeof(uint64_t) - 1;
    WordBitCount = llvm::countr_one(WordBitMask);

    AdjacencyWordCount = ((TotalVertexCount >> WordBitCount) +
                          ((TotalVertexCount & WordBitMask) ? 1 : 0));
    AdjacencyBitMask = 8*AdjacencyWordCount*sizeof(uint64_t) - 1;
    AdjacencyBitCount = llvm::countr_one(AdjacencyBitMask);

    AdjacencyMatrix = std::vector<uint64_t>(TotalVertexCount*AdjacencyWordCount, 0);
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
