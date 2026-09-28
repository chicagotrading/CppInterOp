//===--- cppinterop-effects.cpp - Build-time memory-effect tables --------===//
//
// Part of the compiler-research project, under the Apache License v2.0 with
// LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Writes the memory-effect summaries of the functions that a library defines,
// for Cpp::LoadMemoryEffectsSummaries. The inputs are the library's bitcode
// (or textual IR); the --summaries tables of its dependencies stand in for
// the bodies that the inputs only declare.
//
//===----------------------------------------------------------------------===//

#include "../../lib/CppInterOp/MemoryEffects.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"

#include <memory>
#include <string>

namespace {
llvm::cl::list<std::string> Inputs(llvm::cl::Positional, llvm::cl::OneOrMore,
                                   llvm::cl::desc("<bitcode or IR files>"));
llvm::cl::list<std::string>
    Summaries("summaries", llvm::cl::desc("A dependency's effect table"),
              llvm::cl::value_desc("file"));
llvm::cl::opt<std::string> Output("o", llvm::cl::Required,
                                  llvm::cl::desc("The effect table to write"),
                                  llvm::cl::value_desc("file"));

int Fail(const llvm::Twine& Message) {
  llvm::WithColor::error() << Message << "\n";
  return 1;
}
} // namespace

int main(int argc, char** argv) {
  llvm::InitLLVM X(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "Build-time memory-effect tables\n");

  for (const std::string& Path : Summaries) {
    auto Text = llvm::MemoryBuffer::getFile(Path);
    if (!Text)
      return Fail("cannot read " + Path);
    if (CppInternal::LoadMemoryEffectsSummaries((*Text)->getBuffer()) < 0)
      return Fail(Path + " is not an effect table of this LLVM version");
  }

  llvm::LLVMContext Context;
  auto Linked = std::make_unique<llvm::Module>("cppinterop-effects", Context);
  llvm::Linker L(*Linked);
  for (const std::string& Path : Inputs) {
    llvm::SMDiagnostic Err;
    std::unique_ptr<llvm::Module> M = llvm::parseIRFile(Path, Err, Context);
    if (!M) {
      Err.print(argv[0], llvm::errs());
      return 1;
    }
    if (L.linkInModule(std::move(M)))
      return Fail("cannot link " + Path);
  }

  std::error_code EC;
  llvm::ToolOutputFile Out(Output, EC, llvm::sys::fs::OF_Text);
  if (EC)
    return Fail("cannot write " + Output + ": " + EC.message());
  Out.os() << CppInternal::SummarizeMemoryEffects(*Linked);
  Out.keep();
  return 0;
}
