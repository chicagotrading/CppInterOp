//===--- MemoryEffects.cpp - Memory effects from optimized IR ---*- C++ -*-===//
//
// Part of the compiler-research project, under the Apache License v2.0 with
// LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MemoryEffects.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Demangle/ItaniumDemangle.h"
#include "llvm/Demangle/MicrosoftDemangle.h"
#include "llvm/Demangle/MicrosoftDemangleNodes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace CppInternal {

namespace {

using llvm::ModRefInfo;

Cpp::ModRef ToModRef(ModRefInfo MR) {
  switch (MR) {
  case ModRefInfo::NoModRef:
    return Cpp::ModRef::None;
  case ModRefInfo::Ref:
    return Cpp::ModRef::Read;
  case ModRefInfo::Mod:
    return Cpp::ModRef::Write;
  case ModRefInfo::ModRef:
    return Cpp::ModRef::ReadWrite;
  }
  return Cpp::ModRef::ReadWrite;
}

char AccessChar(ModRefInfo MR) {
  switch (MR) {
  case ModRefInfo::NoModRef:
    return 'n';
  case ModRefInfo::Ref:
    return 'r';
  case ModRefInfo::Mod:
    return 'w';
  case ModRefInfo::ModRef:
    return 'm';
  }
  return 'm';
}

bool ParseAccess(char C, ModRefInfo& MR) {
  switch (C) {
  case 'n':
    MR = ModRefInfo::NoModRef;
    return true;
  case 'r':
    MR = ModRefInfo::Ref;
    return true;
  case 'w':
    MR = ModRefInfo::Mod;
    return true;
  case 'm':
    MR = ModRefInfo::ModRef;
    return true;
  }
  return false;
}

// Allocation, exception and unwinding runtime entry points. They touch only
// their arguments and memory that the program cannot name, but the runtime
// that defines them is prebuilt, so the module has no body for them.
bool IsRuntimeSupport(llvm::StringRef Name) {
  static constexpr llvm::StringLiteral Names[] = {
      "_Znwm",
      "_Znam",
      "_ZnwmRKSt9nothrow_t",
      "_ZnamRKSt9nothrow_t",
      "_ZnwmSt11align_val_t",
      "_ZnamSt11align_val_t",
      "_ZdlPv",
      "_ZdaPv",
      "_ZdlPvm",
      "_ZdaPvm",
      "_ZdlPvSt11align_val_t",
      "_ZdaPvSt11align_val_t",
      "_ZdlPvmSt11align_val_t",
      "_ZdaPvmSt11align_val_t",
      "__cxa_allocate_exception",
      "__cxa_free_exception",
      "__cxa_throw",
      "__cxa_rethrow",
      "__cxa_begin_catch",
      "__cxa_end_catch",
      "__cxa_bad_cast",
      "__cxa_bad_typeid",
      "__cxa_throw_bad_array_new_length",
      "__cxa_call_unexpected",
      "__clang_call_terminate",
      "_Unwind_Resume",
      "_ZSt9terminatev",
  };
  if (std::find(std::begin(Names), std::end(Names), Name) != std::end(Names))
    return true;
  // std::__throw_length_error and the other libstdc++ throw helpers.
  return Name.starts_with("_ZSt") && Name.contains("__throw_");
}

// The runtime keeps no pointer that it is given to free, and the exception
// object that a handler receives is memory only the runtime names.
bool IsPointerSink(llvm::StringRef Name) {
  return Name.starts_with("_Zdl") || Name.starts_with("_Zda") ||
         Name == "__cxa_free_exception" || Name == "__cxa_begin_catch" ||
         Name == "__clang_call_terminate";
}

// The exception object of a throw is fresh memory that only the runtime and
// the catch handlers see, so the analysis gives it a local slot. A throw
// with a destructor stays as it is: the runtime calls the destructor.
void LocalizeThrows(llvm::Module& M) {
  llvm::Function* Alloc = M.getFunction("__cxa_allocate_exception");
  llvm::Function* Throw = M.getFunction("__cxa_throw");
  if (!Alloc || !Throw)
    return;
  llvm::FunctionCallee Stub = M.getOrInsertFunction(
      "__cppinterop_analysis_throw", Throw->getFunctionType());
  auto* StubF = llvm::cast<llvm::Function>(Stub.getCallee());
  StubF->setDoesNotReturn();
  StubF->setMemoryEffects(llvm::MemoryEffects::inaccessibleMemOnly());
  for (unsigned I = 0; I < StubF->arg_size(); ++I) {
    StubF->addParamAttr(I, llvm::Attribute::ReadNone);
    StubF->addParamAttr(I, llvm::Attribute::getWithCaptureInfo(
                               M.getContext(), llvm::CaptureInfo::none()));
  }

  llvm::SmallVector<llvm::CallBase*, 8> Throws;
  for (llvm::User* U : Throw->users())
    if (auto* CB = llvm::dyn_cast<llvm::CallBase>(U))
      if (CB->getCalledFunction() == Throw &&
          llvm::isa<llvm::ConstantPointerNull>(CB->getArgOperand(2)))
        Throws.push_back(CB);
  for (llvm::CallBase* CB : Throws) {
    auto* Obj = llvm::dyn_cast<llvm::CallInst>(CB->getArgOperand(0));
    auto* Size = Obj && Obj->getCalledFunction() == Alloc
                     ? llvm::dyn_cast<llvm::ConstantInt>(Obj->getArgOperand(0))
                     : nullptr;
    if (!Size)
      continue;
    llvm::BasicBlock& Entry = Obj->getFunction()->getEntryBlock();
    llvm::IRBuilder<> B(&Entry, Entry.getFirstInsertionPt());
    llvm::AllocaInst* Slot = B.CreateAlloca(B.getInt8Ty(), Size);
    Slot->setAlignment(llvm::Align(16));
    Obj->replaceAllUsesWith(Slot);
    Obj->eraseFromParent();
    CB->setCalledFunction(Stub);
  }
}

// A noexcept function reaches out-of-line code through an invoke whose
// landing pad calls __clang_call_terminate, a helper that clang defines in
// the module. It never returns and its argument is memory only the runtime
// names, so its body must not count as an effect of the caller: it becomes
// a declaration that PrepareForAnalysis bounds like the other runtime entry
// points.
void LocalizeTerminate(llvm::Module& M) {
  llvm::Function* F = M.getFunction("__clang_call_terminate");
  if (!F || F->isDeclaration())
    return;
  F->deleteBody();
  F->setComdat(nullptr);
  F->setDoesNotReturn();
}

//===----------------------------------------------------------------------===//
// Reachable-memory attribution
//
// LLVM's attributes merge the memory reachable from a parameter (a
// container's heap) with globals and unknown pointers into "other" memory,
// and a parameter whose address is only compared loses its access bound.
// This pass attributes every access of the optimized code to the pointer
// parameter whose memory it is in, by the origin of the access's pointer:
//
//   Direct(P)  a pointer based on parameter P (through GEPs, casts, phis
//              and selects): the parameter's own object;
//   Loaded(P)  a pointer loaded from memory attributed to P, transitively,
//              or returned by a callee from such memory;
//   Object(A)  an alloca or the result of allocation A (a noalias call):
//              memory that did not exist when the function was called;
//   Global     a non-constant global; Unknown anything else (inttoptr, a
//              load from an object, an opaque result).
//
// Soundness conditions, enforced below:
//
// 1. An access counts for P only when every origin of its pointer is Direct
//    or Loaded of P. A pointer with a Global or Unknown origin counts as
//    other memory as well; one with origins in two parameters counts for
//    both. Accesses to an object's own bytes are not visible to the caller.
// 2. The memory reachable from P must not become reachable from elsewhere,
//    or a later write through another pointer could change it: P is
//    captured when a pointer into its memory is stored into memory that P
//    does not own, is kept by a callee that may store it anywhere, or leaves
//    as an integer (a ptrtoint whose arithmetic reaches a store, a call, a
//    return or an inttoptr). Comparing addresses, or subtracting one from
//    another for an offset, is not a capture (LLVM 21's capture
//    components). A capture
//    makes the attribution to P advisory: the caller must treat P as
//    possibly written.
// 3. An object belongs to the parameter whose memory holds a pointer to it,
//    so storing a pointer to P's memory into an object that only P holds is
//    not a capture. An object that escapes (stored to global or unknown
//    memory, held by two parameters, returned, or kept by an opaque callee)
//    captures every parameter it holds a pointer to.
// 4. A callee's effects apply to the origins of its arguments. A callee that
//    touches no other memory and receives memory of one parameter only (and
//    objects) keeps every pointer it stores inside that parameter's memory,
//    so its captures are internal; any other callee's captures are real.
//===----------------------------------------------------------------------===//

// What a callee does through one pointer parameter.
struct ParamReach {
  ModRefInfo Direct = ModRefInfo::NoModRef;
  ModRefInfo Reachable = ModRefInfo::NoModRef;
  bool Captured = false;
};

// The reachable-memory summary of a function.
struct Reach {
  ModRefInfo Other = ModRefInfo::NoModRef;
  std::vector<ParamReach> Params;
  // Where a pointer result may point: into a parameter's own object, into
  // memory loaded from a parameter, into fresh memory, or elsewhere.
  llvm::SmallBitVector RetDirect, RetLoaded;
  bool RetFresh = false, RetUnknown = false;

