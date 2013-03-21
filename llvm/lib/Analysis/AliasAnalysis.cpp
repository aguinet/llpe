//===- AliasAnalysis.cpp - Generic Alias Analysis Interface Implementation -==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the generic AliasAnalysis interface which is used as the
// common interface used by all clients and implementations of alias analysis.
//
// This file also implements the default version of the AliasAnalysis interface
// that is to be used when no other implementation is specified.  This does some
// simple tests that detect obvious cases: two different global pointers cannot
// alias, a global cannot alias a malloc, two different mallocs cannot alias,
// etc.
//
// This alias analysis implementation really isn't very good for anything, but
// it is very fast, and makes a nice clean default implementation.  Because it
// handles lots of little corner cases, other, more complex, alias analysis
// implementations may choose to rely on this pass to resolve these simple and
// easy cases.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Pass.h"
#include "llvm/BasicBlock.h"
#include "llvm/Function.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Instructions.h"
#include "llvm/Type.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Analysis/HypotheticalConstantFolder.h"
using namespace llvm;

// Register the AliasAnalysis interface, providing a nice name to refer to.
static RegisterAnalysisGroup<AliasAnalysis> Z("Alias Analysis");
char AliasAnalysis::ID = 0;

//===----------------------------------------------------------------------===//
// Default chaining methods
//===----------------------------------------------------------------------===//

AliasAnalysis::AliasResult
AliasAnalysis::alias(const Value *V1, unsigned V1Size,
                     const Value *V2, unsigned V2Size) {
  assert(AA && "AA didn't call InitializeAliasAnalysis in its run method!");
  return AA->alias(V1, V1Size, V2, V2Size);
}

AliasAnalysis::AliasResult
AliasAnalysis::aliasHypothetical(ShadowValue V1, unsigned V1Size,
				 ShadowValue V2, unsigned V2Size, bool usePBKnowledge) {
  assert(AA && "AA didn't call InitializeAliasAnalysis in its run method!");
  return AA->aliasHypothetical(V1, V1Size, V2, V2Size);
}

bool AliasAnalysis::pointsToConstantMemory(const Value *P) {
  assert(AA && "AA didn't call InitializeAliasAnalysis in its run method!");
  return AA->pointsToConstantMemory(P);
}

void AliasAnalysis::deleteValue(Value *V) {
  assert(AA && "AA didn't call InitializeAliasAnalysis in its run method!");
  AA->deleteValue(V);
}

void AliasAnalysis::copyValue(Value *From, Value *To) {
  assert(AA && "AA didn't call InitializeAliasAnalysis in its run method!");
  AA->copyValue(From, To);
}

AliasAnalysis::ModRefResult
AliasAnalysis::getCSModRefInfoWithOffset(ShadowValue CSV, ShadowValue P, int64_t POffset, unsigned PSize, IntAAProxy& AACB) {

  return getCSModRefInfo(CSV, P, PSize, true, POffset, &AACB);

}

AliasAnalysis::ModRefResult
AliasAnalysis::getCSModRefInfo(ShadowValue CSV, ShadowValue P, unsigned Size, bool usePBKnowledge, int64_t POffset, IntAAProxy* AACB) {

  assert(!!CSV.getCtx() == !!P.getCtx());

  ImmutableCallSite CS(CSV.getBareVal());

  // Don't assert AA because BasicAA calls us in order to make use of the
  // logic here.

  ModRefBehavior MRB = getModRefBehavior(CS);
  if (MRB == DoesNotAccessMemory)
    return NoModRef;
  
  ModRefResult Mask = ModRef;
  if (MRB == OnlyReadsMemory)
    Mask = Ref;
  else if (MRB == AliasAnalysis::AccessesArguments) {
    bool doesAlias = false;
    for(unsigned i = 0; i < CS.arg_size() && !doesAlias; ++i) {
      if (!isNoAlias(P, Size, getValArgOperand(CSV, 0), ~0U, usePBKnowledge, POffset, AACB))
        doesAlias = true;
    }
    
    if (!doesAlias)
      return NoModRef;
  }
  
  // If P points to a constant memory location, the call definitely could not
  // modify the memory location.
  if ((Mask & Mod) && pointsToConstantMemory(P.getBareVal()))
    Mask = ModRefResult(Mask & ~Mod);

  // If this is BasicAA, don't forward.
  if (!AA) return Mask;

  // Otherwise, fall back to the next AA in the chain. But we can merge
  // in any mask we've managed to compute.
  return ModRefResult(AA->getCSModRefInfo(CSV, P, Size, usePBKnowledge, POffset, AACB) & Mask);
}

