#ifndef LLVM_CODEGEN_REGALLOCGRAPHSOLVERS_H
#define LLVM_CODEGEN_REGALLOCGRAPHSOLVERS_H

#include "RegInterferenceGraph.h"

#define REGALLOC_GRAPH_SOLVER_TABLE \
REGALLOC_GRAPH_SOLVER_ENTRY(RegAllocBitEASolver, bitea) \
REGALLOC_GRAPH_SOLVER_ENTRY(RegAllocBitEASolver, bitea2) \


#define REGALLOC_GRAPH_SOLVER(Name) std::vector<int> Name(const RegInterferenceGraph &Graph)
typedef REGALLOC_GRAPH_SOLVER(regalloc_graph_solver);

#define REGALLOC_GRAPH_SOLVER_ENTRY(Name, ID) REGALLOC_GRAPH_SOLVER(Name);
REGALLOC_GRAPH_SOLVER_TABLE
#undef REGALLOC_GRAPH_SOLVER_ENTRY

#endif