  explicit Reach(unsigned N = 0) : Params(N), RetDirect(N), RetLoaded(N) {}

  static Reach conservative(unsigned N) {
    Reach R(N);
    R.Other = ModRefInfo::ModRef;
    for (ParamReach& P : R.Params)
      P = {ModRefInfo::ModRef, ModRefInfo::ModRef, true};
    R.RetUnknown = true;
    return R;
  }
};

// One parameter of a summary: how LLVM bounds the access through it and
// whether it keeps a copy of the pointer.
struct ParamSummary {
  char Access;  // 'n' readnone, 'r' readonly, 'w' writeonly, '-' unknown
  char Capture; // 'c' none, 'a' address only, '.' provenance
};

struct SummaryVariant {
  uint32_t Effects = 0; // llvm::MemoryEffects::toIntValue()
  std::vector<ParamSummary> Params;
  Reach R;
};

// An opaque call inside the analysis: a declaration by its mangled name, an
// indirect call, or inline assembly.
struct OpaqueRef {
  Cpp::OpaqueKind Kind = Cpp::OpaqueKind::Declaration;
  std::string Mangled;

  friend bool operator==(const OpaqueRef& A, const OpaqueRef& B) {
    return A.Kind == B.Kind && A.Mangled == B.Mangled;
  }
  friend bool operator<(const OpaqueRef& A, const OpaqueRef& B) {
    return std::tie(A.Kind, A.Mangled) < std::tie(B.Kind, B.Mangled);
  }
};

void AddUnique(std::vector<OpaqueRef>& Out, const OpaqueRef& Ref) {
  if (!llvm::is_contained(Out, Ref))
    Out.push_back(Ref);
}

// The opaque calls a function reaches, and among them those that a table
// marks as bounded by no signature (the caller adds its own unbounded ones).
struct Reached {
  std::vector<OpaqueRef> Opaque, Unbounded;
};

struct FunctionSummary {
  SummaryVariant Plain;
  // The effects when the opaque calls of the body are trusted to their
  // signatures; equal to Plain when the summary is complete.
  SummaryVariant Trusted;
  bool Incomplete = false;
  // The opaque calls that the body reaches.
  Reached Opaque;
};

llvm::StringMap<FunctionSummary>& Summaries() {
  static llvm::StringMap<FunctionSummary> S;
  return S;
}

constexpr llvm::StringLiteral SummaryHeader =
    "# cppinterop-memory-effects v1 llvm ";

// The table spells an opaque call as the mangled name of a declaration or as
// one of these; a leading '!' marks one that no signature bounds.
constexpr llvm::StringLiteral IndirectToken = "*indirect";
constexpr llvm::StringLiteral InlineAsmToken = "*asm";

std::string TableToken(const OpaqueRef& Ref, bool Unbounded) {
  std::string Out = Unbounded ? "!" : "";
  switch (Ref.Kind) {
  case Cpp::OpaqueKind::Indirect:
    return Out + IndirectToken.str();
  case Cpp::OpaqueKind::InlineAsm:
    return Out + InlineAsmToken.str();
  case Cpp::OpaqueKind::Declaration:
    break;
  }
  return Out + Ref.Mangled;
}

OpaqueRef ParseTableToken(llvm::StringRef Token, bool& Unbounded) {
  Unbounded = Token.consume_front("!");
  if (Token == IndirectToken)
    return {Cpp::OpaqueKind::Indirect, {}};
  if (Token == InlineAsmToken)
    return {Cpp::OpaqueKind::InlineAsm, {}};
  return {Cpp::OpaqueKind::Declaration, Token.str()};
}

void ApplySummary(llvm::Function& F, const SummaryVariant& S) {
  F.setMemoryEffects(llvm::MemoryEffects::createFromIntValue(S.Effects));
  for (unsigned I = 0; I < S.Params.size() && I < F.arg_size(); ++I) {
    const ParamSummary& P = S.Params[I];
    if (P.Access == 'n')
      F.addParamAttr(I, llvm::Attribute::ReadNone);
    else if (P.Access == 'r')
      F.addParamAttr(I, llvm::Attribute::ReadOnly);
    else if (P.Access == 'w')
      F.addParamAttr(I, llvm::Attribute::WriteOnly);
    if (P.Capture == 'c')
      F.addParamAttr(I, llvm::Attribute::getWithCaptureInfo(
                            F.getContext(), llvm::CaptureInfo::none()));
    else if (P.Capture == 'a')
      F.addParamAttr(I,
                     llvm::Attribute::getWithCaptureInfo(
                         F.getContext(),
                         llvm::CaptureInfo(llvm::CaptureComponents::Address)));
  }
}

// Where a pointer value may point, as the caller of the function sees it.
struct Origin {
  llvm::SmallBitVector Direct, Loaded;
  llvm::SmallVector<const llvm::Value*, 2> Objs; // sorted
  bool Global = false, Unknown = false;

  explicit Origin(unsigned N) : Direct(N), Loaded(N) {}

  bool join(const Origin& O) {
    bool Changed = (O.Global && !Global) || (O.Unknown && !Unknown);
    Global |= O.Global;
    Unknown |= O.Unknown;
    // test(X): some bit of X is not set here.
    if (O.Direct.test(Direct)) {
      Direct |= O.Direct;
      Changed = true;
    }
    if (O.Loaded.test(Loaded)) {
      Loaded |= O.Loaded;
      Changed = true;
    }
    for (const llvm::Value* A : O.Objs) {
      auto It = std::lower_bound(Objs.begin(), Objs.end(), A);
      if (It == Objs.end() || *It != A) {
        Objs.insert(It, A);
        Changed = true;
      }
    }
    return Changed;
  }
  llvm::SmallBitVector params() const {
    llvm::SmallBitVector P = Direct;
    P |= Loaded;
    return P;
  }
  bool anyParam() const { return Direct.any() || Loaded.any(); }
  // Memory that the analysis cannot attribute to a parameter or an object.
  bool unowned() const { return Global || Unknown; }
};

// Who holds the only pointers to an object.
struct Owner {
  enum Kind { Unstored, Param, Escaped } K = Unstored;
  unsigned P = 0;

  bool join(Owner O) {
    if (O.K == Unstored || K == Escaped)
      return false;
    if (K == Unstored || (K == Param && O.K == Param && P == O.P)) {
      bool Changed = K != O.K;
      *this = O;
      return Changed;
    }
    K = Escaped;
    return true;
  }
};

class ReachAnalysis {
public:
  explicit ReachAnalysis(bool Trusted) : Trusted(Trusted) {}

  const Reach& of(const llvm::Function& F) {
    auto It = Done.find(&F);
    if (It != Done.end())
      return *It->second;
    if (!Busy.insert(&F).second) {
      // Recursion: the callee is bounded by nothing yet.
      return *(Done[&F] =
                   std::make_unique<Reach>(Reach::conservative(F.arg_size())));
    }
    Reach R = analyze(F);
    Busy.erase(&F);
    return *(Done[&F] = std::make_unique<Reach>(std::move(R)));
  }

  // The callee's summary in the frame of call \p CB: one entry per argument.
  Reach ofCall(const llvm::CallBase& CB) {
    const unsigned N = CB.arg_size();
    if (!CB.isInlineAsm())
      if (const llvm::Function* Callee = CB.getCalledFunction()) {
        if (!Callee->isDeclaration())
          return padded(of(*Callee), N);
        auto It = Summaries().find(Callee->getName());
        if (It != Summaries().end())
          return padded((Trusted ? It->second.Trusted : It->second.Plain).R, N);
      }
    return fromAttributes(CB);
  }

private:
  bool Trusted;
  llvm::DenseMap<const llvm::Function*, std::unique_ptr<Reach>> Done;
  llvm::SmallPtrSet<const llvm::Function*, 8> Busy;

  // A variadic callee's extra arguments are bounded by nothing.
  static Reach padded(const Reach& R, unsigned N) {
    if (R.Params.size() == N)
      return R;
    Reach Out = R;
    Out.Params.resize(N);
    Out.RetDirect.resize(N);
    Out.RetLoaded.resize(N);
    for (unsigned K = R.Params.size(); K < N; ++K)
      Out.Params[K] = {ModRefInfo::ModRef, ModRefInfo::ModRef, true};
    return Out;
  }

