#include "RegAllocGraphSolvers.h"

#include <unordered_set>
#include <unordered_map>
#include <optional>
#include <algorithm>
#include <ostream>

namespace alihan {

template <typename Iterator> class Range {
public:
  Range(Iterator beginIt, Iterator endIt) : mBeginIt(beginIt), mEndIt(endIt) {}
  auto begin() -> Iterator { return mBeginIt; }
  auto end() -> Iterator { return mEndIt; }

private:
  Iterator mBeginIt;
  Iterator mEndIt;
};

class InterferenceGraph {
private:
  class Node {
  public:
    using EdgeIterator = std::unordered_set<unsigned>::const_iterator;

    Node() = delete;
    Node(double weight, bool spillable);

    [[nodiscard]] auto getWeight() const -> double;
    [[nodiscard]] auto getSpillable() const -> bool;
    [[nodiscard]] auto getEdgeCount() const -> std::size_t;

    [[nodiscard]] auto hasEdge(unsigned node) const -> bool;
    void addEdge(unsigned node);
    void removeEdge(unsigned node);

    [[nodiscard]] auto isLessThan(const Node &node) const -> bool;

    [[nodiscard]] auto begin() const -> EdgeIterator;
    [[nodiscard]] auto end() const -> EdgeIterator;

  private:
    double mWeight;
    bool mSpillable;
    std::unordered_set<unsigned> mEdges;
  };

public:
  using EdgeIterator = Node::EdgeIterator;

  class NodeIterator {
  private:
    using iterator = std::unordered_map<unsigned, Node>::const_iterator;

  public:
    using value_type = unsigned;

    NodeIterator(iterator node);
    auto operator++() -> NodeIterator &;
    [[nodiscard]] auto operator*() const -> unsigned;
    [[nodiscard]] auto operator==(NodeIterator const &it) const -> bool;
    [[nodiscard]] auto operator!=(NodeIterator const &it) const -> bool;

  private:
    iterator mNode;
  };

  [[nodiscard]] auto isEmpty() const -> bool;
  [[nodiscard]] auto getSize() const -> std::size_t;
  [[nodiscard]] auto getWeight(unsigned node) const -> std::optional<double>;
  [[nodiscard]] auto getSpillable(unsigned node) const -> std::optional<bool>;
  [[nodiscard]] auto getEdgeCount(unsigned node) const -> std::optional<std::size_t>;

  [[nodiscard]] auto hasNode(unsigned node) const -> bool;
  [[nodiscard]] auto hasEdge(unsigned node1, unsigned node2) const -> bool;
  void addNode(unsigned id, double weight, bool spillable);
  auto addEdge(unsigned node1, unsigned node2) -> bool;
  void removeNode(unsigned node);
  auto removeEdge(unsigned node1, unsigned node2) -> bool;

  [[nodiscard]] auto isNodeLessThan(unsigned node1, unsigned node2) const -> std::optional<bool>;

  [[nodiscard]] auto getNodeRange() const -> Range<NodeIterator>;
  [[nodiscard]] auto getEdgeRange(unsigned node) const -> std::optional<Range<EdgeIterator>>;

  auto print(std::ostream &os) const -> std::ostream &;

private:
  [[nodiscard]] auto getNode(unsigned node) const -> const Node *;
  [[nodiscard]] auto getNode(unsigned node) -> Node *;