AliasAnalysis::ModRefResult
AliasAnalysis::get2CSModRefInfo(ShadowValue CS1V, ShadowValue CS2V, bool usePBKnowledge) {
  // Don't assert AA because BasicAA calls us in order to make use of the
  // logic here.

  ImmutableCallSite CS1(CS1V.getBareVal());
  ImmutableCallSite CS2(CS2V.getBareVal());

  assert(!!CS1Ctx == !!CS2Ctx);

  // If CS1 or CS2 are readnone, they don't interact.
  ModRefBehavior CS1B = getModRefBehavior(CS1);
  if (CS1B == DoesNotAccessMemory) return NoModRef;

  ModRefBehavior CS2B = getModRefBehavior(CS2);
  if (CS2B == DoesNotAccessMemory) return NoModRef;

  // If they both only read from memory, there is no dependence.
  if (CS1B == OnlyReadsMemory && CS2B == OnlyReadsMemory)
    return NoModRef;

  AliasAnalysis::ModRefResult Mask = ModRef;

  // If CS1 only reads memory, the only dependence on CS2 can be
  // from CS1 reading memory written by CS2.
  if (CS1B == OnlyReadsMemory)
    Mask = ModRefResult(Mask & Ref);

  // If CS2 only access memory through arguments, accumulate the mod/ref
  // information from CS1's references to the memory referenced by
  // CS2's arguments.
  if (CS2B == AccessesArguments) {
    AliasAnalysis::ModRefResult R = NoModRef;
    for(unsigned i = 0; i < CS2.arg_size() && R != Mask; ++i) {
      R = ModRefResult((R | getSVModRefInfo(CS1V, getValArgOperand(CS2V, i), UnknownSize)) & Mask);
    }
    return R;
  }

  // If CS1 only accesses memory through arguments, check if CS2 references
  // any of the memory referenced by CS1's arguments. If not, return NoModRef.
  if (CS1B == AccessesArguments) {
    AliasAnalysis::ModRefResult R = NoModRef;
    for(unsigned i = 0; i < CS1.arg_size() && R != Mask; ++i) {
      if (getSVModRefInfo(CS2V, getValArgOperand(CS1V, i), UnknownSize, usePBKnowledge) != NoModRef) {
        R = Mask;
        break;
      }
    }
    if (R == NoModRef)
      return R;
  }

  // If this is BasicAA, don't forward.
  if (!AA) return Mask;

  // Otherwise, fall back to the next AA in the chain. But we can merge
  // in any mask we've managed to compute.
  return ModRefResult(AA->get2CSModRefInfo(CS1V, CS2V, usePBKnowledge) & Mask);
}

AliasAnalysis::ModRefBehavior
AliasAnalysis::getModRefBehavior(ImmutableCallSite CS) {
  // Don't assert AA because BasicAA calls us in order to make use of the
  // logic here.

  ModRefBehavior Min = UnknownModRefBehavior;

  // Call back into the alias analysis with the other form of getModRefBehavior
  // to see if it can give a better response.
  if (const Function *F = CS.getCalledFunction())
    Min = getModRefBehavior(F);

  // If this is BasicAA, don't forward.
  if (!AA) return Min;

  // Otherwise, fall back to the next AA in the chain. But we can merge
  // in any result we've managed to compute.
  return std::min(AA->getModRefBehavior(CS), Min);
}

AliasAnalysis::ModRefBehavior
AliasAnalysis::getModRefBehavior(const Function *F) {
  assert(AA && "AA didn't call InitializeAliasAnalysis in its run method!");
  return AA->getModRefBehavior(F);
}


//===----------------------------------------------------------------------===//
// AliasAnalysis non-virtual helper method implementation
//===----------------------------------------------------------------------===//

AliasAnalysis::ModRefResult
AliasAnalysis::getLoadModRefInfo(ShadowValue L, ShadowValue P, unsigned Size, bool usePBKnowledge) {

  assert(!!L.getCtx() == !!P.getCtx());

  // Be conservative in the face of volatile.
  if (cast_val<LoadInst>(L)->isVolatile())
    return ModRef;

  // If the load address doesn't alias the given address, it doesn't read
  // or write the specified memory.
  if (!aliasHypothetical(getValOperand(L, 0), getTypeStoreSize(L.getType()), P, Size, usePBKnowledge))
    return NoModRef;

  // Otherwise, a load just reads.
  return Ref;
}

AliasAnalysis::ModRefResult
AliasAnalysis::getStoreModRefInfo(ShadowValue S, ShadowValue P, unsigned Size, bool usePBKnowledge) {

  assert(!!S.getCtx() == !!P.getCtx());

  // Be conservative in the face of volatile.
  if (cast_val<StoreInst>(S)->isVolatile())
    return ModRef;

  // If the store address cannot alias the pointer in question, then the
  // specified memory cannot be modified by the store.
  if (!aliasHypothetical(getValOperand(S, 1), getTypeStoreSize(getValOperand(S, 0).getType()), P, Size, usePBKnowledge))
    return NoModRef;

  // If the pointer is a pointer to constant memory, then it could not have been
  // modified by this store.
  if (pointsToConstantMemory(P.getBareVal()))
    return NoModRef;
  
  // Otherwise, a store just writes.
  return Mod;
}