  // A declaration: what its attributes (and the call site's) promise. LLVM's
  // argmem is the memory the arguments point to, so the callee reaches no
  // further unless it also accesses other memory.
  static Reach fromAttributes(const llvm::CallBase& CB) {
    const unsigned N = CB.arg_size();
    Reach R(N);
    llvm::MemoryEffects ME = CB.getMemoryEffects();
    R.Other = ME.getModRef(llvm::IRMemLocation::Other);
    const ModRefInfo Arg = ME.getModRef(llvm::IRMemLocation::ArgMem);
    const bool PtrResult = CB.getType()->isPointerTy();
    for (unsigned K = 0; K < N; ++K) {
      if (!CB.getArgOperand(K)->getType()->isPointerTy())
        continue;
      ModRefInfo MR = Arg;
      if (CB.paramHasAttr(K, llvm::Attribute::ReadNone))
        MR = ModRefInfo::NoModRef;
      else if (CB.paramHasAttr(K, llvm::Attribute::ReadOnly))
        MR &= ModRefInfo::Ref;
      else if (CB.paramHasAttr(K, llvm::Attribute::WriteOnly))
        MR &= ModRefInfo::Mod;
      R.Params[K].Direct = MR;
      llvm::CaptureInfo CI = CB.getCaptureInfo(K);
      R.Params[K].Captured =
          llvm::capturesAnyProvenance(CI.getOtherComponents());
      if (PtrResult && llvm::capturesAnyProvenance(CI.getRetComponents()))
        R.RetLoaded.set(K);
    }
    if (PtrResult) {
      if (CB.returnDoesNotAlias())
        R.RetFresh = true;
      else if (const llvm::Value* A = CB.getReturnedArgOperand()) {
        for (unsigned K = 0; K < N; ++K)
          if (CB.getArgOperand(K) == A)
            R.RetDirect.set(K);
      } else
        R.RetUnknown = true;
    }
    return R;
  }

  Reach analyze(const llvm::Function& F);
};

// The analysis of one function body.
class FunctionReach {
public:
  FunctionReach(ReachAnalysis& RA, const llvm::Function& F)
      : RA(RA), F(F), N(F.arg_size()), R(N) {}

  Reach run() {
    computeOrigins();
    computeTaint();
    computeOwners();
    collect();
    return std::move(R);
  }

private:
  ReachAnalysis& RA;
  const llvm::Function& F;
  const unsigned N;
  Reach R;
  llvm::DenseMap<const llvm::Value*, Origin> Origins;
  llvm::DenseMap<const llvm::CallBase*, Reach> Calls;
  llvm::DenseMap<const llvm::Value*, Owner> Owners;
  // Per object: the parameters whose memory it may hold pointers to, and
  // whether it may hold pointers of unknown origin.
  llvm::DenseMap<const llvm::Value*, llvm::SmallBitVector> Holds;
  llvm::SmallPtrSet<const llvm::Value*, 8> HoldsUnknown;
  // Per scalar: the parameters whose address it may carry (from ptrtoint).
  llvm::DenseMap<const llvm::Value*, llvm::SmallBitVector> Taint;

  const Reach& callee(const llvm::CallBase& CB) {
    auto It = Calls.find(&CB);
    if (It == Calls.end())
      It = Calls.try_emplace(&CB, RA.ofCall(CB)).first;
    return It->second;
  }

  static bool ignored(const llvm::Instruction& I) {
    if (I.isLifetimeStartOrEnd() || I.isDebugOrPseudoInst())
      return true;
    const auto* II = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
    return II && II->isAssumeLikeIntrinsic();
  }

  Origin constantOrigin(const llvm::Value* V) const {
    Origin O(N);
    const llvm::Value* UO = llvm::getUnderlyingObject(V);
    if (llvm::isa<llvm::ConstantPointerNull>(UO) ||
        llvm::isa<llvm::UndefValue>(UO) || llvm::isa<llvm::Function>(UO))
      return O; // no memory
    if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(UO)) {
      // Constant memory holds no value that changes.
      if (!GV->isConstant())
        O.Global = true;
      return O;
    }
    O.Unknown = true;
    return O;
  }

  Origin originOf(const llvm::Value* V) const {
    if (llvm::isa<llvm::Constant>(V))
      return constantOrigin(V);
    auto It = Origins.find(V);
    return It == Origins.end() ? Origin(N) : It->second;
  }

  // A pointer loaded from memory of origin \p O. An object may hold pointers
  // the analysis did not see stored, so its content is unknown; a
  // parameter's memory yields its reachable memory.
  Origin loadedFrom(const Origin& O) const {
    Origin R(N);
    R.Loaded = O.params();
    R.Unknown = O.unowned() || !O.Objs.empty();
    return R;
  }

  Origin callResult(const llvm::CallBase& CB) {
    const Reach& S = callee(CB);
    Origin O(N);
    if (S.RetFresh)
      O.Objs.push_back(&CB);
    O.Unknown = S.RetUnknown;
    for (unsigned K = 0; K < CB.arg_size(); ++K) {
      if (!CB.getArgOperand(K)->getType()->isPointerTy())
        continue;
      if (S.RetDirect.test(K))
        O.join(originOf(CB.getArgOperand(K)));
      if (S.RetLoaded.test(K))
        O.join(loadedFrom(originOf(CB.getArgOperand(K))));
    }
    return O;
  }

  Origin transfer(const llvm::Instruction& I) {
    Origin O(N);
    switch (I.getOpcode()) {
    case llvm::Instruction::Alloca:
      O.Objs.push_back(&I);
      return O;
    case llvm::Instruction::GetElementPtr:
    case llvm::Instruction::BitCast:
    case llvm::Instruction::AddrSpaceCast:
    case llvm::Instruction::Freeze:
      return originOf(I.getOperand(0));
    case llvm::Instruction::PHI:
      for (const llvm::Value* In :
           llvm::cast<llvm::PHINode>(I).incoming_values())
        O.join(originOf(In));
      return O;
    case llvm::Instruction::Select:
      O.join(originOf(I.getOperand(1)));
      O.join(originOf(I.getOperand(2)));
      return O;
    case llvm::Instruction::Load:
      return loadedFrom(originOf(I.getOperand(0)));
    case llvm::Instruction::Call:
    case llvm::Instruction::Invoke:
    case llvm::Instruction::CallBr:
      return callResult(llvm::cast<llvm::CallBase>(I));
    default:
      O.Unknown = true;
      return O;
    }
  }