  std::unordered_map<unsigned, Node> mGraph;
};

InterferenceGraph::Node::Node(double Weight, bool spillable)
    : mWeight{Weight}, mSpillable{spillable} {}

auto InterferenceGraph::Node::getWeight() const -> double { return mWeight; }

auto InterferenceGraph::Node::getSpillable() const -> bool {
  return mSpillable;
}

auto InterferenceGraph::Node::getEdgeCount() const -> std::size_t {
  return mEdges.size();
}

auto InterferenceGraph::Node::hasEdge(unsigned node) const -> bool {
  return mEdges.count(node);
}

void InterferenceGraph::Node::addEdge(unsigned node) { mEdges.insert(node); }

void InterferenceGraph::Node::removeEdge(unsigned node) { mEdges.erase(node); }

auto InterferenceGraph::Node::isLessThan(const Node &other) const -> bool {
  if (mSpillable && other.mSpillable) {
    return mWeight < other.mWeight;
  } 
  return mSpillable;
}

auto InterferenceGraph::Node::begin() const -> EdgeIterator {
  return mEdges.begin();
}

auto InterferenceGraph::Node::end() const -> EdgeIterator {
  return mEdges.end();
}

InterferenceGraph::NodeIterator::NodeIterator(iterator node) : mNode{node} {}

auto InterferenceGraph::NodeIterator::operator++() -> NodeIterator & {
  ++mNode;
  return *this;
}

auto InterferenceGraph::NodeIterator::operator*() const -> unsigned {
  return mNode->first;
}

auto InterferenceGraph::NodeIterator::operator==(const NodeIterator &it) const
    -> bool {
  return mNode == it.mNode;
}

auto InterferenceGraph::NodeIterator::operator!=(const NodeIterator &it) const
    -> bool {
  return !(*this == it);
}

auto InterferenceGraph::isEmpty() const -> bool { return mGraph.empty(); }

auto InterferenceGraph::getSize() const -> std::size_t { return mGraph.size(); }

auto InterferenceGraph::getWeight(unsigned node) const
    -> std::optional<double> {
  if (const Node *n = getNode(node)) {
    return n->getWeight();
  }
  return {};
}

auto InterferenceGraph::getSpillable(unsigned node) const
    -> std::optional<bool> {
  if (const Node *n = getNode(node)) {
    return n->getSpillable();
  }
  return {};
}

auto InterferenceGraph::getEdgeCount(unsigned node) const
    -> std::optional<std::size_t> {
  if (const Node *n = getNode(node)) {
    return n->getEdgeCount();
  }
  return {};
}

auto InterferenceGraph::hasNode(unsigned node) const -> bool {
  return mGraph.count(node);
}

auto InterferenceGraph::hasEdge(unsigned node1, unsigned node2) const -> bool {
  const Node *n1 = getNode(node1);
  return n1 && n1->hasEdge(node2);
}

void InterferenceGraph::addNode(unsigned id, double weight, bool spillable) {
  mGraph.insert({id, Node(weight, spillable)});
}

auto InterferenceGraph::addEdge(unsigned node1, unsigned node2) -> bool {
  if (Node *n1 = getNode(node1)) {
    if (Node *n2 = getNode(node2)) {
      n1->addEdge(node2);
      n2->addEdge(node1);
      return true;
    }
  }
  return false;
}

void InterferenceGraph::removeNode(unsigned node) {
  if (Node *n = getNode(node)) {
    for (unsigned edge : *n) {
      Node *e = getNode(edge);
      e->removeEdge(node);
    }
    mGraph.erase(node);
  }
}

auto InterferenceGraph::removeEdge(unsigned node1, unsigned node2) -> bool {
  if (Node *n1 = getNode(node1)) {
    if (Node *n2 = getNode(node2)) {
      n1->removeEdge(node2);
      n2->removeEdge(node1);
      return true;
    }
  }
  return false;
}

auto InterferenceGraph::isNodeLessThan(unsigned node1, unsigned node2) const
    -> std::optional<bool> {
  if (const Node *n1 = getNode(node1)) {
    if (const Node *n2 = getNode(node2)) {
      return n1->isLessThan(*n2);
    }
  }
  return {};
}

auto InterferenceGraph::getNodeRange() const -> Range<NodeIterator> {
  return Range<NodeIterator>(mGraph.begin(), mGraph.end());
}

auto InterferenceGraph::getEdgeRange(unsigned node) const
    -> std::optional<Range<EdgeIterator>> {
  if (const Node *n = getNode(node)) {
    return Range<EdgeIterator>(n->begin(), n->end());
  }
  return {};
}

namespace {
  template<typename Iterator>
  std::vector<unsigned> sortAndCollectGreaterThanOrEqual(Iterator first, Iterator last, unsigned gte) {
    std::vector<unsigned> vec;
    for (; first != last; ++first) {
      if (*first >= gte) {
        vec.push_back(*first);
      }
    }
    std::sort(vec.begin(), vec.end());
    return vec;
  }
}

auto InterferenceGraph::print(std::ostream &os) const -> std::ostream & {
  auto nodeRange = getNodeRange();
  std::vector<unsigned> nodes = sortAndCollectGreaterThanOrEqual(nodeRange.begin(), nodeRange.end(), 0);
  bool firstEdge{true};
  for (unsigned node : nodes) {
    auto edgeRange = getEdgeRange(node).value();
    std::vector<unsigned> edgeNodes = sortAndCollectGreaterThanOrEqual(edgeRange.begin(), edgeRange.end(), node + 1);
    for (unsigned edgeNode : edgeNodes) {
      if (!firstEdge) {
        os << '\n';
      }
      os << node << ' ' << edgeNode;
      firstEdge = false;
    }
  }
  return os;
}

auto InterferenceGraph::getNode(unsigned node) const -> const Node * {
  auto it = mGraph.find(node);
  return (it == mGraph.end()) ? nullptr : &it->second;
}

auto InterferenceGraph::getNode(unsigned node) -> Node * {
  auto it = mGraph.find(node);
  return (it == mGraph.end()) ? nullptr : &it->second;
}

using SolutionMap = std::unordered_map<unsigned, unsigned>;

namespace {
auto findUnusedColor(const alihan::InterferenceGraph &graph,
                     unsigned numberOfColors,
                     const alihan::SolutionMap &solution,
                     unsigned node) -> std::optional<unsigned> {
  std::vector<char> colorUsage(numberOfColors);
  if (auto edgeRangeOpt = graph.getEdgeRange(node)) {
    for (unsigned edge : *edgeRangeOpt) {
      auto it = solution.find(edge);
      if (it != solution.end()) {
        colorUsage[it->second] = true;
      }
    }
  } else {
    return {};
  }

  for (unsigned color{0}; color != numberOfColors; ++color) {
    if (!colorUsage[color]) {
      return color;
    }
  }
  return {};
}
} // namespace

auto solveChaitin(const InterferenceGraph &graph,
                  unsigned numberOfColors) -> SolutionMap {
  InterferenceGraph tempGraph = graph;

  auto isLess = [&](unsigned node1, unsigned node2) {
    double w1{tempGraph.getWeight(node1).value()};
    double w2{tempGraph.getWeight(node2).value()};
    std::size_t e1{tempGraph.getEdgeCount(node1).value()};
    std::size_t e2{tempGraph.getEdgeCount(node2).value()};
    bool s1{tempGraph.getSpillable(node1).value()};
    bool s2{tempGraph.getSpillable(node2).value()};
    if (s1 && s2) {
      return w1 / e1 < w2 / e2;
    } else if (!s1 && !s2) {
      return e1 > e2;
    } else {
      return s1;
    }
  };

  std::vector<unsigned> stack;
  while (!tempGraph.isEmpty()) {
    std::optional<unsigned> max;
    for (unsigned node : tempGraph.getNodeRange()) {
      if (tempGraph.getEdgeCount(node).value() >= numberOfColors) {
        continue;
      }

      if (!max || (max && isLess(*max, node))) {
        max = node;
      }
    }

    if (max) {
      stack.push_back(*max);
      tempGraph.removeNode(*max);
    } else {
      auto nodeRange = tempGraph.getNodeRange();
      unsigned min =
          *std::min_element(nodeRange.begin(), nodeRange.end(), isLess);
      tempGraph.removeNode(min);
    }
  }

  SolutionMap solution;

  while (!stack.empty()) {
    unsigned node = stack.back();
    stack.pop_back();
    tempGraph.addNode(node, 0.0, true);
    auto range = graph.getEdgeRange(node);
    for (unsigned edge : *range) {
      if (tempGraph.hasNode(edge)) {
        tempGraph.addEdge(node, edge);
      }
    }
    unsigned color =
        findUnusedColor(tempGraph, numberOfColors, solution, node).value();
    solution.insert({node, color});
  }
  return solution;
}

} // namespace alihan

