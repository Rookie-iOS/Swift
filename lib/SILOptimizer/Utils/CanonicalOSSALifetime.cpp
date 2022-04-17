//===--- CanonicalOSSALifetime.cpp - Canonicalize OSSA value lifetimes ----===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// This top-level API rewrites the extended lifetime of a SILValue:
///
///     bool CanonicalizeOSSALifetime::canonicalizeValueLifetime(SILValue def)
///
/// Each time it's called on a single OSSA value, `def`, it performs three
/// steps:
///
/// 1. Compute "pruned" liveness of def and its copies, ignoring original
///    destroys. Initializes `liveness`.
///
/// 2. Find `def`s final destroy points based on its pruned
///    liveness. Initializes `consumes` and inserts new destroy_value
///    instructions.
///
/// 3. Rewrite `def`s original copies and destroys, inserting new copies where
///    needed. Deletes original copies and destroys and inserts new copies.
///
/// See CanonicalOSSALifetime.h for examples.
///
/// TODO: Canonicalization currently bails out if any uses of the def has
/// OperandOwnership::PointerEscape. Once project_box is protected by a borrow
/// scope and mark_dependence is associated with an end_dependence, those will
/// no longer be represented as PointerEscapes, and canonicalization will
/// naturally work everywhere as intended. The intention is to keep the
/// canonicalization algorithm as simple and robust, leaving the remaining
/// performance opportunities contingent on fixing the SIL representation.
///
/// TODO: Replace BasicBlock SmallDenseMaps/SetVectors with inlined bits;
/// see BasicBlockDataStructures.h.
///
/// TODO: This algorithm would be extraordinarily simple and cheap except for
/// the following issues:
///
/// 1. Liveness is extended by any overlapping begin/end_access scopes. This
/// avoids calling a destructor within an exclusive access. A simpler
/// alternative would be to model all end_access instructions as deinit
/// barriers, but that may significantly limit optimization.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "copy-propagation"

#include "swift/SILOptimizer/Utils/CanonicalOSSALifetime.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/OwnershipUtils.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/DebugOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"
#include "swift/SILOptimizer/Utils/ValueLifetime.h"
#include "llvm/ADT/Statistic.h"

using namespace swift;
using llvm::SmallSetVector;

llvm::Statistic swift::NumCopiesEliminated = {
    DEBUG_TYPE, "NumCopiesEliminated",
    "number of copy_value instructions removed"};

llvm::Statistic swift::NumCopiesGenerated = {
    DEBUG_TYPE, "NumCopiesGenerated",
    "number of copy_value instructions created"};

STATISTIC(NumDestroysEliminated,
          "number of destroy_value instructions removed");
STATISTIC(NumDestroysGenerated, "number of destroy_value instructions created");

//===----------------------------------------------------------------------===//
//                           MARK: General utilities
//===----------------------------------------------------------------------===//