  void computeOrigins() {
    for (const llvm::Argument& A : F.args())
      if (A.getType()->isPointerTy())
        Origins.try_emplace(&A, N).first->second.Direct.set(A.getArgNo());
    llvm::SmallVector<const llvm::Instruction*, 64> Pointers;
    for (const llvm::Instruction& I : llvm::instructions(F))
      if (I.getType()->isPointerTy())
        Pointers.push_back(&I);
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const llvm::Instruction* I : Pointers) {
        Origin New = transfer(*I);
        Changed |= Origins.try_emplace(I, N).first->second.join(New);
      }
    }
  }

  // Arithmetic keeps an address inside the scalar it computes.
  static bool arithmetic(const llvm::Instruction& I) {
    if (llvm::isa<llvm::IntToPtrInst>(I) || llvm::isa<llvm::PtrToIntInst>(I))
      return false;
    if (llvm::isa<llvm::BinaryOperator>(I) ||
        llvm::isa<llvm::UnaryOperator>(I) || llvm::isa<llvm::CastInst>(I) ||
        llvm::isa<llvm::PHINode>(I) || llvm::isa<llvm::SelectInst>(I) ||
        llvm::isa<llvm::FreezeInst>(I))
      return true;
    const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
    return CB && CB->getIntrinsicID() != llvm::Intrinsic::not_intrinsic &&
           CB->doesNotAccessMemory();
  }

  static bool scalar(llvm::Type* T) {
    return T->isIntOrIntVectorTy() || T->isFPOrFPVectorTy();
  }

  llvm::SmallBitVector taintOf(const llvm::Value* V) const {
    auto It = Taint.find(V);
    return It == Taint.end() ? llvm::SmallBitVector(N) : It->second;
  }

  void computeTaint() {
    llvm::SmallVector<const llvm::Instruction*, 64> Scalars;
    for (const llvm::Instruction& I : llvm::instructions(F))
      if (scalar(I.getType()))
        Scalars.push_back(&I);
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const llvm::Instruction* I : Scalars) {
        llvm::SmallBitVector T(N);
        if (llvm::isa<llvm::PtrToIntInst>(I)) {
          T = originOf(I->getOperand(0)).params();
        } else if (I->getOpcode() == llvm::Instruction::Sub &&
                   taintOf(I->getOperand(0)).any() &&
                   taintOf(I->getOperand(1)).any()) {
          // The difference of two addresses is an offset, not an address.
        } else if (arithmetic(*I)) {
          for (const llvm::Value* Op : I->operands())
            if (scalar(Op->getType()))
              T |= taintOf(Op);
        } else {
          continue;
        }
        llvm::SmallBitVector& Cur = Taint.try_emplace(I, N).first->second;
        if (T.test(Cur)) {
          Cur |= T;
          Changed = true;
        }
      }
    }
  }

  // Whether the callee's stores stay inside the memory it is given
  // (condition 4), and that memory's single parameter if any.
  bool internal(const llvm::CallBase& CB, const Reach& S, Owner& Single) {
    Single = Owner{};
    if (S.Other != ModRefInfo::NoModRef)
      return false;
    llvm::SmallBitVector Seen(N);
    for (const llvm::Value* A : CB.args()) {
      if (!A->getType()->isPointerTy())
        continue;
      Origin O = originOf(A);
      if (O.unowned())
        return false;
      Seen |= O.params();
    }
    if (Seen.count() > 1)
      return false;
    if (Seen.any())
      Single = Owner{Owner::Param, static_cast<unsigned>(Seen.find_first())};
    return true;
  }

  Owner ownerOf(const Origin& D) {
    Owner W;
    if (D.unowned())
      W.K = Owner::Escaped;
    // set_bits() refers to its vector; a temporary dies before the loop body.
    const llvm::SmallBitVector Params = D.params();
    for (unsigned I : Params.set_bits())
      W.join(Owner{Owner::Param, I});
    for (const llvm::Value* A : D.Objs)
      W.join(Owners[A]);
    return W;
  }

  // Pointers with origin \p Src are copied into memory with origin \p Dst
  // (a pointer store, or a memcpy of bytes that may hold pointers).
  bool recordCopy(const Origin& Src, const Origin& Dst) {
    bool Changed = false;
    if (Src.Objs.empty() && !Src.anyParam() && !Src.unowned())
      return false;
    Owner W = ownerOf(Dst);
    for (const llvm::Value* A : Src.Objs)
      Changed |= Owners[A].join(W);
    for (const llvm::Value* A : Dst.Objs) {
      llvm::SmallBitVector& H = Holds.try_emplace(A, N).first->second;
      llvm::SmallBitVector P = Src.params();
      if (P.test(H)) {
        H |= P;
        Changed = true;
      }
      if (Src.unowned())
        Changed |= HoldsUnknown.insert(A).second;
    }
    return Changed;
  }

  static bool mayHoldPointers(llvm::Type* T) {
    if (T->isPointerTy())
      return true;
    if (auto* ST = llvm::dyn_cast<llvm::StructType>(T))
      return llvm::any_of(ST->elements(), mayHoldPointers);
    if (auto* AT = llvm::dyn_cast<llvm::ArrayType>(T))
      return mayHoldPointers(AT->getElementType());
    if (auto* VT = llvm::dyn_cast<llvm::VectorType>(T))
      return mayHoldPointers(VT->getElementType());
    return false;
  }

  // The origin of the pointers, or addresses, that a stored value may carry.
  Origin carried(const llvm::Value* V) const {
    if (V->getType()->isPointerTy())
      return originOf(V);
    Origin O(N);
    if (scalar(V->getType())) {
      O.Direct = taintOf(V);
      return O;
    }
    if (!mayHoldPointers(V->getType()))
      return O;
    if (const auto* L = llvm::dyn_cast<llvm::LoadInst>(V))
      return loadedFrom(originOf(L->getPointerOperand()));
    O.Unknown = true;
    return O;
  }

  void computeOwners() {
    for (bool Changed = true; Changed;) {
      Changed = false;
      for (const llvm::Instruction& I : llvm::instructions(F)) {
        if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
          Changed |= recordCopy(carried(SI->getValueOperand()),
                                originOf(SI->getPointerOperand()));
        } else if (const auto* MT = llvm::dyn_cast<llvm::MemTransferInst>(&I)) {
          Changed |= recordCopy(loadedFrom(originOf(MT->getRawSource())),
                                originOf(MT->getRawDest()));
        } else if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
          if (ignored(I) || llvm::isa<llvm::MemIntrinsic>(I))
            continue;
          Changed |= ownersAtCall(*CB);
        } else if (const auto* RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
          if (const llvm::Value* V = RI->getReturnValue())
            for (const llvm::Value* A : carried(V).Objs)
              Changed |= Owners[A].join(Owner{Owner::Escaped, 0});
        }
      }
    }
  }

  bool ownersAtCall(const llvm::CallBase& CB) {
    const Reach& S = callee(CB);
    bool Changed = false;
    Owner Single;
    const bool Internal = internal(CB, S, Single);
    bool AnyCaptured = false;
    llvm::SmallVector<const llvm::Value*, 4> Touched;
    llvm::SmallBitVector Params(N);
    for (unsigned K = 0; K < CB.arg_size(); ++K) {
      const llvm::Value* A = CB.getArgOperand(K);
      if (!A->getType()->isPointerTy()) {
        // An address passed as a scalar: the callee may store it.
        llvm::SmallBitVector T =
            scalar(A->getType()) ? taintOf(A) : llvm::SmallBitVector(N);
        Params |= T;
        AnyCaptured |= T.any();
        continue;
      }
      Origin O = originOf(A);
      Params |= O.params();
      AnyCaptured |= S.Params[K].Captured;
      for (const llvm::Value* Ob : O.Objs) {
        Touched.push_back(Ob);
        if (!Internal && S.Params[K].Captured)
          Changed |= Owners[Ob].join(Owner{Owner::Escaped, 0});
        // A callee that writes it may store pointers of unknown origin.
        if (!Internal &&
            llvm::isModSet(S.Params[K].Direct | S.Params[K].Reachable))
          Changed |= HoldsUnknown.insert(Ob).second;
      }
    }
    if (S.RetFresh && CB.getType()->isPointerTy())
      Touched.push_back(&CB);
    if (!Internal || !AnyCaptured)
      return Changed;
    // The callee may link the objects to each other and to the parameter's
    // memory: they share one owner and may hold its pointers.
    Owner W = Single;
    for (const llvm::Value* Ob : Touched)
      W.join(Owners[Ob]);
    for (const llvm::Value* Ob : Touched) {
      Changed |= Owners[Ob].join(W);
      llvm::SmallBitVector& H = Holds.try_emplace(Ob, N).first->second;
      if (Params.test(H)) {
        H |= Params;
        Changed = true;
      }
    }
    return Changed;
  }

  void access(ModRefInfo MR, const Origin& O, bool Reachable) {
    if (MR == ModRefInfo::NoModRef)
      return;
    for (unsigned I : O.Direct.set_bits())
      (Reachable ? R.Params[I].Reachable : R.Params[I].Direct) |= MR;
    for (unsigned I : O.Loaded.set_bits())
      R.Params[I].Reachable |= MR;
    if (O.Global || O.Unknown)
      R.Other |= MR;
    if (!Reachable)
      return;
    // Memory reachable from an object: whatever it holds.
    for (const llvm::Value* A : O.Objs) {
      auto It = Holds.find(A);
      if (It != Holds.end())
        for (unsigned I : It->second.set_bits())
          R.Params[I].Reachable |= MR;
      if (HoldsUnknown.count(A))
        R.Other |= MR;
    }
  }

  void capture(const llvm::SmallBitVector& Params) {
    for (unsigned I : Params.set_bits())
      R.Params[I].Captured = true;
  }

  // A pointer into the memory of \p Src's parameters is copied into memory
  // of origin \p Dst (condition 2).
  void captureByCopy(const Origin& Src, const Origin& Dst) {
    const llvm::SmallBitVector SrcParams = Src.params();
    const llvm::SmallBitVector DstParams = Dst.params();
    for (unsigned I : SrcParams.set_bits()) {
      bool Own = !Dst.unowned();
      for (unsigned J : DstParams.set_bits())
        Own &= J == I;
      for (const llvm::Value* A : Dst.Objs) {
        Owner W = Owners[A];
        Own &= W.K == Owner::Unstored || (W.K == Owner::Param && W.P == I);
      }
      if (!Own)
        R.Params[I].Captured = true;
    }
  }

  // An address that leaves as a scalar (condition 2): into memory that its
  // parameter does not own, a callee that may keep it, or a pointer again.
  void captureScalar(const llvm::Value* V, const Origin* Dst = nullptr) {
    auto It = Taint.find(V);
    if (It == Taint.end() || It->second.none())
      return;
    if (!Dst)
      return capture(It->second);
    Origin Src(N);
    Src.Direct = It->second;
    captureByCopy(Src, *Dst);
  }

  void collectCall(const llvm::CallBase& CB) {
    const Reach& S = callee(CB);
    R.Other |= S.Other;
    Owner Single;
    const bool Internal = internal(CB, S, Single);
    const llvm::Function* Callee = CB.getCalledFunction();
    // Intrinsics and the runtime consume their scalars.
    const bool Consumes = Callee && (Callee->isIntrinsic() ||
                                     IsRuntimeSupport(Callee->getName()));
    for (unsigned K = 0; K < CB.arg_size(); ++K) {
      const llvm::Value* A = CB.getArgOperand(K);
      if (!A->getType()->isPointerTy()) {
        if (Consumes || !scalar(A->getType()))
          continue;
        llvm::SmallBitVector T = taintOf(A);
        if (Internal && Single.K == Owner::Param)
          T.reset(Single.P);
        capture(T);
        continue;
      }
      Origin O = originOf(A);
      access(S.Params[K].Direct, O, /*Reachable=*/false);
      access(S.Params[K].Reachable, O, /*Reachable=*/true);
      if (S.Params[K].Captured && !Internal)
        capture(O.params());
    }
  }

  void collectReturn(const llvm::Value* V) {
    if (!V)
      return;
    if (!V->getType()->isPointerTy()) {
      R.RetUnknown |= mayHoldPointers(V->getType());
      return;
    }
    Origin O = originOf(V);
    R.RetDirect |= O.Direct;
    R.RetLoaded |= O.Loaded;
    R.RetFresh |= !O.Objs.empty();
    R.RetUnknown |= O.unowned();
  }

  // Any other use of a pointer into a parameter's memory: LLVM says whether
  // it keeps the provenance (a ptrtoint, an insertvalue) or only compares
  // the address.
  void collectOtherUse(const llvm::Use& U) {
    const llvm::Value* V = U.get();
    if (!V->getType()->isPointerTy())
      return;
    Origin O = originOf(V);
    if (!O.anyParam())
      return;
    llvm::UseCaptureInfo CI = llvm::DetermineUseCaptureKind(U, V);
    if (llvm::capturesAnyProvenance(CI.UseCC) ||
        llvm::capturesAnyProvenance(CI.ResultCC))
      capture(O.params());
  }

  void collect() {
    for (const llvm::Instruction& I : llvm::instructions(F)) {
      if (ignored(I))
        continue;
      if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
        access(ModRefInfo::Ref, originOf(LI->getPointerOperand()), false);
      } else if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
        Origin Dst = originOf(SI->getPointerOperand());
        access(ModRefInfo::Mod, Dst, false);
        captureByCopy(carried(SI->getValueOperand()), Dst);
        captureScalar(SI->getValueOperand(), &Dst);
      } else if (const auto* MT = llvm::dyn_cast<llvm::MemTransferInst>(&I)) {
        Origin Src = originOf(MT->getRawSource());
        Origin Dst = originOf(MT->getRawDest());
        access(ModRefInfo::Ref, Src, false);
        access(ModRefInfo::Mod, Dst, false);
        captureByCopy(loadedFrom(Src), Dst);
      } else if (const auto* MS = llvm::dyn_cast<llvm::MemSetInst>(&I)) {
        access(ModRefInfo::Mod, originOf(MS->getRawDest()), false);
      } else if (const auto* RMW = llvm::dyn_cast<llvm::AtomicRMWInst>(&I)) {
        Origin Dst = originOf(RMW->getPointerOperand());
        access(ModRefInfo::ModRef, Dst, false);
        captureByCopy(carried(RMW->getValOperand()), Dst);
        captureScalar(RMW->getValOperand(), &Dst);
      } else if (const auto* CX = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&I)) {
        Origin Dst = originOf(CX->getPointerOperand());
        access(ModRefInfo::ModRef, Dst, false);
        captureByCopy(carried(CX->getNewValOperand()), Dst);
        captureScalar(CX->getNewValOperand(), &Dst);
      } else if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
        collectCall(*CB);
      } else if (const auto* RI = llvm::dyn_cast<llvm::ReturnInst>(&I)) {
        collectReturn(RI->getReturnValue());
        if (RI->getReturnValue())
          captureScalar(RI->getReturnValue());
      } else if (llvm::isa<llvm::VAArgInst>(I)) {
        R.Other = ModRefInfo::ModRef;
      } else if (arithmetic(I) || llvm::isa<llvm::ICmpInst>(I) ||
                 llvm::isa<llvm::PtrToIntInst>(I) ||
                 llvm::isa<llvm::GetElementPtrInst>(I)) {
        // The origins and the taint follow them; a comparison keeps nothing.
      } else {
        for (const llvm::Use& U : I.operands()) {
          collectOtherUse(U);
          captureScalar(U.get());
        }
      }
    }
    // An object that escaped, or that another parameter holds, exposes
    // every parameter it holds a pointer to (condition 3).
    for (const auto& [A, Held] : Holds) {
      Owner W = Owners[A];
      for (unsigned I : Held.set_bits())
        if (W.K == Owner::Escaped || (W.K == Owner::Param && W.P != I))
          R.Params[I].Captured = true;
    }
  }
};

