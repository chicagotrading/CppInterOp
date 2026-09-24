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
#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace llvm {
class Module;
} // namespace llvm

namespace CppInternal {

/// Optimize one copy of \p M and store in \p Out, per name in \p Names, the
/// memory effects that LLVM infers for that function. \p M itself is not
/// changed.
void AnalyzeMemoryEffects(const llvm::Module& M,
                          llvm::ArrayRef<std::string> Names,
                          std::vector<Cpp::MemoryEffects>& Out);

/// Read effect summaries of functions whose bodies the analysis cannot see
/// (a prebuilt library); the analysis applies them to matching declarations.
/// \returns the number of summaries read, or -1 when the text is malformed
/// or was written by a different LLVM major version.
int LoadMemoryEffectsSummaries(llvm::StringRef Text);

/// Run the analysis pipeline on \p M in place and return the summaries of
/// every function it defines with external linkage, in the format that
/// LoadMemoryEffectsSummaries reads.
std::string SummarizeMemoryEffects(llvm::Module& M);

} // namespace CppInternal

#endif // CPPINTEROP_MEMORYEFFECTS_H