template <typename... T, typename... U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
                     U &&...args) {
  Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

/// The lifetime extends beyond given consuming use. Copy the value.
///
/// This can set the operand value, but cannot invalidate the use iterator.
void swift::copyLiveUse(Operand *use, InstModCallbacks &instModCallbacks) {
  SILInstruction *user = use->getUser();
  SILBuilderWithScope builder(user->getIterator());

  auto loc = RegularLocation::getAutoGeneratedLocation(user->getLoc());
  auto *copy = builder.createCopyValue(loc, use->get());
  instModCallbacks.createdNewInst(copy);
  use->set(copy);

  ++NumCopiesGenerated;
  LLVM_DEBUG(llvm::dbgs() << "  Copying at last use " << *copy);
}

//===----------------------------------------------------------------------===//
//                    MARK: Step 1. Compute pruned liveness
//===----------------------------------------------------------------------===//

bool CanonicalizeOSSALifetime::computeCanonicalLiveness() {
  defUseWorklist.initialize(currentDef);
  while (SILValue value = defUseWorklist.pop()) {
    SILPhiArgument *arg;
    if ((arg = dyn_cast<SILPhiArgument>(value)) && arg->isPhi()) {
      visitAdjacentReborrowsOfPhi(arg, [&](SILPhiArgument *reborrow) {
        defUseWorklist.insert(reborrow);
        return true;
      });
    }
    for (Operand *use : value->getUses()) {
      auto *user = use->getUser();

      // Recurse through copies.
      if (auto *copy = dyn_cast<CopyValueInst>(user)) {
        defUseWorklist.insert(copy);
        continue;
      }
      // Handle debug_value instructions separately.
      if (pruneDebugMode) {
        if (auto *dvi = dyn_cast<DebugValueInst>(user)) {
          // Only instructions potentially outside current pruned liveness are
          // interesting.
          if (liveness.getBlockLiveness(dvi->getParent())
              != PrunedLiveBlocks::LiveOut) {
            recordDebugValue(dvi);
          }
          continue;
        }
      }
      switch (use->getOperandOwnership()) {
      case OperandOwnership::NonUse:
        break;
      case OperandOwnership::TrivialUse:
        llvm_unreachable("this operand cannot handle ownership");

      // Conservatively treat a conversion to an unowned value as a pointer
      // escape. Is it legal to canonicalize ForwardingUnowned?
      case OperandOwnership::ForwardingUnowned:
      case OperandOwnership::PointerEscape:
        return false;
      case OperandOwnership::InstantaneousUse:
      case OperandOwnership::UnownedInstantaneousUse:
      case OperandOwnership::BitwiseEscape:
        liveness.updateForUse(user, /*lifetimeEnding*/ false);
        break;
      case OperandOwnership::ForwardingConsume:
        recordConsumingUse(use);
        liveness.updateForUse(user, /*lifetimeEnding*/ true);
        break;
      case OperandOwnership::DestroyingConsume:
        if (isa<DestroyValueInst>(user)) {
          destroys.insert(user);
        } else {
          // destroy_value does not force pruned liveness (but store etc. does).
          liveness.updateForUse(user, /*lifetimeEnding*/ true);
        }
        recordConsumingUse(use);
        break;
      case OperandOwnership::Borrow:
        if (!liveness.updateForBorrowingOperand(use))
          return false;
        break;
      case OperandOwnership::InteriorPointer:
      case OperandOwnership::ForwardingBorrow:
      case OperandOwnership::EndBorrow:
        // Guaranteed values are considered uses of the value when the value is
        // an owned phi and the guaranteed values are adjacent reborrow phis or
        // reborrow of such.
        liveness.updateForUse(user, /*lifetimeEnding*/ false);
        break;
      case OperandOwnership::Reborrow:
        BranchInst *branch;
        if (!(branch = dyn_cast<BranchInst>(user))) {
          // Non-phi reborrows (tuples, etc) never end the lifetime of the owned
          // value.
          liveness.updateForUse(user, /*lifetimeEnding*/ false);
          defUseWorklist.insert(cast<SingleValueInstruction>(user));
          break;
        }
        if (is_contained(user->getOperandValues(), currentDef)) {
          // An adjacent phi consumes the value being reborrowed.  Although this
          // use doesn't end the lifetime, this user does.
          liveness.updateForUse(user, /*lifetimeEnding*/ true);
          break;
        }
        // No adjacent phi consumes the value.  This use is not lifetime ending.
        liveness.updateForUse(user, /*lifetimeEnding*/ false);
        // This branch reborrows a guaranteed phi whose lifetime is dependent on
        // currentDef.  Uses of the reborrowing phi extend liveness.
        auto *reborrow = branch->getArgForOperand(use);
        defUseWorklist.insert(reborrow);
        break;
      }
    }
  }
  return true;
}

// Return true if \p inst is an end_access whose access scope overlaps the end
// of the pruned live range. This means that a hoisted destroy might execute
// within the access scope which previously executed outside the access scope.
//
// Not overlapping (ignored):
//
//     %def
//     use %def     // pruned liveness ends here
//     begin_access // access scope unrelated to def
//     end_access
//
// Overlapping (must extend pruned liveness):
//
//     %def
//     begin_access // access scope unrelated to def
//     use %def     // pruned liveness ends here
//     end_access
//
// Overlapping (must extend pruned liveness):
//
//     begin_access // access scope unrelated to def
//     %def
//     use %def     // pruned liveness ends here
//     end_access
//
bool CanonicalizeOSSALifetime::
endsAccessOverlappingPrunedBoundary(SILInstruction *inst) {
  if (isa<EndUnpairedAccessInst>(inst)) {
    return true;
  }
  auto *endAccess = dyn_cast<EndAccessInst>(inst);
  if (!endAccess) {
    return false;
  }
  auto *beginAccess = endAccess->getBeginAccess();
  SILBasicBlock *beginBB = beginAccess->getParent();
  switch (liveness.getBlockLiveness(beginBB)) {
  case PrunedLiveBlocks::LiveOut:
    // Found partial overlap of the form:
    //     currentDef
    //     beginAccess
    //     br...
    //   bb...
    //     use
    //     endAccess
    return true;
  case PrunedLiveBlocks::LiveWithin:
    // Check for partial overlap of this form where beginAccess and the last use
    // are in the same block:
    //     currentDef
    //     beginAccess
    //     use
    //     endAccess
    if (std::find_if(std::next(beginAccess->getIterator()), beginBB->end(),
                     [this](SILInstruction &nextInst) {
                       return liveness.isInterestingUser(&nextInst)
                         != PrunedLiveness::NonUser;
                     }) != beginBB->end()) {
      // An interesting use after the beginAccess means overlap.
      return true;
    }
    return false;
  case PrunedLiveBlocks::Dead:
    // Check for partial overlap of this form where beginAccess and currentDef
    // are in different blocks:
    //     beginAccess
    //     br...
    //  bb...
    //     currentDef
    //     endAccess
    //
    // Since beginAccess is not within the canonical live range, its access
    // scope overlaps only if there is a path from beginAccess to currentDef
    // that does not pass through endAccess. endAccess is dominated by
    // both currentDef and begin_access. Therefore, such a path only exists if
    // beginAccess dominates currentDef.
    return domTree->properlyDominates(beginAccess->getParent(),
                                      getCurrentDef()->getParentBlock());
  }
  llvm_unreachable("covered switch");
}

// Find all overlapping access scopes and extend pruned liveness to cover them:
//
// This may also unnecessarily, but conservatively extend liveness over some
// originally overlapping access, such as:
//
//     begin_access // access scope unrelated to def
//     %def
//     use %def
//     destroy %def
//     end_access
//
// Or:
//
//     %def
//     begin_access // access scope unrelated to def
//     use %def
//     destroy %def
//     end_access
//
// To minimize unnecessary lifetime extension, only search for end_access
// within dead blocks that are backward reachable from an original destroy.
//
// Note that lifetime extension is iterative because adding a new liveness use
// may create new overlapping access scopes. This can happen because there is no
// guarantee of strict stack discipline across unrelated access. For example:
//
//     %def
//     begin_access A
//     use %def        // Initial pruned lifetime boundary
//     begin_access B
//     end_access A    // Lifetime boundary after first extension
//     end_access B    // Lifetime boundary after second extension
//     destroy %def
//
// If the lifetime extension did not iterate, then def would be destroyed within
// B's access scope when originally it was destroyed outside that scope.
void CanonicalizeOSSALifetime::extendLivenessThroughOverlappingAccess() {
  this->accessBlocks = accessBlockAnalysis->get(getCurrentDef()->getFunction());

  // Visit each original consuming use or destroy as the starting point for a
  // backward CFG traversal. This traversal must only visit blocks within the
  // original extended lifetime.
  bool changed = true;
  while (changed) {
    changed = false;
    // The blocks in which we may have to extend liveness over access scopes.
    //
    // It must be populated first so that we can test membership during the loop
    // (see findLastConsume).
    BasicBlockSetVector blocksToVisit(currentDef->getFunction());
    for (auto *block : consumingBlocks) {
      blocksToVisit.insert(block);
    }
    for (auto iterator = blocksToVisit.begin(); iterator != blocksToVisit.end();
         ++iterator) {
      auto *bb = *iterator;
      // If the block isn't dead, then we won't need to extend liveness within
      // any of its predecessors (though we may within it).
      if (liveness.getBlockLiveness(bb) != PrunedLiveBlocks::Dead)
        continue;
      // Continue searching upward to find the pruned liveness boundary.
      for (auto *predBB : bb->getPredecessorBlocks()) {
        blocksToVisit.insert(predBB);
      }
    }
    for (auto *bb : blocksToVisit) {
      auto blockLiveness = liveness.getBlockLiveness(bb);
      // Ignore blocks within pruned liveness.
      if (blockLiveness == PrunedLiveBlocks::LiveOut) {
        continue;
      }
      if (blockLiveness == PrunedLiveBlocks::Dead) {
        // Otherwise, ignore dead blocks with no nonlocal end_access.
        if (!accessBlocks->containsNonLocalEndAccess(bb)) {
          continue;
        }
      }
      bool blockHasUse = (blockLiveness == PrunedLiveBlocks::LiveWithin);
      // Find the latest partially overlapping access scope, if one exists:
      //     use %def // pruned liveness ends here
      //     end_access

      // Whether to look for the last consume in the block.
      //
      // We need to avoid extending liveness over end_accesses that occur after
      // original liveness ended.
      bool findLastConsume =
          consumingBlocks.contains(bb) &&
          llvm::none_of(bb->getSuccessorBlocks(), [&](auto *successor) {
            return blocksToVisit.contains(successor) &&
                   liveness.getBlockLiveness(successor) ==
                       PrunedLiveBlocks::Dead;
          });
      for (auto &inst : llvm::reverse(*bb)) {
        if (findLastConsume) {
          findLastConsume = !destroys.contains(&inst);
          continue;
        }
        // Stop at the latest use. An earlier end_access does not overlap.
        if (blockHasUse && liveness.isInterestingUser(&inst)) {
          break;
        }
        if (endsAccessOverlappingPrunedBoundary(&inst)) {
          liveness.updateForUse(&inst, /*lifetimeEnding*/ false);
          changed = true;
          break;
        }
      }
      // If liveness changed, might as well restart CFG traversal.
      if (changed) {
        break;
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// MARK: Step 2. Find the destroy points of the current def based on the pruned
// liveness computed in Step 1.
//===----------------------------------------------------------------------===//

// Look past destroys and incidental uses to find a destroy on \p edgeBB that
// destroys \p def.
static DestroyValueInst *findDestroyOnCFGEdge(SILBasicBlock *edgeBB,
                                              SILValue def) {
  for (auto &inst : *edgeBB) {
    if (isIncidentalUse(&inst))
      continue;
    if (auto *destroy = dyn_cast<DestroyValueInst>(&inst)) {
      if (destroy->getOperand() == def)
        return destroy;
      continue;
    }
    break;
  }
  return nullptr;
}

/// The liveness boundary is at a CFG edge `predBB` -> `succBB`, meaning that
/// `currentDef` is live out of at least one other `predBB` successor.
///
/// Create and record a final destroy_value at the beginning of `succBB`
/// (assuming no critical edges).
///
/// Avoid deleting and recreating a destroy that was already placed on this
/// edge. Ignore any intervening destroys that may have been placed while
/// canonicalizing other values.  This is especially important when
/// canonicalization is called within an iterative worklist such as SILCombine.
void CanonicalizeOSSALifetime::findOrInsertDestroyOnCFGEdge(
    SILBasicBlock *predBB, SILBasicBlock *succBB) {

  assert(succBB->getSinglePredecessorBlock() == predBB
         && "value is live-out on another predBB successor: critical edge?");
  auto *di = findDestroyOnCFGEdge(succBB, currentDef);
  if (!di) {
    auto pos = succBB->begin();
    SILBuilderWithScope builder(pos);
    auto loc = RegularLocation::getAutoGeneratedLocation(pos->getLoc());
    di = builder.createDestroyValue(loc, currentDef);
    getCallbacks().createdNewInst(di);
  }
  consumes.recordFinalConsume(di);

  ++NumDestroysGenerated;
  LLVM_DEBUG(llvm::dbgs() << "  Destroy on edge " << *di);
}

/// This liveness boundary is within a basic block at the given position.
///
/// Create a final destroy, immediately after `pos`.
static void insertDestroyAtInst(SILBasicBlock::iterator pos,
                                DestroyValueInst *existingDestroy, SILValue def,
                                CanonicalOSSAConsumeInfo &consumes,
                                InstModCallbacks &callbacks) {
  if (existingDestroy) {
    for (; pos != existingDestroy->getIterator(); ++pos) {
      if (auto *debugVal = dyn_cast<DebugValueInst>(&*pos)) {
        consumes.popDebugAfterConsume(debugVal);
      }
    }
    consumes.recordFinalConsume(existingDestroy);
    return;
  }
  SILBuilderWithScope builder(pos);
  auto loc = RegularLocation::getAutoGeneratedLocation((*pos).getLoc());
  auto *di = builder.createDestroyValue(loc, def);
  callbacks.createdNewInst(di);
  consumes.recordFinalConsume(di);

  ++NumDestroysGenerated;
  LLVM_DEBUG(llvm::dbgs() << "  Destroy at last use " << *di);
}

// The pruned liveness boundary is within the given basic block. Find the
// block's last use. If the last use consumes the value, record it as a
// destroy. Otherwise, insert a new destroy_value.
//
// TODO: This has become quite a hack. Instead, the final liveness boundary
// should be returned in a data structure along with summary information about
// each block. Then any special logic for handling existing destroys and debug
// values should be applied to that block summary which can provide the input
// to rewriteCopies.
void CanonicalizeOSSALifetime::findOrInsertDestroyInBlock(SILBasicBlock *bb) {
  auto *defInst = currentDef->getDefiningInstruction();
  DestroyValueInst *existingDestroy = nullptr;
  auto instIter = bb->getTerminator()->getIterator();
  while (true) {
    auto *inst = &*instIter;

    if (pruneDebugMode) {
      if (auto *dvi = dyn_cast<DebugValueInst>(inst)) {
        if (debugValues.erase(dvi))
          consumes.recordDebugAfterConsume(dvi);
      }
    }
    switch (liveness.isInterestingUser(inst)) {
    case PrunedLiveness::NonUser:
      break;
    case PrunedLiveness::NonLifetimeEndingUse:
      // Insert a destroy after this non-consuming use.
      if (isa<TermInst>(inst)) {
        for (auto &succ : bb->getSuccessors()) {
          findOrInsertDestroyOnCFGEdge(bb, succ);
        }
      } else {
        insertDestroyAtInst(std::next(instIter), existingDestroy, currentDef,
                            consumes, getCallbacks());
      }
      return;
    case PrunedLiveness::LifetimeEndingUse:
      // This use becomes a final consume.
      consumes.recordFinalConsume(inst);
      return;
    }
    // This is not a potential last user. Keep scanning.
    // Allow lifetimes to be artificially extended up to the next non-ignored
    // instruction. The goal is to prevent repeated destroy rewriting without
    // inhibiting optimization.
    if (!ignoredByDestroyHoisting(inst->getKind())) {
      existingDestroy = nullptr;
    } else if (!existingDestroy) {
      if (auto *destroy = dyn_cast<DestroyValueInst>(inst)) {
        auto destroyDef = CanonicalizeOSSALifetime::getCanonicalCopiedDef(
            destroy->getOperand());
        if (destroyDef == currentDef) {
          existingDestroy = destroy;
        }
      }
    }
    if (instIter == bb->begin()) {
      assert(cast<SILArgument>(currentDef)->getParent() == bb);
      insertDestroyAtInst(instIter, existingDestroy, currentDef, consumes,
                          getCallbacks());
      return;
    }
    --instIter;
    // If the original def is reached, this is a dead live range. Insert a
    // destroy immediately after the def.
    if (&*instIter == defInst) {
      insertDestroyAtInst(std::next(instIter), existingDestroy, currentDef,
                          consumes, getCallbacks());
      return;
    }
  }
}

/// Populate `consumes` with the final destroy points once copies are
/// eliminated. This only applies to owned values.
///
/// Observations:
/// - currentDef must be postdominated by some subset of its
///   consuming uses, including destroys on all return paths.
/// - The postdominating consumes cannot be within nested loops.
/// - Any blocks in nested loops are now marked LiveOut.
///
/// TODO: replace this with PrunedLivenessAnalysis::computeBoundary. Separate
/// out destroy insertion, debug info, diagnostics, etc. as post-passes.
void CanonicalizeOSSALifetime::findOrInsertDestroys() {
  this->accessBlocks = accessBlockAnalysis->get(getCurrentDef()->getFunction());

  // Visit each original consuming use or destroy as the starting point for a
  // backward CFG traversal.
  blockWorklist.initializeRange(consumingBlocks);
  while (auto *bb = blockWorklist.pop()) {
    // Process each block that has not been visited and is not LiveOut.
    switch (liveness.getBlockLiveness(bb)) {
    case PrunedLiveBlocks::LiveOut:
      // A lifetimeEndBlock may be determined to be LiveOut after analyzing the
      // liveness. It is irrelevant for finding the boundary.
      break;
    case PrunedLiveBlocks::LiveWithin: {
      // The liveness boundary is inside this block. Insert a final destroy
      // inside the block if it doesn't already have one.
      findOrInsertDestroyInBlock(bb);
      break;
    }
    case PrunedLiveBlocks::Dead:
      // Continue searching upward to find the pruned liveness boundary.
      for (auto *predBB : bb->getPredecessorBlocks()) {
        if (liveness.getBlockLiveness(predBB) == PrunedLiveBlocks::LiveOut) {
          findOrInsertDestroyOnCFGEdge(predBB, bb);
        } else {
          blockWorklist.insert(predBB);
        }
      }
      break;
    }
  }
}

//===----------------------------------------------------------------------===//
// MARK: Step 3. Rewrite copies and destroys
//===----------------------------------------------------------------------===//

/// Revisit the def-use chain of currentDef. Mark unneeded original
/// copies and destroys for deletion. Insert new copies for interior uses that
/// require ownership of the used operand.
void CanonicalizeOSSALifetime::rewriteCopies() {
  assert(currentDef->getOwnershipKind() == OwnershipKind::Owned);

  SmallSetVector<SILInstruction *, 8> instsToDelete;
  defUseWorklist.clear();

  // Visit each operand in the def-use chain.
  //
  // Return true if the operand can use the current definition. Return false if
  // it requires a copy.
  auto visitUse = [&](Operand *use) {
    auto *user = use->getUser();
    // Recurse through copies.
    if (auto *copy = dyn_cast<CopyValueInst>(user)) {
      defUseWorklist.insert(copy);
      return true;
    }
    if (auto *destroy = dyn_cast<DestroyValueInst>(user)) {
      // If this destroy was marked as a final destroy, ignore it; otherwise,
      // delete it.
      if (!consumes.claimConsume(destroy)) {
        instsToDelete.insert(destroy);
        LLVM_DEBUG(llvm::dbgs() << "  Removing " << *destroy);
        ++NumDestroysEliminated;
      }
      return true;
    }

    // Nonconsuming uses do not need copies and cannot be marked as destroys.
    // A lifetime-ending use here must be a consume because EndBorrow/Reborrow
    // uses have been filtered out.
    if (!use->isLifetimeEnding())
      return true;

    // If this use was not marked as a final destroy *or* this is not the first
    // consumed operand we visited, then it needs a copy.
    if (!consumes.claimConsume(user)) {
      maybeNotifyMoveOnlyCopy(use);
      return false;
    }

    // Ok, this is a final user that isn't a destroy_value. Notify our caller if
    // we were asked to.
    //
    // If we need this for diagnostics, we will only use it if we found actual
    // uses that required copies.
    maybeNotifyFinalConsumingUse(use);

    return true;
  };

  // Perform a def-use traversal, visiting each use operand.
  for (auto useIter = currentDef->use_begin(), endIter = currentDef->use_end();
       useIter != endIter;) {
    Operand *use = *useIter++;
    if (!visitUse(use)) {
      copyLiveUse(use, getCallbacks());
    }
  }
  while (SILValue value = defUseWorklist.pop()) {
    CopyValueInst *srcCopy = cast<CopyValueInst>(value);
    // Recurse through copies while replacing their uses.
    Operand *reusedCopyOp = nullptr;
    for (auto useIter = srcCopy->use_begin(); useIter != srcCopy->use_end();) {
      Operand *use = *useIter++;
      if (!visitUse(use)) {
        if (!reusedCopyOp && srcCopy->getParent() == use->getParentBlock()) {
          reusedCopyOp = use;
        } else {
          copyLiveUse(use, getCallbacks());
        }
      }
    }
    if (!(reusedCopyOp && srcCopy->hasOneUse())) {
      getCallbacks().replaceValueUsesWith(srcCopy, srcCopy->getOperand());
      if (reusedCopyOp) {
        reusedCopyOp->set(srcCopy);
      } else {
        if (instsToDelete.insert(srcCopy)) {
          LLVM_DEBUG(llvm::dbgs() << "  Removing " << *srcCopy);
          ++NumCopiesEliminated;
        }
      }
    }
  }
  assert(!consumes.hasUnclaimedConsumes());

  // Add any debug_values from Dead blocks into the debugAfterConsume set.
  for (auto *dvi : debugValues) {
    if (liveness.getBlockLiveness(dvi->getParent()) == PrunedLiveBlocks::Dead) {
      consumes.recordDebugAfterConsume(dvi);
    }
  }

  // Remove any dead, non-recovered debug_values.
  for (auto *dvi : consumes.getDebugInstsAfterConsume()) {
    LLVM_DEBUG(llvm::dbgs() << "  Removing debug_value: " << *dvi);
    deleter.forceDelete(dvi);
  }

  // Remove the leftover copy_value and destroy_value instructions.
  for (unsigned idx = 0, eidx = instsToDelete.size(); idx != eidx; ++idx) {
    deleter.forceDelete(instsToDelete[idx]);
  }
}

//===----------------------------------------------------------------------===//
//                            MARK: Top-Level API
//===----------------------------------------------------------------------===//

/// Canonicalize a single extended owned lifetime.
bool CanonicalizeOSSALifetime::canonicalizeValueLifetime(SILValue def) {
  if (def->getOwnershipKind() != OwnershipKind::Owned)
    return false;

  if (def->isLexical())
    return false;

  LLVM_DEBUG(llvm::dbgs() << "  Canonicalizing: " << def);

  // Note: There is no need to register callbacks with this utility. 'onDelete'
  // is the only one in use to handle dangling pointers, which could be done
  // instead be registering a temporary handler with the pass. Canonicalization
  // is only allowed to create and delete instructions that are associated with
  // this canonical def (copies and destroys). Each canonical def has a disjoint
  // extended lifetime. Any pass calling this utility should work at the level
  // canonical defs, not individual instructions.
  //
  // NotifyWillBeDeleted will not work because copy rewriting removes operands
  // before deleting instructions. Also prohibit setUse callbacks just because
  // that would simply be insane.
  assert(!getCallbacks().notifyWillBeDeletedFunc
         && !getCallbacks().setUseValueFunc && "unsupported");

  initDef(def);
  // Step 1: compute liveness
  if (!computeCanonicalLiveness()) {
    LLVM_DEBUG(llvm::errs() << "Failed to compute canonical liveness?!\n");
    clearLiveness();
    return false;
  }
  extendLivenessThroughOverlappingAccess();
  // Step 2: record final destroys
  findOrInsertDestroys();
  // Step 3: rewrite copies and delete extra destroys
  rewriteCopies();

  clearLiveness();
  consumes.clear();
  return true;
}

//===----------------------------------------------------------------------===//
//                              MARK: Debugging
//===----------------------------------------------------------------------===//

SWIFT_ASSERT_ONLY_DECL(
  void CanonicalOSSAConsumeInfo::dump() const {
    llvm::dbgs() << "Consumes:";
    for (auto &blockAndInst : finalBlockConsumes) {
      llvm::dbgs() << "  " << *blockAndInst.getSecond();
    }
  })