Reach ReachAnalysis::analyze(const llvm::Function& F) {
  return FunctionReach(*this, F).run();
}

void PrepareForAnalysis(llvm::Module& M, bool Trusted) {
  LocalizeThrows(M);
  LocalizeTerminate(M);
  llvm::LLVMContext& C = M.getContext();
  for (llvm::Function& F : M) {
    if (F.isDeclaration() && IsRuntimeSupport(F.getName())) {
      F.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
      // The runtime hands out fresh exception storage and keeps no pointer
      // it is given to free or to handle.
      if (F.getName() == "__cxa_allocate_exception" ||
          F.getName() == "__cxa_begin_catch")
        F.setReturnDoesNotAlias();
      bool Sink = IsPointerSink(F.getName());
      bool ExceptionSink = F.getName() == "__cxa_begin_catch" ||
                           F.getName() == "__clang_call_terminate";
      for (unsigned I = 0; Sink && I < F.arg_size(); ++I) {
        if (!F.getArg(I)->getType()->isPointerTy())
          continue;
        F.addParamAttr(I, llvm::Attribute::getWithCaptureInfo(
                              C, llvm::CaptureInfo::none()));
        if (ExceptionSink)
          F.addParamAttr(I, llvm::Attribute::ReadNone);
      }
    } else if (F.isDeclaration() && !F.isIntrinsic()) {
      auto It = Summaries().find(F.getName());
      if (It != Summaries().end())
        ApplySummary(F, Trusted ? It->second.Trusted : It->second.Plain);
    }
    // At -O0 clang marks every function optnone and noinline; the analysis
    // must see through them.
    if (F.hasOptNone()) {
      F.removeFnAttr(llvm::Attribute::OptimizeNone);
      F.removeFnAttr(llvm::Attribute::NoInline);
    }
  }
}

void Optimize(llvm::Module& M) {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2).run(M, MAM);
}

// A call is opaque when it has no body in the module and nothing bounds its
// effects on memory that the caller does not pass to it.
std::optional<OpaqueRef> OpaqueCallee(const llvm::CallBase& CB) {
  if (CB.getMemoryEffects().getModRef(llvm::IRMemLocation::Other) ==
      ModRefInfo::NoModRef)
    return std::nullopt;
  if (CB.isInlineAsm())
    return OpaqueRef{Cpp::OpaqueKind::InlineAsm, {}};
  const llvm::Function* Callee = CB.getCalledFunction();
  if (!Callee)
    return OpaqueRef{Cpp::OpaqueKind::Indirect, {}};
  // A loaded summary states the effects, so the call is not opaque.
  if (!Callee->isDeclaration() || Callee->isIntrinsic() ||
      Summaries().count(Callee->getName()))
    return std::nullopt;
  return OpaqueRef{Cpp::OpaqueKind::Declaration, Callee->getName().str()};
}

