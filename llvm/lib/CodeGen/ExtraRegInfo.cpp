#include "ExtraRegInfo.h"

using namespace llvm;

void ExtraRegInfo::LRE_DidCloneVirtReg(Register New, Register Old) {
  // Cloning a register we haven't even heard about yet?  Just ignore it.
  if (!Info.inBounds(Old))
    return;

  // LRE may clone a virtual register because dead code elimination causes it to
  // be split into connected components. The new components are much smaller
  // than the original, so they should get a new chance at being assigned.
  // same stage as the parent.
  Info[Old].Stage = RS_Assign;
  Info.grow(New.id());
  Info[New] = Info[Old];
}
