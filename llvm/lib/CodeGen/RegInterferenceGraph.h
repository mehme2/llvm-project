#ifndef LLVM_CODEGEN_REGINTERFERENCEGRAPH_H
#define LLVM_CODEGEN_REGINTERFERENCEGRAPH_H

#include <vector>
#include <stdint.h>

class RegInterferenceGraph
{
public:

    RegInterferenceGraph(unsigned PhysRegisterCount, unsigned VirtRegisterCount);

    void addEdge(unsigned VertexIndexA, unsigned VertexIndexB);

    void setWeight(unsigned VirtRegIndex, float Weight);
    void setSpillable(unsigned VirtRegIndex, bool Spillable);

    bool hasEdge(unsigned VertexIndexA, unsigned VertexIndexB) const;
    bool isSpillable(unsigned VirtIndex) const;
    float getWeight(unsigned VirtIndex) const;

    unsigned physIndexToVertIndex(unsigned PhysIndex) const;
    unsigned virtIndexToVertIndex(unsigned VirtIndex) const;

    unsigned vertIndexToPhysIndex(unsigned VertexIndex) const;
    unsigned vertIndexToVirtIndex(unsigned VertexIndex) const;

    bool isPhys(unsigned VertexIndex) const;

    unsigned getPhysRegCount() const;
    unsigned getVirtRegCount() const;
    unsigned getVertexCount() const;

    const uint64_t *getAdjacencyMatrix() const;

private:

    std::vector<float> VirtRegWeights;
    std::vector<unsigned> VirtRegSpillable;

    std::vector<uint64_t> AdjacencyMatrix;

    unsigned PhysRegCount;
    unsigned VirtRegCount;
    unsigned TotalVertexCount;
    unsigned WordBitMask;
    unsigned WordBitCount;
    unsigned AdjacencyWordCount;
    unsigned AdjacencyBitMask;
    unsigned AdjacencyBitCount;
};

#endif
