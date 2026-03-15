#ifndef LLVM_CODEGEN_REGINTERFERENCEGRAPH_H
#define LLVM_CODEGEN_REGINTERFERENCEGRAPH_H

#include <vector>
#include <string>
#include <stdint.h>

class RegInterferenceGraph
{
public:

    RegInterferenceGraph(unsigned PhysRegisterCount, unsigned VirtRegisterCount);

    void addEdge(unsigned VertexIndexA, unsigned VertexIndexB);
    void addHint(unsigned VertexIndexA, unsigned VertexIndexB);

    void setWeight(unsigned VirtRegIndex, float Weight);
    void setSpillable(unsigned VirtRegIndex, bool Spillable);

    bool hasEdge(unsigned VertexIndexA, unsigned VertexIndexB) const;
    bool isHint(unsigned VertexIndexA, unsigned VertexIndexB) const;
    bool isSpillable(unsigned VirtIndex) const;
    float getWeight(unsigned VirtIndex) const;
    unsigned getEdgeCount(unsigned VertIndex) const;

    unsigned physIndexToVertIndex(unsigned PhysIndex) const;
    unsigned virtIndexToVertIndex(unsigned VirtIndex) const;

    unsigned vertIndexToPhysIndex(unsigned VertexIndex) const;
    unsigned vertIndexToVirtIndex(unsigned VertexIndex) const;

    bool isPhys(unsigned VertexIndex) const;

    unsigned getPhysRegCount() const;
    unsigned getVirtRegCount() const;
    unsigned getVertexCount() const;

    const uint64_t *getAdjacencyMatrix() const;
    const uint64_t *getHintAdjacencyMatrix() const;

    int vertexWeightAsInteger(unsigned VertIndex) const;

    void saveAs(std::string Name) const;

private:

    std::vector<float> VirtRegWeights;
    std::vector<unsigned> VirtRegSpillable;
    std::vector<unsigned> EdgeCounts;

    std::vector<uint64_t> AdjacencyMatrix;
    std::vector<uint64_t> HintAdjacencyMatrix;

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