// Reached for every defined function of \p M, through the functions it
// calls: a fixpoint over the call graph, so cycles need no special case.
llvm::DenseMap<const llvm::Function*, Reached>
OpaqueClosure(const llvm::Module& M) {
  llvm::DenseMap<const llvm::Function*, Reached> Out;
  llvm::DenseMap<const llvm::Function*,
                 llvm::SmallVector<const llvm::Function*, 8>>
      Callees;
  for (const llvm::Function& F : M) {
    if (F.isDeclaration())
      continue;
    Reached& R = Out[&F];
    auto& Cs = Callees[&F];
    for (const llvm::Instruction& I : llvm::instructions(F)) {
      const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!CB)
        continue;
      if (const llvm::Function* Callee = CB->getCalledFunction()) {
        if (!Callee->isDeclaration()) {
          if (!llvm::is_contained(Cs, Callee))
            Cs.push_back(Callee);
          continue;
        }
        auto It = Summaries().find(Callee->getName());
        if (It != Summaries().end()) {
          for (const OpaqueRef& Ref : It->second.Opaque.Opaque)
            AddUnique(R.Opaque, Ref);
          for (const OpaqueRef& Ref : It->second.Opaque.Unbounded)
            AddUnique(R.Unbounded, Ref);
          continue;
        }
      }
      if (std::optional<OpaqueRef> Ref = OpaqueCallee(*CB))
        AddUnique(R.Opaque, *Ref);
    }
  }
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto& [F, Cs] : Callees) {
      Reached& R = Out[F];
      for (const llvm::Function* Callee : Cs) {
        const Reached& CR = Out[Callee];
        for (const OpaqueRef& Ref : CR.Opaque)
          if (!llvm::is_contained(R.Opaque, Ref)) {
            R.Opaque.push_back(Ref);
            Changed = true;
          }
        for (const OpaqueRef& Ref : CR.Unbounded)
          if (!llvm::is_contained(R.Unbounded, Ref)) {
            R.Unbounded.push_back(Ref);
            Changed = true;
          }
      }
    }
  }
  // The fixpoint visits functions in pointer order; sorting keeps the tables
  // reproducible.
  for (auto& Entry : Out) {
    llvm::sort(Entry.second.Opaque);
    llvm::sort(Entry.second.Unbounded);
  }
  return Out;
}

namespace id = llvm::itanium_demangle;
namespace ms = llvm::ms_demangle;

// The node allocator of one Itanium demangler parse.
class DemangleAllocator {
  llvm::BumpPtrAllocator Alloc;

public:
  void reset() { Alloc.Reset(); }

  template <typename T, typename... Args> T* makeNode(Args&&... A) {
    return new (Alloc.Allocate(sizeof(T), alignof(T)))
        T(std::forward<Args>(A)...);
  }

  void* allocateNodeArray(size_t N) {
    return Alloc.Allocate(sizeof(id::Node*) * N, alignof(id::Node*));
  }
};

// The demangler spells a builtin type as a name node, like a class.
bool IsBuiltinName(std::string_view Name) {
  static constexpr llvm::StringLiteral Scalars[] = {
      "bool",    "char",           "signed char", "unsigned char",
      "wchar_t", "char8_t",        "char16_t",    "char32_t",
      "short",   "unsigned short", "int",         "unsigned int",
      "long",    "unsigned long",  "long long",   "unsigned long long",
      "float",   "double",
  };
  return llvm::is_contained(Scalars, llvm::StringRef(Name));
}

const id::Node* StripQualifiers(const id::Node* N, bool& Const) {
  while (N && N->getKind() == id::Node::KQualType) {
    const auto* Q = static_cast<const id::QualType*>(N);
    Const |= (Q->getQuals() & id::QualConst) != 0;
    N = Q->getChild();
  }
  return N;
}

Cpp::ParamKind ClassifyItanium(const id::Node* N) {
  bool TopConst = false; // a by-value parameter's own const does not matter
  N = StripQualifiers(N, TopConst);
  if (!N)
    return Cpp::ParamKind::Unknown;
  switch (N->getKind()) {
  case id::Node::KReferenceType:
  case id::Node::KPointerType: {
    bool Reference = N->getKind() == id::Node::KReferenceType;
    const id::Node* Pointee = nullptr;
    if (Reference)
      static_cast<const id::ReferenceType*>(N)->match(
          [&](const id::Node* P, id::ReferenceKind) { Pointee = P; });
    else
      Pointee = static_cast<const id::PointerType*>(N)->getPointee();
    bool Const = false;
    const id::Node* Target = StripQualifiers(Pointee, Const);
    if (!Target || Target->getKind() == id::Node::KFunctionType)
      return Cpp::ParamKind::Unknown;
    if (Reference)
      return Const ? Cpp::ParamKind::ConstReference
                   : Cpp::ParamKind::MutableReference;
    return Const ? Cpp::ParamKind::ConstPointer
                 : Cpp::ParamKind::MutablePointer;
  }
  case id::Node::KNameType:
    return IsBuiltinName(N->getBaseName()) ? Cpp::ParamKind::Scalar
                                           : Cpp::ParamKind::Object;
  case id::Node::KPointerToMemberType:
  case id::Node::KForwardTemplateReference:
  case id::Node::KParameterPackExpansion:
  case id::Node::KVendorExtQualType:
    return Cpp::ParamKind::Unknown;
  default:
    return Cpp::ParamKind::Object;
  }
}

Cpp::FunctionSignature ReadItaniumSignature(llvm::StringRef Mangled) {
  Cpp::FunctionSignature S;
  id::ManglingParser<DemangleAllocator> Parser(Mangled.begin(), Mangled.end());
  const id::Node* Root = Parser.parse();
  if (!Root || Root->getKind() != id::Node::KFunctionEncoding)
    return S;
  const auto* FE = static_cast<const id::FunctionEncoding*>(Root);
  S.Known = true;
  if (const id::Node* Name = FE->getName())
    S.BaseName = std::string(Name->getBaseName());
  S.ConstMethod = (FE->getCVQuals() & id::QualConst) != 0;
  for (const id::Node* Param : FE->getParams()) {
    if (Param->getKind() == id::Node::KNameType &&
        Param->getBaseName() == "...") {
      S.Variadic = true;
      continue;
    }
    S.Params.push_back(ClassifyItanium(Param));
  }
  return S;
}

Cpp::ParamKind ClassifyMicrosoft(const ms::Node* N) {
  if (!N)
    return Cpp::ParamKind::Unknown;
  switch (N->kind()) {
  case ms::NodeKind::PrimitiveType:
    return Cpp::ParamKind::Scalar;
  case ms::NodeKind::TagType:
    return Cpp::ParamKind::Object;
  case ms::NodeKind::PointerType: {
    const auto* P = static_cast<const ms::PointerTypeNode*>(N);
    if (P->ClassParent || !P->Pointee ||
        P->Pointee->kind() == ms::NodeKind::FunctionSignature)
      return Cpp::ParamKind::Unknown;
    bool Const = (P->Pointee->Quals & ms::Q_Const) != 0;
    if (P->Affinity == ms::PointerAffinity::Pointer)
      return Const ? Cpp::ParamKind::ConstPointer
                   : Cpp::ParamKind::MutablePointer;
    return Const ? Cpp::ParamKind::ConstReference
                 : Cpp::ParamKind::MutableReference;
  }
  default:
    return Cpp::ParamKind::Unknown;
  }
}

Cpp::FunctionSignature ReadMicrosoftSignature(llvm::StringRef Mangled) {
  Cpp::FunctionSignature S;
  ms::Demangler D;
  std::string_view View(Mangled.data(), Mangled.size());
  ms::SymbolNode* Symbol = D.parse(View);
  if (D.Error || !Symbol || Symbol->kind() != ms::NodeKind::FunctionSymbol)
    return S;
  const ms::FunctionSignatureNode* Sig =
      static_cast<ms::FunctionSymbolNode*>(Symbol)->Signature;
  if (!Sig)
    return S;
  S.Known = true;
  if (Symbol->Name)
    if (ms::IdentifierNode* Id = Symbol->Name->getUnqualifiedIdentifier())
      if (Id->kind() == ms::NodeKind::NamedIdentifier)
        S.BaseName =
            std::string(static_cast<ms::NamedIdentifierNode*>(Id)->Name);
  S.ConstMethod = (Sig->Quals & ms::Q_Const) != 0;
  S.Variadic = Sig->IsVariadic;
  if (Sig->Params)
    for (size_t I = 0; I < Sig->Params->Count; ++I)
      S.Params.push_back(ClassifyMicrosoft(Sig->Params->Nodes[I]));
  return S;
}

// The signature that the mangled name of a function shows; not Known for a
// C function, whose name carries no parameters.
Cpp::FunctionSignature ReadSignature(llvm::StringRef Mangled) {
  if (Mangled.starts_with("_Z"))
    return ReadItaniumSignature(Mangled);
  if (Mangled.starts_with("?"))
    return ReadMicrosoftSignature(Mangled);
  return {};
}

bool IsConstKind(Cpp::ParamKind K) {
  return K == Cpp::ParamKind::ConstReference ||
         K == Cpp::ParamKind::ConstPointer;
}

