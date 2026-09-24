//===--- MemoryEffects.cpp - Memory effects from optimized IR ---*- C++ -*-===//
//
// Part of the compiler-research project, under the Apache License v2.0 with
// LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MemoryEffects.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace CppInternal {

namespace {

Cpp::ModRef ToModRef(llvm::ModRefInfo MR) {
  switch (MR) {
  case llvm::ModRefInfo::NoModRef:
    return Cpp::ModRef::None;
  case llvm::ModRefInfo::Ref:
    return Cpp::ModRef::Read;
  case llvm::ModRefInfo::Mod:
    return Cpp::ModRef::Write;
  case llvm::ModRefInfo::ModRef:
    return Cpp::ModRef::ReadWrite;
  }
  return Cpp::ModRef::ReadWrite;
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

// One parameter of a summary: how the function accesses memory through it,
// and whether it keeps a copy of the pointer.
struct ParamSummary {
  char Access; // 'n' readnone, 'r' readonly, 'w' writeonly, '-' unknown
  bool NoCapture;
};

struct FunctionSummary {
  uint32_t Effects; // llvm::MemoryEffects::toIntValue()
  std::vector<ParamSummary> Params;
};

llvm::StringMap<FunctionSummary>& Summaries() {
  static llvm::StringMap<FunctionSummary> S;
  return S;
}

constexpr llvm::StringLiteral SummaryHeader =
    "# cppinterop-memory-effects v1 llvm ";

void ApplySummary(llvm::Function& F, const FunctionSummary& S) {
  F.setMemoryEffects(llvm::MemoryEffects::createFromIntValue(S.Effects));
  for (unsigned I = 0; I < S.Params.size() && I < F.arg_size(); ++I) {
    const ParamSummary& P = S.Params[I];
    if (P.Access == 'n')
      F.addParamAttr(I, llvm::Attribute::ReadNone);
    else if (P.Access == 'r')
      F.addParamAttr(I, llvm::Attribute::ReadOnly);
    else if (P.Access == 'w')
      F.addParamAttr(I, llvm::Attribute::WriteOnly);
    if (P.NoCapture)
      F.addParamAttr(I, llvm::Attribute::getWithCaptureInfo(
                            F.getContext(), llvm::CaptureInfo::none()));
  }
}

void PrepareForAnalysis(llvm::Module& M) {
  LocalizeThrows(M);
  for (llvm::Function& F : M) {
    if (F.isDeclaration() && IsRuntimeSupport(F.getName()))
      F.setMemoryEffects(llvm::MemoryEffects::inaccessibleOrArgMemOnly());
    else if (F.isDeclaration() && !F.isIntrinsic()) {
      auto It = Summaries().find(F.getName());
      if (It != Summaries().end())
        ApplySummary(F, It->second);
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
std::string OpaqueCallee(const llvm::CallBase& CB) {
  if (CB.getMemoryEffects().getModRef(llvm::IRMemLocation::Other) ==
      llvm::ModRefInfo::NoModRef)
    return {};
  if (CB.isInlineAsm())
    return "<asm>";
  const llvm::Function* Callee = CB.getCalledFunction();
  if (!Callee)
    return "<indirect>";
  // A loaded summary states the effects, so the call is not opaque.
  if (!Callee->isDeclaration() || Callee->isIntrinsic() ||
      Summaries().count(Callee->getName()))
    return {};
  return llvm::demangle(Callee->getName());
}

void CollectOpaque(const llvm::Function& Root, Cpp::MemoryEffects& Out) {
  llvm::SmallPtrSet<const llvm::Function*, 16> Seen;
  llvm::SmallVector<const llvm::Function*, 16> Work{&Root};
  while (!Work.empty()) {
    const llvm::Function* F = Work.pop_back_val();
    if (!Seen.insert(F).second || F->isDeclaration())
      continue;
    for (const llvm::Instruction& I : llvm::instructions(*F)) {
      const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
      if (!CB)
        continue;
      if (const llvm::Function* Callee = CB->getCalledFunction())
        Work.push_back(Callee);
      std::string Name = OpaqueCallee(*CB);
      if (!Name.empty() && std::find(Out.m_Opaque.begin(), Out.m_Opaque.end(),
                                     Name) == Out.m_Opaque.end())
        Out.m_Opaque.push_back(std::move(Name));
    }
  }
}

// One parameter of a demangled signature: how it lowers to the IR.
enum class ParamKind { Pointer, ConstPointer, Scalar, Unknown };

ParamKind ClassifyParam(llvm::StringRef P) {
  P = P.trim();
  P.consume_back(" volatile");
  if (P.contains("(&") || P.contains("(*") || P.contains("::*"))
    return ParamKind::Unknown;
  for (llvm::StringRef Suffix : {"&&", "&", "*"})
    if (P.consume_back(Suffix)) {
      P = P.rtrim();
      P.consume_back(" volatile");
      return P.ends_with("const") ? ParamKind::ConstPointer
                                  : ParamKind::Pointer;
    }
  static constexpr llvm::StringLiteral Scalars[] = {
      "bool",    "char",           "signed char", "unsigned char",
      "wchar_t", "char8_t",        "char16_t",    "char32_t",
      "short",   "unsigned short", "int",         "unsigned int",
      "long",    "unsigned long",  "long long",   "unsigned long long",
      "float",   "double",
  };
  P.consume_back(" const");
  if (std::find(std::begin(Scalars), std::end(Scalars), P) != std::end(Scalars))
    return ParamKind::Scalar;
  return ParamKind::Unknown;
}

// The parameters of a demangled function name and whether it is a const
// method; false when the name is not a function or is variadic.
bool ParseSignature(llvm::StringRef D, llvm::SmallVectorImpl<ParamKind>& Out,
                    bool& ConstMethod) {
  D = D.trim();
  ConstMethod = false;
  for (bool More = true; More;) {
    More = false;
    for (llvm::StringRef Q : {" const", " volatile", " &&", " &"})
      if (D.consume_back(Q)) {
        ConstMethod |= Q == " const";
        More = true;
      }
  }
  if (!D.ends_with(")"))
    return false;
  int Depth = 0;
  size_t Open = llvm::StringRef::npos;
  for (size_t I = D.size(); I-- > 0;) {
    char C = D[I];
    if (C == ')' || C == '>' || C == ']')
      ++Depth;
    else if ((C == '(' || C == '<' || C == '[') && --Depth == 0) {
      Open = I;
      break;
    }
  }
  if (Open == llvm::StringRef::npos || Open == 0)
    return false;
  llvm::StringRef Params = D.slice(Open + 1, D.size() - 1).trim();
  if (Params.contains("..."))
    return false;
  Depth = 0;
  size_t Start = 0;
  for (size_t I = 0; I <= Params.size(); ++I) {
    char C = I < Params.size() ? Params[I] : ',';
    if (C == '(' || C == '<' || C == '[')
      ++Depth;
    else if (C == ')' || C == '>' || C == ']')
      --Depth;
    else if (C == ',' && Depth == 0) {
      llvm::StringRef P = Params.slice(Start, I).trim();
      if (!P.empty())
        Out.push_back(ClassifyParam(P));
      Start = I + 1;
    }
  }
  return true;
}

// Marks the pointer parameters of a declaration that its signature makes
// read-only. False when the C++ parameters do not map one to one to the IR
// parameters, so the signature cannot bound them.
bool BoundBySignature(llvm::Function& F) {
  llvm::SmallVector<ParamKind, 8> Kinds;
  bool ConstMethod = false;
  if (!ParseSignature(llvm::demangle(F.getName()), Kinds, ConstMethod) ||
      llvm::is_contained(Kinds, ParamKind::Unknown))
    return false;
  llvm::SmallVector<unsigned, 8> IRParams;
  for (unsigned I = 0; I < F.arg_size(); ++I)
    if (!F.hasParamAttribute(I, llvm::Attribute::StructRet))
      IRParams.push_back(I);
  bool HasThis = IRParams.size() == Kinds.size() + 1;
  if (!HasThis && (IRParams.size() != Kinds.size() || ConstMethod))
    return false;
  if (HasThis)
    Kinds.insert(Kinds.begin(),
                 ConstMethod ? ParamKind::ConstPointer : ParamKind::Pointer);
  for (unsigned K = 0; K < Kinds.size(); ++K)
    if (F.getArg(IRParams[K])->getType()->isPointerTy() !=
        (Kinds[K] != ParamKind::Scalar))
      return false;
  for (unsigned K = 0; K < Kinds.size(); ++K) {
    unsigned I = IRParams[K];
    if (Kinds[K] == ParamKind::ConstPointer &&
        !F.hasParamAttribute(I, llvm::Attribute::WriteOnly) &&
        !F.hasParamAttribute(I, llvm::Attribute::ReadNone))
      F.addParamAttr(I, llvm::Attribute::ReadOnly);
  }
  return true;
}

// Every call with no body or summary is assumed to access only memory
// through its arguments, to keep no copy of them except in a pointer
// result, and not to write through a parameter that its signature makes
// const. Returns the demangled names that the signature cannot bound.
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
          Unbounded.insert(llvm::demangle(F.getName()));
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
    Out.m_Callees.push_back(
        {Callee->getName().str(), llvm::demangle(Callee->getName())});
  }
}

void ReadMemoryEffects(const llvm::Function& F, Cpp::MemoryEffects& Out) {
  if (F.isDeclaration())
    return;
  llvm::MemoryEffects ME = F.getMemoryEffects();
  Out.m_Valid = true;
  Out.m_ArgMem = ToModRef(ME.getModRef(llvm::IRMemLocation::ArgMem));
  // Memory that the program cannot name, errno and target state cannot hold
  // a value that the caller reads.
  Out.m_Other = ToModRef(ME.getModRef(llvm::IRMemLocation::Other));

  for (const llvm::Argument& A : F.args()) {
    if (!A.getType()->isPointerTy()) {
      Out.m_Params.push_back(Cpp::ModRef::None);
      Out.m_Captured.push_back(false);
      continue;
    }
    Cpp::ModRef MR = Out.m_ArgMem;
    if (A.hasAttribute(llvm::Attribute::ReadNone))
      MR = Cpp::ModRef::None;
    else if (A.onlyReadsMemory())
      MR = static_cast<Cpp::ModRef>(static_cast<unsigned>(MR) &
                                    static_cast<unsigned>(Cpp::ModRef::Read));
    else if (A.hasAttribute(llvm::Attribute::WriteOnly))
      MR = static_cast<Cpp::ModRef>(static_cast<unsigned>(MR) &
                                    static_cast<unsigned>(Cpp::ModRef::Write));
    Out.m_Params.push_back(MR);
    Out.m_Captured.push_back(
        !llvm::capturesNothing(A.getAttributes().getCaptureInfo()));
  }

  CollectOpaque(F, Out);
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
  PrepareForAnalysis(*Copy);
  Optimize(*Copy);
  if (std::getenv("CPPINTEROP_MEMORY_EFFECTS_DUMP"))
    Copy->print(llvm::errs(), nullptr);

  bool AnyOpaque = false;
  for (size_t I = 0; I < Names.size(); ++I) {
    if (const llvm::Function* F = Copy->getFunction(Names[I]))
      ReadMemoryEffects(*F, Out[I]);
    Out[I].m_TrustedOther = Out[I].m_Other;
    Out[I].m_TrustedCaptured = Out[I].m_Captured;
    Out[I].m_TrustedParams = Out[I].m_Params;
    AnyOpaque |= !Out[I].m_Opaque.empty();
  }
  if (!AnyOpaque)
    return;

  Copy = llvm::CloneModule(M);
  PrepareForAnalysis(*Copy);
  llvm::StringSet<> Unbounded = TrustOpaqueCalls(*Copy);
  Optimize(*Copy);
  for (size_t I = 0; I < Names.size(); ++I) {
    const llvm::Function* F = Copy->getFunction(Names[I]);
    if (Out[I].m_Opaque.empty() || !F || F->isDeclaration())
      continue;
    Cpp::MemoryEffects Trusted;
    ReadMemoryEffects(*F, Trusted);
    Out[I].m_TrustedOther = Trusted.m_Other;
    Out[I].m_TrustedCaptured = Trusted.m_Captured;
    Out[I].m_TrustedParams = Trusted.m_Params;
    for (const std::string& O : Out[I].m_Opaque)
      if (O == "<indirect>" || O == "<asm>" || Unbounded.contains(O))
        Out[I].m_Untrusted.push_back(O);
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
    llvm::SmallVector<llvm::StringRef, 3> Fields;
    Line.split(Fields, '\t');
    FunctionSummary S;
    if (Fields.size() != 3 || Fields[1].getAsInteger(16, S.Effects) ||
        Fields[2].size() % 2)
      return -1;
    for (size_t I = 0; I < Fields[2].size(); I += 2)
      S.Params.push_back({Fields[2][I], Fields[2][I + 1] == 'c'});
    Read[Fields[0]] = std::move(S);
  }
  for (auto& Entry : Read)
    Summaries()[Entry.getKey()] = std::move(Entry.getValue());
  return static_cast<int>(Read.size());
}

std::string SummarizeMemoryEffects(llvm::Module& M) {
  PrepareForAnalysis(M);
  Optimize(M);
  std::string Out = (SummaryHeader + std::to_string(LLVM_VERSION_MAJOR)).str();
  Out += "\n";
  for (const llvm::Function& F : M) {
    if (F.isDeclaration() || F.hasLocalLinkage() || !F.hasName())
      continue;
    Out += F.getName().str() + "\t";
    Out += llvm::utohexstr(F.getMemoryEffects().toIntValue()) + "\t";
    for (const llvm::Argument& A : F.args()) {
      char Access = '-';
      if (A.hasAttribute(llvm::Attribute::ReadNone))
        Access = 'n';
      else if (A.hasAttribute(llvm::Attribute::ReadOnly))
        Access = 'r';
      else if (A.hasAttribute(llvm::Attribute::WriteOnly))
        Access = 'w';
      Out += Access;
      Out += A.getType()->isPointerTy() &&
                     llvm::capturesNothing(A.getAttributes().getCaptureInfo())
                 ? 'c'
                 : '.';
    }
    Out += "\n";
  }
  return Out;
}

} // namespace CppInternal
