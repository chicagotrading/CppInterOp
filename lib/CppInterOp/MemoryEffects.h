//===--- MemoryEffects.h - Memory effects from optimized IR -----*- C++ -*-===//
//
// Part of the compiler-research project, under the Apache License v2.0 with
// LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef CPPINTEROP_MEMORYEFFECTS_H
#define CPPINTEROP_MEMORYEFFECTS_H

#include "CppInterOp/CppInterOpTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {
class Module;
} // namespace llvm

namespace CppInternal {

/// Whether bytes [Offset, Offset + Size) of an object of the class whose
/// defaulted copy or move constructor or assignment has the mangled name
/// \p Function hold no pointer. False when the function is not such a member.
using PointerFreeBytes = llvm::function_ref<bool(
    llvm::StringRef Function, uint64_t Offset, uint64_t Size)>;

/// Optimize one copy of \p M and store in \p Out, per name in \p Names, the
/// memory effects that LLVM infers for that function. \p M itself is not
/// changed. \p PointerFree, from the AST, makes the field copies of
/// defaulted members precise for PointerCaptured.
void AnalyzeMemoryEffects(const llvm::Module& M,
                          llvm::ArrayRef<std::string> Names,
                          std::vector<Cpp::MemoryEffects>& Out,
                          PointerFreeBytes PointerFree = {});

/// Read effect summaries of functions whose bodies the analysis cannot see
/// (a prebuilt library); the analysis applies them to matching declarations.
/// \returns the number of summaries read, or -1 when the text is malformed,
/// or was written by a different LLVM major version.
///
/// A table starts with "# cppinterop-memory-effects v1 llvm <major>". Each
/// other line is one function, tab-separated: the mangled name; seven fields
/// of the plain variant; a flag; and for an incomplete summary the seven
/// fields of the trusted variant followed by the opaque calls the body
/// reaches: the mangled name of a declaration, "*indirect" or "*asm", with
/// "!" before one that no signature bounds.
/// The seven fields: LLVM's MemoryEffects in hex; per parameter LLVM's
/// access (n/r/w/-) and capture (c none, a address only, . provenance); per
/// parameter the access to its own object (n/r/w/m); per parameter the
/// access to memory reachable from it; per parameter the capture (see
/// below); the access to other memory; the origin of a pointer result (f
/// fresh, u unknown, b both, . none) then per parameter d (into
/// its object), l (into its reachable memory), b or ".". The flag is "."
/// for a complete summary and "i" for one whose body reaches an opaque
/// call, so its effects are an upper bound only with that call included.
/// The capture field per parameter: c when a pointer to its object escapes;
/// r when only pointers that its memory holds escape; i when only integers
/// and characters of its memory escape. For a parameter whose
/// memory the function writes and nothing escapes: s when every stored
/// pointer points into that memory or into fresh memory that only it holds,
/// and o when that fresh memory holds only such pointers too. Else ".".
/// For a parameter that is no pointer: c when its value may leave the call
/// or become a pointer, else ".".
int LoadMemoryEffectsSummaries(llvm::StringRef Text);

/// Run the analysis pipeline on \p M in place and return the summaries of
/// every function it defines that the JIT cannot see from headers (not
/// local, not linkonce_odr, not available_externally), in the format that
/// LoadMemoryEffectsSummaries reads.
std::string SummarizeMemoryEffects(llvm::Module& M);

} // namespace CppInternal

#endif // CPPINTEROP_MEMORYEFFECTS_H