// Marks the pointer parameters of a declaration that its signature makes
// read-only. False when the C++ parameters do not map one to one to the IR
// parameters, so the signature cannot bound them.
bool BoundBySignature(llvm::Function& F) {
  Cpp::FunctionSignature S = ReadSignature(F.getName());
  if (!S.Known || S.Variadic)
    return false;
  llvm::SmallVector<Cpp::ParamKind, 8> Kinds(S.Params.begin(), S.Params.end());
  // A class or enum by value lowers to registers or memory that the IR
  // parameters do not show one to one.
  for (Cpp::ParamKind K : Kinds)
    if (K == Cpp::ParamKind::Unknown || K == Cpp::ParamKind::Object)
      return false;
  bool ConstMethod = S.ConstMethod;
  llvm::SmallVector<unsigned, 8> IRParams;
  for (unsigned I = 0; I < F.arg_size(); ++I)
    if (!F.hasParamAttribute(I, llvm::Attribute::StructRet))
      IRParams.push_back(I);
  bool HasThis = IRParams.size() == Kinds.size() + 1;
  if (!HasThis && (IRParams.size() != Kinds.size() || ConstMethod))
    return false;
  if (HasThis)
    Kinds.insert(Kinds.begin(), ConstMethod ? Cpp::ParamKind::ConstPointer
                                            : Cpp::ParamKind::MutablePointer);
  for (unsigned K = 0; K < Kinds.size(); ++K)
    if (F.getArg(IRParams[K])->getType()->isPointerTy() !=
        (Kinds[K] != Cpp::ParamKind::Scalar))
      return false;
  for (unsigned K = 0; K < Kinds.size(); ++K) {
    unsigned I = IRParams[K];
    if (IsConstKind(Kinds[K]) &&
        !F.hasParamAttribute(I, llvm::Attribute::WriteOnly) &&
        !F.hasParamAttribute(I, llvm::Attribute::ReadNone))
      F.addParamAttr(I, llvm::Attribute::ReadOnly);
  }
  return true;
}

// Every call with no body or summary is assumed to access only memory
// through its arguments, to keep no copy of them except in a pointer
// result, and not to write through a parameter that its signature makes
// const. Returns the mangled names that the signature cannot bound.
llvm::StringSet<> TrustOpaqueCalls(llvm::Module& M) {
  const llvm::MemoryEffects ArgOnly =
      llvm::MemoryEffects::inaccessibleOrArgMemOnly();
  auto Bound = [&](auto& Call) {
    Call.setMemoryEffects(Call.getMemoryEffects() & ArgOnly);
    llvm::FunctionType* FT = Call.getFunctionType();
    llvm::CaptureInfo CI = FT->getReturnType()->isPointerTy()
                               ? llvm::CaptureInfo::retOnly()
                               : llvm::CaptureInfo::none();
    for (unsigned I = 0; I < FT->getNumParams(); ++I)
      if (FT->getParamType(I)->isPointerTy())
        Call.addParamAttr(
            I, llvm::Attribute::getWithCaptureInfo(M.getContext(), CI));
  };
  llvm::StringSet<> Unbounded;
  for (llvm::Function& F : M) {
    if (F.isDeclaration()) {
      if (!F.isIntrinsic() && !IsRuntimeSupport(F.getName()) &&
          !Summaries().count(F.getName())) {
        Bound(F);
        if (!BoundBySignature(F))
          Unbounded.insert(F.getName());
      }
      continue;
    }
    for (llvm::Instruction& I : llvm::instructions(F)) {
      auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
      if (CB && (CB->isIndirectCall() || CB->isInlineAsm()))
        Bound(*CB);
    }
  }
  return Unbounded;
}

void CollectCallees(const llvm::Function& F, Cpp::MemoryEffects& Out) {
  llvm::SmallPtrSet<const llvm::Function*, 8> Seen;
  for (const llvm::Instruction& I : llvm::instructions(F)) {
    const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
    const llvm::Function* Callee = CB ? CB->getCalledFunction() : nullptr;
    if (!Callee || Callee->isIntrinsic() || !Seen.insert(Callee).second)
      continue;
    Out.Callees.push_back({Callee->getName().str(),
                           llvm::demangle(Callee->getName()),
                           ReadSignature(Callee->getName())});
  }
}

// LLVM's own per-parameter bound.
Cpp::ModRef ParamBound(const llvm::Argument& A, Cpp::ModRef ArgMem) {
  if (!A.getType()->isPointerTy())
    return Cpp::ModRef::None;
  unsigned MR = static_cast<unsigned>(ArgMem);
  if (A.hasAttribute(llvm::Attribute::ReadNone))
    MR = 0;
  else if (A.onlyReadsMemory())
    MR &= static_cast<unsigned>(Cpp::ModRef::Read);
  else if (A.hasAttribute(llvm::Attribute::WriteOnly))
    MR &= static_cast<unsigned>(Cpp::ModRef::Write);
  return static_cast<Cpp::ModRef>(MR);
}

// The fields of one analysis variant (untrusted or trusted).
struct Variant {
  Cpp::ModRef Other = Cpp::ModRef::ReadWrite;
  std::vector<Cpp::ModRef> Params, Direct, Reachable;
  std::vector<bool> Captured;
};

Variant ReadVariant(const llvm::Function& F, ReachAnalysis& RA) {
  Variant V;
  const Reach& R = RA.of(F);
  V.Other = ToModRef(R.Other);
  Cpp::ModRef ArgMem =
      ToModRef(F.getMemoryEffects().getModRef(llvm::IRMemLocation::ArgMem));
  for (const llvm::Argument& A : F.args()) {
    V.Params.push_back(ParamBound(A, ArgMem));
    const ParamReach& P = R.Params[A.getArgNo()];
    bool Ptr = A.getType()->isPointerTy();
    V.Direct.push_back(Ptr ? ToModRef(P.Direct) : Cpp::ModRef::None);
    V.Reachable.push_back(Ptr ? ToModRef(P.Reachable) : Cpp::ModRef::None);
    V.Captured.push_back(Ptr && P.Captured);
  }
  return V;
}

void ReadMemoryEffects(const llvm::Function& F, ReachAnalysis& RA,
                       const Reached& Opaque, Cpp::MemoryEffects& Out) {
  if (F.isDeclaration())
    return;
  Out.Valid = true;
  // Memory that the program cannot name, errno and target state cannot hold
  // a value that the caller reads.
  Out.ArgMem =
      ToModRef(F.getMemoryEffects().getModRef(llvm::IRMemLocation::ArgMem));
  Variant V = ReadVariant(F, RA);
  Out.Other = V.Other;
  Out.Params = V.Params;
  Out.ParamsDirect = V.Direct;
  Out.ParamsReachable = V.Reachable;
  Out.Captured = V.Captured;
  for (const OpaqueRef& Ref : Opaque.Opaque) {
    Cpp::OpaqueCall Call;
    Call.Kind = Ref.Kind;
    if (Ref.Kind == Cpp::OpaqueKind::Declaration) {
      Call.Mangled = Ref.Mangled;
      Call.Demangled = llvm::demangle(Ref.Mangled);
      Call.Signature = ReadSignature(Ref.Mangled);
      Call.Bounded = !llvm::is_contained(Opaque.Unbounded, Ref);
    }
    Out.Opaque.push_back(std::move(Call));
  }
}

char CaptureChar(const llvm::Argument& A) {
  if (!A.getType()->isPointerTy())
    return '.';
  llvm::CaptureInfo CI = A.getAttributes().getCaptureInfo();
  if (llvm::capturesNothing(CI))
    return 'c';
  if (!llvm::capturesAnyProvenance(CI.getOtherComponents()) &&
      !llvm::capturesAnyProvenance(CI.getRetComponents()))
    return 'a';
  return '.';
}

// The seven table fields of one variant of a function.
std::string VariantFields(const llvm::Function& F, ReachAnalysis& RA) {
  const Reach& R = RA.of(F);
  std::string Params, Direct, Reachable, Captured, Ret;
  for (const llvm::Argument& A : F.args()) {
    char Access = '-';
    if (A.hasAttribute(llvm::Attribute::ReadNone))
      Access = 'n';
    else if (A.hasAttribute(llvm::Attribute::ReadOnly))
      Access = 'r';
    else if (A.hasAttribute(llvm::Attribute::WriteOnly))
      Access = 'w';
    Params += Access;
    Params += CaptureChar(A);
    const ParamReach& P = R.Params[A.getArgNo()];
    bool Ptr = A.getType()->isPointerTy();
    Direct += Ptr ? AccessChar(P.Direct) : 'n';
    Reachable += Ptr ? AccessChar(P.Reachable) : 'n';
    Captured += Ptr && P.Captured ? 'c' : '.';
    unsigned I = A.getArgNo();
    Ret += R.RetDirect.test(I) ? (R.RetLoaded.test(I) ? 'b' : 'd')
                               : (R.RetLoaded.test(I) ? 'l' : '.');
  }
  char RetKind =
      R.RetFresh ? (R.RetUnknown ? 'b' : 'f') : (R.RetUnknown ? 'u' : '.');
  return llvm::utohexstr(F.getMemoryEffects().toIntValue()) + "\t" + Params +
         "\t" + Direct + "\t" + Reachable + "\t" + Captured + "\t" +
         AccessChar(R.Other) + "\t" + RetKind + Ret;
}