#include <iostream>

REGALLOC_GRAPH_SOLVER(RegAllocChaitinSolver)
{
    alihan::InterferenceGraph ChaitinGraph;

    unsigned PhysCount = Graph.getPhysRegCount();
    unsigned VirtCount = Graph.getVirtRegCount();
    unsigned VertCount = Graph.getVertexCount();

    for(unsigned VertIndex = 0;
        VertIndex < VertCount;
        ++VertIndex)
    {
        double Weight = 10000000.0f;
        bool Spillable = false;

        if(!Graph.isPhys(VertIndex))
        {
            unsigned VirtIndex = Graph.vertIndexToVirtIndex(VertIndex);
            if(Graph.isSpillable(VirtIndex))
            {
                Spillable = true;
                Weight = Graph.getWeight(VirtIndex);
            }
            else Weight = 10000.0f;
        }

        ChaitinGraph.addNode(VertIndex, Weight, Spillable);

        for(unsigned CheckIndex = 0;
            CheckIndex < VertIndex;
            ++CheckIndex)
        {
            if(Graph.hasEdge(VertIndex, CheckIndex))
            {
                ChaitinGraph.addEdge(VertIndex, CheckIndex);
            }
        }
    }

    auto ChaitinSolution = alihan::solveChaitin(ChaitinGraph, PhysCount);

    std::vector<int> Solution(VirtCount, -1);

    auto End = ChaitinSolution.end();

    for(unsigned ColorIndex = 0;
        ColorIndex < PhysCount;
        ++ColorIndex)
    {
        int ColorPhys = -1;

        for(unsigned PhysIndex = 0;
            PhysIndex < PhysCount;
            ++PhysIndex)
        {
            unsigned VertIndex = Graph.physIndexToVertIndex(PhysIndex);
            auto Iterator = ChaitinSolution.find(VertIndex);
            if((Iterator != End) &&
               (Iterator->second == ColorIndex))
            {
                ColorPhys = PhysIndex;
            }
        }

        for(unsigned VirtIndex = 0;
            VirtIndex < VirtCount;
            ++VirtIndex)
        {
            unsigned VertIndex = Graph.virtIndexToVertIndex(VirtIndex);
            auto Iterator = ChaitinSolution.find(VertIndex);
            if((Iterator != End) &&
               (Iterator->second == ColorIndex))
            {
                Solution[VirtIndex] = ColorPhys;
            }
        }
    }

    return Solution;
}