AliasAnalysis::ModRefResult
AliasAnalysis::getVAModRefInfo(ShadowValue I, ShadowValue V, unsigned Size, bool usePBKnowledge) {

  assert(!!V.getCtx() == !!P.getCtx());

  // If the va_arg address cannot alias the pointer in question, then the
  // specified memory cannot be accessed by the va_arg.
  if (!aliasHypothetical(getValOperand(I, 0), UnknownSize, V, Size, usePBKnowledge))
    return NoModRef;

  // If the pointer is a pointer to constant memory, then it could not have been
  // modified by this va_arg.
  if (pointsToConstantMemory(V.getBareVal()))
    return NoModRef;

  // Otherwise, a va_arg reads and writes.
  return ModRef;
}


AliasAnalysis::ModRefBehavior
AliasAnalysis::getIntrinsicModRefBehavior(unsigned iid) {
#define GET_INTRINSIC_MODREF_BEHAVIOR
#include "llvm/Intrinsics.gen"
#undef GET_INTRINSIC_MODREF_BEHAVIOR
}

// AliasAnalysis destructor: DO NOT move this to the header file for
// AliasAnalysis or else clients of the AliasAnalysis class may not depend on
// the AliasAnalysis.o file in the current .a file, causing alias analysis
// support to not be included in the tool correctly!
//
AliasAnalysis::~AliasAnalysis() {}

/// InitializeAliasAnalysis - Subclasses must call this method to initialize the
/// AliasAnalysis interface before any other methods are called.
///
void AliasAnalysis::InitializeAliasAnalysis(Pass *P) {
  TD = P->getAnalysisIfAvailable<TargetData>();
  AA = &P->getAnalysis<AliasAnalysis>();
}

// getAnalysisUsage - All alias analysis implementations should invoke this
// directly (using AliasAnalysis::getAnalysisUsage(AU)).
void AliasAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<AliasAnalysis>();         // All AA's chain
}

/// getTypeStoreSize - Return the TargetData store size for the given type,
/// if known, or a conservative value otherwise.
///
unsigned AliasAnalysis::getTypeStoreSize(const Type *Ty) {
  return TD ? TD->getTypeStoreSize(Ty) : ~0u;
}

/// canBasicBlockModify - Return true if it is possible for execution of the
/// specified basic block to modify the value pointed to by Ptr.
///
bool AliasAnalysis::canBasicBlockModify(const BasicBlock &BB,
                                        const Value *Ptr, unsigned Size) {
  return canInstructionRangeModify(BB.front(), BB.back(), Ptr, Size);
}

/// canInstructionRangeModify - Return true if it is possible for the execution
/// of the specified instructions to modify the value pointed to by Ptr.  The
/// instructions to consider are all of the instructions in the range of [I1,I2]
/// INCLUSIVE.  I1 and I2 must be in the same basic block.
///
bool AliasAnalysis::canInstructionRangeModify(const Instruction &I1,
                                              const Instruction &I2,
                                              const Value *Ptr, unsigned Size) {
  assert(I1.getParent() == I2.getParent() &&
         "Instructions not in same basic block!");
  BasicBlock::const_iterator I = &I1;
  BasicBlock::const_iterator E = &I2;
  ++E;  // Convert from inclusive to exclusive range.

  for (; I != E; ++I) // Check every instruction in range
    if (getModRefInfo(const_cast<Instruction*>(&*I), Ptr, Size) & Mod)
      return true;
  return false;
}

/// isNoAliasCall - Return true if this pointer is returned by a noalias
/// function.
bool llvm::isNoAliasCall(const Value *V) {
  if (isa<CallInst>(V) || isa<InvokeInst>(V))
    return ImmutableCallSite(cast<Instruction>(V))
      .paramHasAttr(0, Attribute::NoAlias);
  return false;
}

/// isIdentifiedObject - Return true if this pointer refers to a distinct and
/// identifiable object.  This returns true for:
///    Global Variables and Functions (but not Global Aliases)
///    Allocas and Mallocs
///    ByVal and NoAlias Arguments
///    NoAlias returns
///
bool llvm::isIdentifiedObject(const Value *V) {
  if (isa<AllocaInst>(V))
    return true;
  if (isa<GlobalValue>(V) && !isa<GlobalAlias>(V))
    return true;
  if (isNoAliasCall(V))
    return true;
  if (const Argument *A = dyn_cast<Argument>(V))
    return A->hasNoAliasAttr() || A->hasByValAttr();
  return false;
}

// Because of the way .a files work, we must force the BasicAA implementation to
// be pulled in if the AliasAnalysis classes are pulled in.  Otherwise we run
// the risk of AliasAnalysis being used, but the default implementation not
// being linked into the tool that uses it.
DEFINING_FILE_FOR(AliasAnalysis)