bool ParseVariant(llvm::ArrayRef<llvm::StringRef> Fields, SummaryVariant& V) {
  if (Fields.size() != 7 || Fields[0].getAsInteger(16, V.Effects))
    return false;
  llvm::StringRef Params = Fields[1], Direct = Fields[2], Reachable = Fields[3],
                  Captured = Fields[4], Other = Fields[5], Ret = Fields[6];
  const size_t N = Direct.size();
  if (Params.size() != 2 * N || Reachable.size() != N || Captured.size() != N ||
      Other.size() != 1 || Ret.size() != N + 1)
    return false;
  V.R = Reach(N);
  for (size_t I = 0; I < N; ++I) {
    V.Params.push_back({Params[2 * I], Params[2 * I + 1]});
    ParamReach& P = V.R.Params[I];
    if (!ParseAccess(Direct[I], P.Direct) ||
        !ParseAccess(Reachable[I], P.Reachable))
      return false;
    P.Captured = Captured[I] == 'c';
    char C = Ret[I + 1];
    if (C == 'd' || C == 'b')
      V.R.RetDirect.set(I);
    if (C == 'l' || C == 'b')
      V.R.RetLoaded.set(I);
  }
  V.R.RetFresh = Ret[0] == 'f' || Ret[0] == 'b';
  V.R.RetUnknown = Ret[0] == 'u' || Ret[0] == 'b';
  return ParseAccess(Other[0], V.R.Other);
}

// Whether the JIT sees the body of \p F from headers alone: an inline
// function or an implicit instantiation. An explicit instantiation
// definition (weak_odr) may live in a .cpp behind an extern template, so
// it stays in the table.
bool VisibleToTheJIT(const llvm::Function& F) {
  return F.hasLinkOnceODRLinkage() || F.hasAvailableExternallyLinkage();
}

} // namespace

void AnalyzeMemoryEffects(const llvm::Module& M,
                          llvm::ArrayRef<std::string> Names,
                          std::vector<Cpp::MemoryEffects>& Out) {
  Out.assign(Names.size(), Cpp::MemoryEffects{});
  // Optimization inlines the callees and removes the calls.
  for (size_t I = 0; I < Names.size(); ++I)
    if (const llvm::Function* F = M.getFunction(Names[I]))
      CollectCallees(*F, Out[I]);

  std::unique_ptr<llvm::Module> Copy = llvm::CloneModule(M);
  PrepareForAnalysis(*Copy, /*Trusted=*/false);
  Optimize(*Copy);
  if (std::getenv("CPPINTEROP_MEMORY_EFFECTS_DUMP"))
    Copy->print(llvm::errs(), nullptr);

  bool AnyOpaque = false;
  {
    ReachAnalysis RA(/*Trusted=*/false);
    llvm::DenseMap<const llvm::Function*, Reached> Opaque =
        OpaqueClosure(*Copy);
    for (size_t I = 0; I < Names.size(); ++I) {
      if (const llvm::Function* F = Copy->getFunction(Names[I]))
        ReadMemoryEffects(*F, RA, Opaque[F], Out[I]);
      Out[I].TrustedOther = Out[I].Other;
      Out[I].TrustedCaptured = Out[I].Captured;
      Out[I].TrustedParams = Out[I].Params;
      Out[I].TrustedParamsDirect = Out[I].ParamsDirect;
      Out[I].TrustedParamsReachable = Out[I].ParamsReachable;
      AnyOpaque |= !Out[I].Opaque.empty();
    }
  }
  if (!AnyOpaque)
    return;

  Copy = llvm::CloneModule(M);
  PrepareForAnalysis(*Copy, /*Trusted=*/true);
  llvm::StringSet<> Unbounded = TrustOpaqueCalls(*Copy);
  Optimize(*Copy);
  ReachAnalysis RA(/*Trusted=*/true);
  for (size_t I = 0; I < Names.size(); ++I) {
    const llvm::Function* F = Copy->getFunction(Names[I]);
    if (Out[I].Opaque.empty() || !F || F->isDeclaration())
      continue;
    Variant V = ReadVariant(*F, RA);
    Out[I].TrustedOther = V.Other;
    Out[I].TrustedCaptured = V.Captured;
    Out[I].TrustedParams = V.Params;
    Out[I].TrustedParamsDirect = V.Direct;
    Out[I].TrustedParamsReachable = V.Reachable;
    for (Cpp::OpaqueCall& Call : Out[I].Opaque)
      if (Unbounded.contains(Call.Mangled))
        Call.Bounded = false;
  }
}

int LoadMemoryEffectsSummaries(llvm::StringRef Text) {
  llvm::SmallVector<llvm::StringRef, 64> Lines;
  Text.split(Lines, '\n', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  if (Lines.empty() || !Lines[0].starts_with(SummaryHeader) ||
      Lines[0].drop_front(SummaryHeader.size()).trim() !=
          std::to_string(LLVM_VERSION_MAJOR))
    return -1;
  llvm::StringMap<FunctionSummary> Read;
  for (llvm::StringRef Line : llvm::ArrayRef(Lines).drop_front()) {
    if (Line.starts_with("#"))
      continue;
    llvm::SmallVector<llvm::StringRef, 20> Fields;
    Line.split(Fields, '\t');
    FunctionSummary S;
    // name, 7 plain fields, flags; then 7 trusted fields and the opaque
    // names when the summary is incomplete.
    if (Fields.size() < 9 ||
        !ParseVariant(llvm::ArrayRef(Fields).slice(1, 7), S.Plain))
      return -1;
    if (Fields[8] == ".") {
      if (Fields.size() != 9)
        return -1;
      S.Trusted = S.Plain;
    } else if (Fields[8] == "i") {
      if (Fields.size() < 16 ||
          !ParseVariant(llvm::ArrayRef(Fields).slice(9, 7), S.Trusted))
        return -1;
      S.Incomplete = true;
      for (llvm::StringRef Token : llvm::ArrayRef(Fields).drop_front(16)) {
        bool Unbounded = false;
        OpaqueRef Ref = ParseTableToken(Token, Unbounded);
        if (Ref.Kind == Cpp::OpaqueKind::Declaration && Ref.Mangled.empty())
          return -1;
        AddUnique(S.Opaque.Opaque, Ref);
        if (Unbounded)
          AddUnique(S.Opaque.Unbounded, Ref);
      }
    } else
      return -1;
    Read[Fields[0]] = std::move(S);
  }
  for (auto& Entry : Read)
    Summaries()[Entry.getKey()] = std::move(Entry.getValue());
  return static_cast<int>(Read.size());
}

std::string SummarizeMemoryEffects(llvm::Module& M) {
  std::unique_ptr<llvm::Module> TrustedCopy = llvm::CloneModule(M);
  PrepareForAnalysis(M, /*Trusted=*/false);
  Optimize(M);
  ReachAnalysis RA(/*Trusted=*/false);

  llvm::DenseMap<const llvm::Function*, Reached> Reaches = OpaqueClosure(M);
  bool AnyIncomplete = false;
  for (const llvm::Function& F : M) {
    if (F.isDeclaration() || F.hasLocalLinkage() || !F.hasName() ||
        VisibleToTheJIT(F))
      Reaches.erase(&F);
    else
      AnyIncomplete |= !Reaches[&F].Opaque.empty();
  }

  std::unique_ptr<ReachAnalysis> RAT;
  llvm::StringSet<> Unbounded;
  if (AnyIncomplete) {
    PrepareForAnalysis(*TrustedCopy, /*Trusted=*/true);
    Unbounded = TrustOpaqueCalls(*TrustedCopy);
    Optimize(*TrustedCopy);
    RAT = std::make_unique<ReachAnalysis>(/*Trusted=*/true);
  }

  std::string Out = (SummaryHeader + std::to_string(LLVM_VERSION_MAJOR)).str();
  Out += "\n";
  for (const llvm::Function& F : M) {
    auto It = Reaches.find(&F);
    if (It == Reaches.end())
      continue;
    Out += F.getName().str() + "\t" + VariantFields(F, RA) + "\t";
    const Reached& R = It->second;
    if (R.Opaque.empty()) {
      Out += ".\n";
      continue;
    }
    const llvm::Function* TF = TrustedCopy->getFunction(F.getName());
    // The trusted variant needs the body; without one nothing is bounded.
    Out += "i\t";
    Out += TF && !TF->isDeclaration() ? VariantFields(*TF, *RAT)
                                      : VariantFields(F, RA);
    for (const OpaqueRef& Ref : R.Opaque) {
      bool Unb = Ref.Kind != Cpp::OpaqueKind::Declaration ||
                 Unbounded.contains(Ref.Mangled) ||
                 llvm::is_contained(R.Unbounded, Ref);
      Out += "\t" + TableToken(Ref, Unb);
    }
    Out += "\n";
  }
  return Out;
}

} // namespace CppInternal
