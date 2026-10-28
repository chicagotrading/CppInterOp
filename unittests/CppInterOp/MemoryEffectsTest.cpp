#include "Utils.h"

#include "../../lib/CppInterOp/MemoryEffects.h"
#include "CppInterOp/CppInterOp.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"
#include "llvm/Support/SourceMgr.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <string>

using namespace TestUtils;
using Cpp::ModRef;

#ifdef CPPINTEROP_USE_CLING
#define SKIP_WITHOUT_MEMORY_EFFECTS()                                          \
  GTEST_SKIP() << "GetFunctionsMemoryEffects is clang-repl only; with cling "  \
                  "it returns invalid results"
#else
#define SKIP_WITHOUT_MEMORY_EFFECTS() (void)0
#endif

#ifdef _WIN32
#define SKIP_ON_MSVC_ABI(Why)                                                  \
  GTEST_SKIP() << "expects the Itanium C++ ABI: " << Why
#else
#define SKIP_ON_MSVC_ABI(Why) (void)0
#endif

#ifdef _LIBCPP_VERSION
#define SKIP_ON_LIBCXX(Why) GTEST_SKIP() << "expects libstdc++: " << Why
#else
#define SKIP_ON_LIBCXX(Why) (void)0
#endif

namespace {
bool Mentions(const Cpp::MemoryEffects& ME, const std::string& Name) {
  return std::any_of(ME.Opaque.begin(), ME.Opaque.end(),
                     [&](const Cpp::OpaqueCall& O) {
                       return O.Demangled.find(Name) != std::string::npos;
                     });
}

bool HasOpaque(const Cpp::MemoryEffects& ME, Cpp::OpaqueKind Kind) {
  return std::any_of(ME.Opaque.begin(), ME.Opaque.end(),
                     [&](const Cpp::OpaqueCall& O) { return O.Kind == Kind; });
}

std::string OpaqueNames(const Cpp::MemoryEffects& ME) {
  std::string Out;
  for (const Cpp::OpaqueCall& O : ME.Opaque)
    Out += (Out.empty() ? "" : ", ") + O.Demangled;
  return Out;
}

bool AllBounded(const Cpp::MemoryEffects& ME) {
  return std::all_of(ME.Opaque.begin(), ME.Opaque.end(),
                     [](const Cpp::OpaqueCall& O) { return O.Bounded; });
}

bool Writes(ModRef MR) {
  return static_cast<unsigned>(MR) & static_cast<unsigned>(ModRef::Write);
}
} // namespace

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_PureRead) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Box { int v; };
    inline int twice(const Box& b) { return 2 * b.v; }
    extern "C" int me_pure(Box* b) { return twice(*b); }
  )",
                                                        "me_pure");
  ASSERT_TRUE(ME.Valid);
  EXPECT_EQ(ME.ArgMem, ModRef::Read);
  EXPECT_EQ(ME.Other, ModRef::None);
  ASSERT_EQ(ME.Params.size(), 1u);
  EXPECT_EQ(ME.Params[0], ModRef::Read);
  EXPECT_FALSE(ME.Captured[0]);
  EXPECT_TRUE(ME.Opaque.empty());
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_WritesOnlyTheWrittenParameter) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    inline void bump(int& x) { x += 1; }
    extern "C" void me_bump(int* a, int* b) { bump(*a); (void)*b; }
  )",
                                                        "me_bump");
  ASSERT_TRUE(ME.Valid);
  EXPECT_EQ(ME.Other, ModRef::None);
  ASSERT_EQ(ME.Params.size(), 2u);
  EXPECT_EQ(ME.Params[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.Params[1], ModRef::None);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_MutableMemberIsAWrite) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Counted { mutable int hits = 0; int get() const { return ++hits; } };
    extern "C" int me_mutable(Counted* c) {
      const Counted& view = *c;
      return view.get();
    }
  )",
                                                        "me_mutable");
  ASSERT_TRUE(ME.Valid);
  ASSERT_EQ(ME.Params.size(), 1u);
  EXPECT_EQ(ME.Params[0], ModRef::ReadWrite);
}

TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_HandlePointeeWriteIsReachableFromTheParameter) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Impl { double data[4]; };
    struct Tensor { Impl* impl; void add(double x) const { impl->data[0] += x; } };
    extern "C" void me_handle(Tensor* t) {
      const Tensor& view = *t;
      view.add(1.0);
    }
  )",
                                                        "me_handle");
  ASSERT_TRUE(ME.Valid);
  ASSERT_EQ(ME.Params.size(), 1u);
  // The handle itself is only read; the write goes through the pointer it
  // holds. Whether that memory is the handle's is the caller's decision.
  EXPECT_EQ(ME.Params[0], ModRef::Read);
  EXPECT_EQ(ME.ParamsDirect[0], ModRef::Read);
  EXPECT_EQ(ME.ParamsReachable[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.Other, ModRef::None);
  EXPECT_FALSE(ME.Captured[0]);
  EXPECT_TRUE(ME.Opaque.empty());
}

// A container's heap is memory reachable from the parameter that owns it,
// not other memory.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ContainerHeapIsReachable) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("libc++ inlines its throws around out-of-line exception "
                 "constructors, which are opaque calls");
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("#include <vector>"), 0);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    extern "C" void me_push(std::vector<int>* v, const int* x) { v->push_back(*x); }
  )",
                                                        "me_push");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(ME.Opaque.empty());
  EXPECT_EQ(ME.Other, ModRef::None);
  EXPECT_TRUE(Writes(ME.ParamsDirect[0]));
  EXPECT_TRUE(Writes(ME.ParamsReachable[0]));
  EXPECT_FALSE(ME.Captured[0]);
  EXPECT_EQ(ME.ParamsDirect[1], ModRef::Read);
  EXPECT_EQ(ME.ParamsReachable[1], ModRef::None);
  EXPECT_FALSE(ME.Captured[1]);
}

// A ring buffer over a vector, as a sliding-window state uses it.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RingBufferOverAVector) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("#include <vector>\n#include <utility>"), 0);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Window {
      std::vector<std::pair<double, long>> s{8};
      unsigned long head = 0, tail = 0;
      std::pair<double, long>& at(unsigned long i) { return s[i & (s.size() - 1)]; }
      void push(double v, long i) {
        while (head > tail && !(at(head - 1).first > v)) --head;
        head -= (head - tail == s.size());
        at(head++) = {v, i};
      }
      void purge(long i) { while (tail < head && i >= at(tail).second) ++tail; }
      double peek() const { return s[tail & (s.size() - 1)].first; }
    };
    extern "C" double me_window(Window* w, const double* v, const long* t) {
      w->push(*v, *t);
      w->purge(*t - 5);
      return w->peek();
    }
  )",
                                                        "me_window");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(ME.Opaque.empty());
  EXPECT_EQ(ME.Other, ModRef::None);
  EXPECT_EQ(ME.ParamsDirect[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.ParamsReachable[0], ModRef::ReadWrite);
  EXPECT_FALSE(ME.Captured[0]);
  EXPECT_EQ(ME.ParamsDirect[1], ModRef::Read);
  EXPECT_EQ(ME.ParamsDirect[2], ModRef::Read);
}

// Comparing a parameter's address keeps no pointer and writes nothing.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressCompareIsNotACapture) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("libc++'s erase frees the node through a unique_ptr "
                 "temporary, and a load from a local is other memory");
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("#include <unordered_set>"), 0);
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::GetFunctionsMemoryEffects(R"(
    struct Node { int v; Node* next; };
    struct List { Node* head; };
    extern "C" int me_find_addr(const List* l, const int* key) {
      for (Node* n = l->head; n; n = n->next)
        if (&n->v == key)
          return 1;
      return 0;
    }
    extern "C" void me_erase(std::unordered_set<int>* s, const int* key) { s->erase(*key); }
  )",
                                     {"me_find_addr", "me_erase"},
                                     /*rewind=*/false);
  ASSERT_EQ(MEs.size(), 2u);
  const Cpp::MemoryEffects& Find = MEs[0];
  ASSERT_TRUE(Find.Valid);
  EXPECT_EQ(Find.Other, ModRef::None);
  EXPECT_EQ(Find.ParamsDirect[0], ModRef::Read);
  EXPECT_EQ(Find.ParamsReachable[0], ModRef::Read);
  EXPECT_EQ(Find.ParamsDirect[1], ModRef::None);
  EXPECT_EQ(Find.ParamsReachable[1], ModRef::None);
  EXPECT_FALSE(Find.Captured[1]);

  const Cpp::MemoryEffects& Erase = MEs[1];
  ASSERT_TRUE(Erase.Valid);
  EXPECT_TRUE(Erase.Opaque.empty());
  EXPECT_EQ(Erase.Other, ModRef::None);
  EXPECT_TRUE(Writes(Erase.ParamsDirect[0]) ||
              Writes(Erase.ParamsReachable[0]));
  EXPECT_FALSE(Writes(Erase.ParamsDirect[1]));
  EXPECT_EQ(Erase.ParamsReachable[1], ModRef::None);
  EXPECT_FALSE(Erase.Captured[1]);
}

// A write through a const parameter shows on that parameter, whatever else
// the function writes.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_MutableWriteNextToAWrittenParameter) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Counted { mutable int hits = 0; int get() const { return ++hits; } };
    extern "C" void me_mutable_pair(int* out, const Counted* c) { *out = c->get(); }
  )",
                                                        "me_mutable_pair");
  ASSERT_TRUE(ME.Valid);
  EXPECT_EQ(ME.Other, ModRef::None);
  EXPECT_EQ(ME.ParamsDirect[0], ModRef::Write);
  EXPECT_EQ(ME.ParamsDirect[1], ModRef::ReadWrite);
  EXPECT_EQ(ME.ParamsReachable[1], ModRef::None);
}

// Two handles may point to one object: the write is reachable from the
// written handle, the read from the other, and a pointer that may come from
// either counts for both.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TwoHandlesToOneObject) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::GetFunctionsMemoryEffects(R"(
    struct Impl { int v; };
    struct H { Impl* p; };
    extern "C" void me_two_handles(const H* a, const H* b) { a->p->v = b->p->v + 1; }
    extern "C" void me_either_handle(const H* a, const H* b, int pick) {
      (pick ? a->p : b->p)->v = 1;
    }
  )",
                                     {"me_two_handles", "me_either_handle"},
                                     /*rewind=*/false);
  ASSERT_EQ(MEs.size(), 2u);
  const Cpp::MemoryEffects& Two = MEs[0];
  ASSERT_TRUE(Two.Valid);
  EXPECT_EQ(Two.Other, ModRef::None);
  EXPECT_EQ(Two.ParamsDirect[0], ModRef::Read);
  EXPECT_EQ(Two.ParamsReachable[0], ModRef::Write);
  EXPECT_EQ(Two.ParamsDirect[1], ModRef::Read);
  EXPECT_EQ(Two.ParamsReachable[1], ModRef::Read);
  EXPECT_FALSE(Two.Captured[0]);
  EXPECT_FALSE(Two.Captured[1]);

  const Cpp::MemoryEffects& Either = MEs[1];
  ASSERT_TRUE(Either.Valid);
  EXPECT_EQ(Either.Other, ModRef::None);
  EXPECT_TRUE(Writes(Either.ParamsReachable[0]));
  EXPECT_TRUE(Writes(Either.ParamsReachable[1]));
}

// A pointer into a parameter's memory that another parameter's memory or a
// global keeps is a capture; one that a fresh node of the same parameter
// keeps is not.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CaptureIsAStoreOutsideTheOwner) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct Slot { const int* p; };
    const int* me_keep_global = nullptr;
    extern "C" void me_keep(Slot* s, const int* x) { s->p = x; }
    extern "C" void me_keep_in_global(const int* x) { me_keep_global = x; }
    struct N { N* next; int v; };
    struct L { N* head; };
    extern "C" void me_link(L* l, const int* x) { l->head = new N{l->head, *x}; }
  )",
      {"me_keep", "me_keep_in_global", "me_link"},
      /*rewind=*/false);
  ASSERT_EQ(MEs.size(), 3u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_FALSE(MEs[0].Captured[0]);
  EXPECT_TRUE(MEs[0].Captured[1]);
  EXPECT_EQ(MEs[0].ParamsDirect[0], ModRef::Write);
  ASSERT_TRUE(MEs[1].Valid);
  EXPECT_TRUE(MEs[1].Captured[0]);
  EXPECT_EQ(MEs[1].Other, ModRef::Write);
  ASSERT_TRUE(MEs[2].Valid);
  EXPECT_TRUE(MEs[2].Opaque.empty());
  EXPECT_EQ(MEs[2].Other, ModRef::None);
  EXPECT_EQ(MEs[2].ParamsDirect[0], ModRef::ReadWrite);
  EXPECT_FALSE(MEs[2].Captured[0]);
  EXPECT_FALSE(MEs[2].Captured[1]);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_GlobalWriteIsOtherMemory) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    int me_counter = 0;
    extern "C" int me_global(const int* x) { ++me_counter; return *x; }
  )",
                                                        "me_global");
  ASSERT_TRUE(ME.Valid);
  EXPECT_EQ(ME.Params[0], ModRef::Read);
  EXPECT_NE(static_cast<unsigned>(ME.Other) &
                static_cast<unsigned>(ModRef::Write),
            0u);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ExternalCallIsOpaque) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    int me_external(const int& x);
    extern "C" int me_calls_external(int* x) { return me_external(*x); }
  )",
                                                        "me_calls_external");
  ASSERT_TRUE(ME.Valid);
  EXPECT_EQ(ME.Other, ModRef::ReadWrite);
  EXPECT_TRUE(Mentions(ME, "me_external"));
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IndirectCallIsOpaque) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Callback { int (*fn)(int); };
    extern "C" int me_indirect(Callback* c) { return c->fn(1); }
  )",
                                                        "me_indirect");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(HasOpaque(ME, Cpp::OpaqueKind::Indirect));
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ThrowPathIsNotAnEffect) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    inline int checked(const int* p, int i) {
      if (i < 0)
        throw 1;
      return p[i];
    }
    extern "C" int me_throws(int* p) { return checked(p, p[1]); }
  )",
                                                        "me_throws");
  ASSERT_TRUE(ME.Valid);
  EXPECT_EQ(ME.Other, ModRef::None);
  EXPECT_EQ(ME.Params[0], ModRef::Read);
  EXPECT_TRUE(ME.Opaque.empty());
}

// An inline function that an earlier input already emitted must still be
// analyzed through its body.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_InlineBodyFromEarlierInput) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare(R"(
    inline void me_poke(int& x) { x = 7; }
    extern "C" void me_first_use(int* p) { me_poke(*p); }
  )"),
            0);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    extern "C" void me_second_use(int* p, int* q) { me_poke(*p); (void)*q; }
  )",
                                                        "me_second_use");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(ME.Opaque.empty());
  EXPECT_EQ(ME.Other, ModRef::None);
  ASSERT_EQ(ME.Params.size(), 2u);
  EXPECT_EQ(ME.Params[0], ModRef::Write);
  EXPECT_EQ(ME.Params[1], ModRef::None);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_InvalidCode) {
  TestFixture::CreateInterpreter();
  // MSBuild fails a test step that prints a "file(l,c): error X:" line.
  testing::internal::CaptureStderr();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(
      R"(extern "C" int me_bad(int* p) { return no_such_function(*p); })",
      "me_bad");
  std::string Err = testing::internal::GetCapturedStderr();
  EXPECT_FALSE(ME.Valid);
#ifndef CPPINTEROP_USE_CLING
  EXPECT_NE(Err.find("no_such_function"), std::string::npos) << Err;
#endif
  ME = Cpp::GetFunctionMemoryEffects(
      R"(extern "C" int me_other(int) { return 0; })", "me_missing");
  EXPECT_FALSE(ME.Valid);
}

// An address that leaves as an integer is a capture; one that is only
// compared or subtracted is not.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_AddressAsIntegerCapturesOnlyWhenKept) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::GetFunctionsMemoryEffects(R"(
    using me_uptr = __UINTPTR_TYPE__;
    me_uptr me_int_global = 0;
    extern "C" void me_int_escape(const int* x) { me_int_global = (me_uptr)x; }
    extern "C" int me_int_compare(const int* a, const int* b) {
      return (me_uptr)a < (me_uptr)b ? (int)((me_uptr)b - (me_uptr)a) : 0;
    }
  )",
                                     {"me_int_escape", "me_int_compare"},
                                     /*rewind=*/false);
  ASSERT_EQ(MEs.size(), 2u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(MEs[0].Captured[0]);
  ASSERT_TRUE(MEs[1].Valid);
  EXPECT_FALSE(MEs[1].Captured[0]);
  EXPECT_FALSE(MEs[1].Captured[1]);
  EXPECT_EQ(MEs[1].Other, ModRef::None);
}

namespace {
std::string SummaryHeader() {
  return "# cppinterop-memory-effects v1 llvm " +
         std::to_string(LLVM_VERSION_MAJOR) + "\n";
}

// The seven fields of a variant that reads one const-reference parameter.
std::string ReadsItsParameter() {
  return llvm::utohexstr(llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref)
                             .toIntValue()) +
         "\trc\tr\tn\t.\tn\t..";
}

// The seven fields of a variant bounded by nothing.
std::string Anything() {
  return llvm::utohexstr(llvm::MemoryEffects::unknown().toIntValue()) +
         "\t-.\tm\tm\tc\tm\tu.";
}
} // namespace

// A summary from a build-time table stands in for a body the module lacks.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_SummaryMakesADeclarationPrecise) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  TestFixture::CreateInterpreter();
  std::string Table =
      SummaryHeader() + "_Z10me_summaryRKi\t" + ReadsItsParameter() + "\t.\n";
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    int me_summary(const int& x);
    extern "C" int me_uses_summary(int* x) { return me_summary(*x); }
  )",
                                                        "me_uses_summary");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(ME.Opaque.empty());
  EXPECT_EQ(ME.Other, ModRef::None);
  ASSERT_EQ(ME.Params.size(), 1u);
  EXPECT_EQ(ME.Params[0], ModRef::Read);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SummaryTableIsValidated) {
  TestFixture::CreateInterpreter();
  // A table from another LLVM major version.
  EXPECT_EQ(
      Cpp::LoadMemoryEffectsSummaries(
          "# cppinterop-memory-effects v1 llvm 0\nf\t0\t\t\t\t\tn\t.\t.\n"),
      -1);
  EXPECT_EQ(Cpp::LoadMemoryEffectsSummaries(
                (SummaryHeader() + "f\tzz\t\t\t\t\tn\t.\t.\n").c_str()),
            -1);
  EXPECT_EQ(Cpp::LoadMemoryEffectsSummaries(
                (SummaryHeader() + "f\t0\tr\tr\tn\t.\tn\t..\t.\n").c_str()),
            -1);
  // An incomplete summary carries its trusted variant.
  EXPECT_EQ(Cpp::LoadMemoryEffectsSummaries(
                (SummaryHeader() + "f\t" + Anything() + "\ti\n").c_str()),
            -1);
  EXPECT_EQ(Cpp::LoadMemoryEffectsSummaries(
                (SummaryHeader() + "f\t" + Anything() + "\t.\n").c_str()),
            1);
  EXPECT_EQ(Cpp::LoadMemoryEffectsSummaries(SummaryHeader().c_str()), 0);
}

// An incomplete summary brings the opaque calls of its body: the untrusted
// fields include them, the trusted fields bound them by their signatures.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IncompleteSummaryIsOpaque) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  TestFixture::CreateInterpreter();
  std::string Table = SummaryHeader() + "_Z13me_incompleteRKi\t" + Anything() +
                      "\ti\t" + ReadsItsParameter() + "\t_Z9me_loggerRKi\n" +
                      "_Z12me_unboundedRKi\t" + Anything() + "\ti\t" +
                      Anything() + "\t!me_sink\n";
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 2);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    int me_incomplete(const int& x);
    int me_unbounded(const int& x);
    extern "C" int me_uses_incomplete(int* x) { return me_incomplete(*x); }
    extern "C" int me_uses_unbounded(int* x) { return me_unbounded(*x); }
  )",
      {"me_uses_incomplete", "me_uses_unbounded"},
      /*rewind=*/false);
  ASSERT_EQ(MEs.size(), 2u);
  const Cpp::MemoryEffects& Inc = MEs[0];
  ASSERT_TRUE(Inc.Valid);
  ASSERT_EQ(Inc.Opaque.size(), 1u);
  EXPECT_EQ(Inc.Opaque[0].Kind, Cpp::OpaqueKind::Declaration);
  EXPECT_EQ(Inc.Opaque[0].Mangled, "_Z9me_loggerRKi");
  EXPECT_EQ(Inc.Opaque[0].Demangled, "me_logger(int const&)");
  EXPECT_TRUE(Inc.Opaque[0].Bounded);
  ASSERT_TRUE(Inc.Opaque[0].Signature.Known);
  EXPECT_EQ(Inc.Opaque[0].Signature.Params,
            std::vector<Cpp::ParamKind>{Cpp::ParamKind::ConstReference});
  EXPECT_EQ(Inc.Other, ModRef::ReadWrite);
  EXPECT_EQ(Inc.TrustedOther, ModRef::None);
  EXPECT_EQ(Inc.TrustedParamsDirect[0], ModRef::Read);
  EXPECT_FALSE(Inc.TrustedCaptured[0]);

  const Cpp::MemoryEffects& Unb = MEs[1];
  ASSERT_TRUE(Unb.Valid);
  ASSERT_EQ(Unb.Opaque.size(), 1u);
  EXPECT_EQ(Unb.Opaque[0].Mangled, "me_sink");
  EXPECT_FALSE(Unb.Opaque[0].Bounded);
  EXPECT_FALSE(Unb.Opaque[0].Signature.Known); // a C name has no parameters
  EXPECT_EQ(Unb.TrustedOther, ModRef::ReadWrite);
}

// A complete summary is a proof: a write it records stays under trust.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CompleteSummaryBeatsTrust) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  TestFixture::CreateInterpreter();
  std::string Effects = llvm::utohexstr(
      llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef).toIntValue());
  std::string Table = SummaryHeader() + "_Z11me_completeRKi\t" + Effects +
                      "\t-c\tm\tn\t.\tn\t..\t.\n";
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    int me_complete(const int& x);
    int me_trust_bait(const int& x);
    extern "C" int me_uses_complete(int* x) { return me_complete(*x) + me_trust_bait(*x); }
  )",
                                                        "me_uses_complete");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(Mentions(ME, "me_trust_bait"));
  EXPECT_FALSE(Mentions(ME, "me_complete"));
  EXPECT_EQ(ME.ParamsDirect[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.TrustedParamsDirect[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.TrustedOther, ModRef::None);
}

// The summarizer marks a body that reaches an opaque call, records its
// trusted variant and the call, and skips what the JIT sees from headers.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_SummarizerMarksIncompleteBodies) {
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    declare void @_Z10me_sum_logRKi(ptr)
    declare void @me_sum_c_log(ptr)
    define i32 @me_sum_complete(ptr %p) {
      %v = load i32, ptr %p
      ret i32 %v
    }
    define void @me_sum_incomplete(ptr %p) {
      call void @_Z10me_sum_logRKi(ptr %p)
      store i32 1, ptr %p
      ret void
    }
    define void @me_sum_unbounded(ptr %p) {
      call void @me_sum_c_log(ptr %p)
      ret void
    }
    define linkonce_odr void @me_sum_inline(ptr %p) {
      store i32 1, ptr %p
      ret void
    }
    define weak_odr void @me_sum_explicit(ptr %p) {
      store i32 1, ptr %p
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  EXPECT_EQ(Table.find("me_sum_inline"), std::string::npos);
  EXPECT_NE(Table.find("\nme_sum_explicit\t"), std::string::npos);
  llvm::SmallVector<llvm::StringRef, 8> Lines;
  llvm::StringRef(Table).split(Lines, '\n', -1, false);
  for (llvm::StringRef Line : Lines) {
    llvm::SmallVector<llvm::StringRef, 20> Fields;
    Line.split(Fields, '\t');
    if (Fields[0] == "me_sum_complete") {
      ASSERT_EQ(Fields.size(), 9u);
      EXPECT_EQ(Fields[3], "r"); // reads its own object
      EXPECT_EQ(Fields[6], "n");
      EXPECT_EQ(Fields[8], ".");
    } else if (Fields[0] == "me_sum_incomplete") {
      ASSERT_EQ(Fields.size(), 17u);
      EXPECT_EQ(Fields[3], "m");
      EXPECT_EQ(Fields[6], "m");
      EXPECT_EQ(Fields[8], "i");
      EXPECT_EQ(Fields[11], "m"); // the store is proven whatever the log does
      EXPECT_EQ(Fields[14], "n"); // trusted: the log touches no other memory
      EXPECT_EQ(Fields[16], "_Z10me_sum_logRKi");
    } else if (Fields[0] == "me_sum_unbounded") {
      ASSERT_EQ(Fields.size(), 17u);
      EXPECT_EQ(Fields[16], "!me_sum_c_log");
    }
  }
  EXPECT_EQ(CppInternal::LoadMemoryEffectsSummaries(Table), 4);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SummarizerSortsOpaqueNames) {
  // The closure visits callees in pointer order; the table must not.
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    declare void @zeta(ptr)
    declare void @alpha(ptr)
    define void @me_ord_b(ptr %p) {
      call void @zeta(ptr %p)
      ret void
    }
    define void @me_ord_a(ptr %p) {
      call void @alpha(ptr %p)
      ret void
    }
    define void @me_ord_top(ptr %p) {
      call void @me_ord_b(ptr %p)
      call void @me_ord_a(ptr %p)
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  llvm::SmallVector<llvm::StringRef, 8> Lines;
  llvm::StringRef(Table).split(Lines, '\n', -1, false);
  bool Found = false;
  for (llvm::StringRef Line : Lines) {
    if (!Line.starts_with("me_ord_top\t"))
      continue;
    Found = true;
    EXPECT_TRUE(Line.ends_with("\t!alpha\t!zeta")) << Line.str();
  }
  EXPECT_TRUE(Found) << Table;
}

// The out-of-line hash of the prebuilt libstdc++ sits behind a noexcept
// std::hash: with its summary, the terminate path of the invoke adds no
// effect.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_NoexceptTerminatePathIsNotAnEffect) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
#if defined(__aarch64__) || defined(_M_ARM64)
  GTEST_SKIP() << "AArch64 returns the one-pointer iterator as i64; the "
                  "analysis loses its origin through inttoptr";
#endif
  SKIP_ON_LIBCXX("libc++ can hash with an out-of-line std::__hash_memory; "
                 "the test has a summary only for libstdc++'s _Hash_bytes");
  // std::string_view: the extern template of std::string hides its bodies.
  TestFixture::CreateInterpreter({"-std=c++17"});
  std::string Hash = llvm::utohexstr(
      llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref).toIntValue());
  std::string Table = SummaryHeader() + "_ZSt11_Hash_bytesPKvmm\t" + Hash +
                      "\trc-.-.\trnn\tnnn\t...\tn\t....\t.\n";
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1);
  ASSERT_EQ(Cpp::Declare("#include <string_view>\n#include <unordered_map>"),
            0);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Table { std::unordered_map<std::string_view, int> m; };
    extern "C" int me_lookup(const Table* t, const std::string_view* k) {
      auto it = t->m.find(*k);
      return it == t->m.end() ? -1 : it->second;
    }
  )",
                                                        "me_lookup");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(ME.Opaque.empty()) << OpaqueNames(ME);
  EXPECT_EQ(ME.Other, ModRef::None);
  EXPECT_EQ(ME.ParamsDirect[0], ModRef::Read);
  EXPECT_EQ(ME.ParamsReachable[0], ModRef::Read);
  EXPECT_EQ(ME.ParamsDirect[1], ModRef::Read);
  EXPECT_FALSE(Writes(ME.ParamsReachable[1]));
  EXPECT_FALSE(ME.Captured[0]);
  EXPECT_FALSE(ME.Captured[1]);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SeveralFunctionsOfOneUnit) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::GetFunctionsMemoryEffects(R"(
    inline void me_set(int& x) { x = 1; }
    extern "C" void me_first(int* a, int* b) { me_set(*a); (void)*b; }
    extern "C" int me_second(int* a) { return *a; }
  )",
                                     {"me_first", "me_second", "me_absent"},
                                     /*rewind=*/false);
  ASSERT_EQ(MEs.size(), 3u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(MEs[0].Params[0], ModRef::Write);
  EXPECT_EQ(MEs[0].Params[1], ModRef::None);
  ASSERT_TRUE(MEs[1].Valid);
  EXPECT_EQ(MEs[1].Params[0], ModRef::Read);
  EXPECT_FALSE(MEs[2].Valid);
  EXPECT_TRUE(Cpp::GetFunctionAddress("me_second"));
}

// The rewind undoes exactly the probe: the unit before it stays, and one
// Undo afterwards removes that unit.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RewindUndoesExactlyTheProbe) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("namespace me_before_ns { int kept = 3; }"), 0);
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::GetFunctionsMemoryEffects(R"(
    namespace me_probe_ns {
    struct Box { int v; };
    extern "C" int me_rewound(Box* b) { return b->v; }
    }
  )",
                                     {"me_rewound"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(MEs[0].Params[0], ModRef::Read);
  EXPECT_FALSE(Cpp::GetNamed("me_probe_ns"));
  EXPECT_FALSE(Cpp::GetFunctionAddress("me_rewound"));
  EXPECT_TRUE(Cpp::GetNamed("me_before_ns"));
  ASSERT_EQ(Cpp::Undo(1), 0);
  EXPECT_FALSE(Cpp::GetNamed("me_before_ns"));
}

#if !defined(_WIN32)
// The braces make this a definition; `extern "C" int x = 1;` warns in gcc.
extern "C" {
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
__attribute__((visibility("default"))) __thread int me_native_tls = 1;
}
#endif

// A native thread-local makes the probe's Declare add a second PTU (the
// executor's empty unit); the rewind must undo both and nothing before them.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RewindCountsInternalUnits) {
#if defined(_WIN32) || defined(EMSCRIPTEN) || defined(CPPINTEROP_USE_CLING)
  GTEST_SKIP() << "the native-TLS redirect is clang-repl on ELF and Mach-O";
#endif
  if (TypeParam::isOutOfProcess)
    GTEST_SKIP() << "the dlsym-based native-TLS redirect is in-process only";
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("namespace me_tls_before_ns { int kept = 3; }"), 0);
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::GetFunctionsMemoryEffects(R"(
    namespace me_tls_probe_ns {
    extern "C" __thread int me_native_tls;
    extern "C" int me_reads_tls(int* p) { return *p + me_native_tls; }
    }
  )",
                                     {"me_reads_tls"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_FALSE(Cpp::GetNamed("me_tls_probe_ns"));
  EXPECT_FALSE(Cpp::GetFunctionAddress("me_reads_tls"));
  EXPECT_TRUE(Cpp::GetNamed("me_tls_before_ns"));
  ASSERT_EQ(Cpp::Undo(1), 0);
  EXPECT_FALSE(Cpp::GetNamed("me_tls_before_ns"));
}

// Code that a rewound probe emitted first is emitted again by a later unit.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RewoundProbeLeavesCodeReusable) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare(R"(
    template <class T> T me_thrice(T x) { return x + x + x; }
    inline int me_inline_twice(int x) { return 2 * x; }
  )"),
            0);
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::GetFunctionsMemoryEffects(R"(
    extern "C" float me_probe_uses(float* p, int* q) {
      return me_thrice(*p) + me_inline_twice(*q);
    }
  )",
                                     {"me_probe_uses"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_FALSE(Cpp::GetFunctionAddress("me_probe_uses"));
  ASSERT_EQ(Cpp::Declare(R"(
    extern "C" float me_after_probe(float x, int y) {
      return me_thrice(x) + me_inline_twice(y);
    }
  )"),
            0);
  void* Addr = Cpp::GetFunctionAddress("me_after_probe");
  ASSERT_NE(Addr, nullptr);
  // An out-of-process address is in the executor, not in this process.
  if (TypeParam::isOutOfProcess)
    return;
  auto* Fn = reinterpret_cast<float (*)(float, int)>(Addr);
  EXPECT_EQ(Fn(1.0f, 2), 7.0f);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CalleesNameTheSelectedOverload) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the test expects Itanium mangled and demangled names");
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::GetFunctionsMemoryEffects(R"(
    namespace me_callee_ns {
    struct Box { int v; };
    inline int pick(const Box& b) { return b.v; }
    inline int pick(Box& b) { return -b.v; }
    struct Holder { int get(int x) const { return x; } int get(int x) { return -x; } };
    extern "C" void me_callee_free(const Box* b) { (void)pick(*b); }
    extern "C" void me_callee_member(const Holder* h, int* x) { (void)(*h).get(*x); }
    }
  )",
                                     {"me_callee_free", "me_callee_member"},
                                     /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  ASSERT_EQ(MEs[0].Callees.size(), 1u);
  EXPECT_EQ(MEs[0].Callees[0].Mangled, "_ZN12me_callee_ns4pickERKNS_3BoxE");
  EXPECT_EQ(MEs[0].Callees[0].Demangled,
            "me_callee_ns::pick(me_callee_ns::Box const&)");
  const Cpp::FunctionSignature& Pick = MEs[0].Callees[0].Signature;
  ASSERT_TRUE(Pick.Known);
  EXPECT_EQ(Pick.BaseName, "pick");
  EXPECT_EQ(Pick.Params,
            std::vector<Cpp::ParamKind>{Cpp::ParamKind::ConstReference});
  EXPECT_FALSE(Pick.ConstMethod);
  ASSERT_EQ(MEs[1].Callees.size(), 1u);
  EXPECT_EQ(MEs[1].Callees[0].Demangled,
            "me_callee_ns::Holder::get(int) const");
  const Cpp::FunctionSignature& Get = MEs[1].Callees[0].Signature;
  EXPECT_EQ(Get.BaseName, "get");
  EXPECT_EQ(Get.Params, std::vector<Cpp::ParamKind>{Cpp::ParamKind::Scalar});
  EXPECT_TRUE(Get.ConstMethod);
}

// The signature of each callee comes from its mangled name, one kind per
// parameter.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CalleeSignatureKinds) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    namespace me_kinds_ns {
    struct Box { int v; };
    struct C { int f; };
    int all(const Box& a, Box& b, Box&& c, const Box* d, Box* e, int f, Box g,
            int (*h)(int), int C::*i);
    template <class T> int tmpl(const T& t);
    int var(int n, ...);
    extern "C" int me_kinds_c(int x);
    }
    extern "C" int me_kinds(me_kinds_ns::Box* b, int (*h)(int)) {
      me_kinds_ns::Box g{1};
      return me_kinds_ns::all(*b, *b, static_cast<me_kinds_ns::Box&&>(*b), b,
                              b, 1, g, h, &me_kinds_ns::C::f) +
             me_kinds_ns::tmpl(*b) + me_kinds_ns::var(1, 2) +
             me_kinds_ns::me_kinds_c(3);
    }
  )",
      {"me_kinds"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  auto Find = [&](const std::string& Base) -> const Cpp::FunctionSignature* {
    for (const Cpp::MemoryEffects::Callee& C : MEs[0].Callees)
      if (C.Signature.BaseName == Base)
        return &C.Signature;
    return nullptr;
  };
  using K = Cpp::ParamKind;
  const Cpp::FunctionSignature* All = Find("all");
  ASSERT_NE(All, nullptr);
  EXPECT_EQ(All->Params, (std::vector<K>{K::ConstReference, K::MutableReference,
                                         K::MutableReference, K::ConstPointer,
                                         K::MutablePointer, K::Scalar,
                                         K::Object, K::Unknown, K::Unknown}));
  EXPECT_FALSE(All->Variadic);
  const Cpp::FunctionSignature* Tmpl = Find("tmpl");
  ASSERT_NE(Tmpl, nullptr);
  EXPECT_EQ(Tmpl->Params, std::vector<K>{K::ConstReference});
  const Cpp::FunctionSignature* Var = Find("var");
  ASSERT_NE(Var, nullptr);
  EXPECT_TRUE(Var->Variadic);
  EXPECT_EQ(Var->Params, std::vector<K>{K::Scalar});
  bool SawC = false;
  for (const Cpp::MemoryEffects::Callee& C : MEs[0].Callees)
    if (C.Mangled == "me_kinds_c") {
      SawC = true;
      EXPECT_FALSE(C.Signature.Known);
    }
  EXPECT_TRUE(SawC);
}

// The trusted fields show what the visible code does around opaque calls.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TrustedFieldsIgnoreOpaqueCalls) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct Box { int v; };
    int me_trust_opaque(const Box& b);
    int me_trust_counter = 0;
    extern "C" int me_trust_only(Box* b) { return me_trust_opaque(*b); }
    extern "C" int me_trust_global(Box* b) {
      ++me_trust_counter;
      return me_trust_opaque(*b);
    }
    extern "C" int me_trust_indirect(int (*fn)(int*), int* x) { return fn(x); }
    extern "C" int me_trust_none(Box* b) { return b->v; }
  )",
      {"me_trust_only", "me_trust_global", "me_trust_indirect",
       "me_trust_none"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 4u);
  const Cpp::MemoryEffects& Only = MEs[0];
  ASSERT_TRUE(Only.Valid);
  EXPECT_TRUE(Mentions(Only, "me_trust_opaque"));
  EXPECT_EQ(Only.Other, ModRef::ReadWrite);
  EXPECT_TRUE(Only.Captured[0]);
  EXPECT_EQ(Only.TrustedOther, ModRef::None);
  EXPECT_FALSE(Only.TrustedCaptured[0]);

  const Cpp::MemoryEffects& Global = MEs[1];
  ASSERT_TRUE(Global.Valid);
  EXPECT_NE(static_cast<unsigned>(Global.TrustedOther) &
                static_cast<unsigned>(ModRef::Write),
            0u);

  const Cpp::MemoryEffects& Indirect = MEs[2];
  ASSERT_TRUE(Indirect.Valid);
  EXPECT_TRUE(HasOpaque(Indirect, Cpp::OpaqueKind::Indirect));
  EXPECT_EQ(Indirect.TrustedOther, ModRef::None);
  EXPECT_FALSE(Indirect.TrustedCaptured[1]);

  const Cpp::MemoryEffects& None = MEs[3];
  ASSERT_TRUE(None.Valid);
  EXPECT_TRUE(None.Opaque.empty());
  EXPECT_EQ(None.TrustedOther, None.Other);
  EXPECT_EQ(None.TrustedCaptured, None.Captured);
}

// The trusted parameters follow the signature of each opaque call.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TrustedParamsFollowSignatures) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the expected parameters follow the Itanium ABI lowering");
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    namespace me_sig_ns {
    struct Box { int v; };
    struct Pair { long a; long b; };
    int by_const_ref(const Box& b);
    void by_ref(Box& b);
    int by_const_ptr(const Box* b, int n);
    void by_pair(Pair p, const Box& b);
    struct Holder {
      int peek() const;
      void poke();
    };
    extern "C" int me_sig_const(Box* b, Box* c, int* n) {
      return by_const_ref(*b) + by_const_ptr(c, *n);
    }
    extern "C" void me_sig_ref(Box* b) { by_ref(*b); }
    extern "C" int me_sig_methods(Holder* h, Holder* g) {
      g->poke();
      return h->peek();
    }
    extern "C" void me_sig_unmapped(Pair* p, Box* b) { by_pair(*p, *b); }
    }
  )",
      {"me_sig_const", "me_sig_ref", "me_sig_methods", "me_sig_unmapped"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 4u);
  const Cpp::MemoryEffects& Const = MEs[0];
  ASSERT_TRUE(Const.Valid);
  EXPECT_EQ(Const.Params[0], ModRef::ReadWrite);
  EXPECT_EQ(Const.TrustedParams[0], ModRef::Read);
  EXPECT_EQ(Const.TrustedParams[1], ModRef::Read);
  EXPECT_EQ(Const.TrustedOther, ModRef::None);
  EXPECT_TRUE(AllBounded(Const));

  const Cpp::MemoryEffects& Ref = MEs[1];
  ASSERT_TRUE(Ref.Valid);
  EXPECT_NE(static_cast<unsigned>(Ref.TrustedParams[0]) &
                static_cast<unsigned>(ModRef::Write),
            0u);
  EXPECT_TRUE(AllBounded(Ref));

  const Cpp::MemoryEffects& Methods = MEs[2];
  ASSERT_TRUE(Methods.Valid);
  EXPECT_EQ(Methods.TrustedParams[0], ModRef::Read);
  EXPECT_NE(static_cast<unsigned>(Methods.TrustedParams[1]) &
                static_cast<unsigned>(ModRef::Write),
            0u);
  EXPECT_TRUE(AllBounded(Methods));

  const Cpp::MemoryEffects& Unmapped = MEs[3];
  ASSERT_TRUE(Unmapped.Valid);
  ASSERT_EQ(std::count_if(Unmapped.Opaque.begin(), Unmapped.Opaque.end(),
                          [](const Cpp::OpaqueCall& O) { return !O.Bounded; }),
            1);
  EXPECT_TRUE(std::any_of(Unmapped.Opaque.begin(), Unmapped.Opaque.end(),
                          [](const Cpp::OpaqueCall& O) {
                            return !O.Bounded &&
                                   O.Signature.BaseName == "by_pair";
                          }));
  EXPECT_NE(static_cast<unsigned>(Unmapped.TrustedParams[1]) &
                static_cast<unsigned>(ModRef::Write),
            0u);
}

// The exception object of a throw is fresh memory, also when an opaque
// constructor fills it and the runtime destroys it.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ThrowOfAClassObjectIsLocal) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct me_error {
      me_error(const char* what);
      ~me_error();
      const char* what;
    };
    extern "C" int me_throw_object(int* p) {
      if (p[0] < 0)
        throw me_error("negative");
      p[1] = p[0];
      return p[0];
    }
  )",
                                                        "me_throw_object");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(Mentions(ME, "me_error"));
  EXPECT_TRUE(AllBounded(ME));
  EXPECT_EQ(ME.TrustedOther, ModRef::None);
  EXPECT_EQ(ME.TrustedParamsDirect[0], ModRef::ReadWrite);
  EXPECT_FALSE(ME.TrustedCaptured[0]);
}

// A load from a fresh allocation yields the pointers stored into it, until
// a write that the analysis does not see may change it.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_FreshAllocationContent) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_table { int** slots; int own; };
    extern "C" void me_rebuild(me_table* t, int n) {
      int** fresh = new int*[n];
      for (int i = 0; i < n; ++i)
        fresh[i] = i ? t->slots[i] : &t->own;
      for (int i = 0; i < n; ++i)
        *fresh[i] += 1;
      delete[] t->slots;
      t->slots = fresh;
    }
    void me_fill(int** slots);
    extern "C" void me_rebuild_opaque(me_table* t) {
      int** fresh = new int*[2];
      fresh[0] = &t->own;
      me_fill(fresh);
      *fresh[1] = 1;
      t->slots = fresh;
    }
  )",
      {"me_rebuild", "me_rebuild_opaque"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  const Cpp::MemoryEffects& Known = MEs[0];
  ASSERT_TRUE(Known.Valid);
  EXPECT_TRUE(Known.Opaque.empty());
  EXPECT_EQ(Known.Other, ModRef::None);
  EXPECT_TRUE(Writes(Known.ParamsDirect[0]));
  EXPECT_TRUE(Writes(Known.ParamsReachable[0]));
  EXPECT_FALSE(Known.Captured[0]);

  // The callee may store any pointer into the allocation.
  const Cpp::MemoryEffects& Filled = MEs[1];
  ASSERT_TRUE(Filled.Valid);
  EXPECT_TRUE(Mentions(Filled, "me_fill"));
  EXPECT_TRUE(Writes(Filled.TrustedOther));
}

// A pointer into a parameter's memory that leaves in an aggregate result,
// like the iterator of an insert, is a result and not a capture. A fresh
// object that only the result holds exposes what it points to.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_PointerInAnAggregateResult) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_node { me_node* next; int v; };
    struct me_list { me_node* head; };
    struct me_hit { me_node* node; bool added; };
    extern "C" me_hit me_insert(me_list* l, const int* v) {
      for (me_node* n = l->head; n; n = n->next)
        if (n->v == *v)
          return {n, false};
      me_node* n = new me_node{l->head, *v};
      l->head = n;
      return {n, true};
    }
    extern "C" me_hit me_wrap(me_list* l) {
      return {new me_node{l->head, 0}, true};
    }
    extern "C" me_node* me_wrap_ptr(me_list* l) {
      return new me_node{l->head, 0};
    }
  )",
      {"me_insert", "me_wrap", "me_wrap_ptr"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 3u);
  const Cpp::MemoryEffects& Insert = MEs[0];
  ASSERT_TRUE(Insert.Valid);
  EXPECT_TRUE(Insert.Opaque.empty());
  EXPECT_EQ(Insert.Other, ModRef::None);
  EXPECT_TRUE(Writes(Insert.ParamsDirect[0]));
  EXPECT_FALSE(Insert.Captured[0]);
  EXPECT_FALSE(Insert.Captured[1]);

  ASSERT_TRUE(MEs[1].Valid);
  EXPECT_TRUE(MEs[1].Captured[0]);
  ASSERT_TRUE(MEs[2].Valid);
  EXPECT_TRUE(MEs[2].Captured[0]);
}

// Hash containers rebuild their bucket arrays, link nodes out of line and
// return iterators in a pair. With the rehash policy trusted to its
// signature, the container's memory stays its own.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_HashContainerInsert) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("expects the out-of-line rehash policy of libstdc++");
  TestFixture::CreateInterpreter();
  ASSERT_EQ(
      Cpp::Declare("#include <unordered_map>\n#include <unordered_set>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    extern "C" void me_set_insert(std::unordered_set<long>* s, const long* k) {
      s->insert(*k);
    }
    extern "C" void me_map_assign(std::unordered_map<int, double>* m,
                                  const int* k, const double* v) {
      (*m)[*k] = *v;
    }
  )",
      {"me_set_insert", "me_map_assign"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(AllBounded(ME)) << OpaqueNames(ME);
    EXPECT_EQ(ME.TrustedOther, ModRef::None) << OpaqueNames(ME);
    EXPECT_TRUE(Writes(ME.TrustedParamsDirect[0]));
    EXPECT_FALSE(ME.TrustedCaptured[0]);
    // The node keeps a copy of the key's bytes, not its address.
    EXPECT_FALSE(ME.TrustedAddressCaptured[1]);
  }
}

// A copy of a parameter's bytes may copy the pointers that its memory holds,
// but not its address; storing the pointer itself keeps the address.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CopyIsNotAnAddressCapture) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_info { double price; bool bid; };
    struct me_cell { me_cell* next; me_info info; const me_info* ref; };
    struct me_cells { me_cell* head; };
    extern "C" void me_add_copy(me_cells* c, const me_info* i) {
      c->head = new me_cell{c->head, *i, nullptr};
    }
    extern "C" void me_add_ref(me_cells* c, const me_info* i) {
      c->head = new me_cell{c->head, {}, i};
    }
  )",
      {"me_add_copy", "me_add_ref"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(MEs[0].Other, ModRef::None);
  EXPECT_FALSE(MEs[0].Captured[0]);
  EXPECT_FALSE(MEs[0].AddressCaptured[1]);
  ASSERT_TRUE(MEs[1].Valid);
  EXPECT_TRUE(MEs[1].Captured[1]);
  EXPECT_TRUE(MEs[1].AddressCaptured[1]);
}

// Ways a pointer reaches memory that the analysis must keep following.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_PointersThatLeaveTheOrigins) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_h { int* p; };
    extern "C" void me_compact(me_h* src, unsigned long n) {
      me_h* tmp = new me_h[n];
      unsigned long k = 0;
      for (unsigned long i = 0; i < n; ++i)
        if (src[i].p)
          tmp[k++] = src[i];
      for (unsigned long i = 0; i < k; ++i)
        ++*tmp[i].p;
      delete[] tmp;
    }
    struct me_exc { bool handled; };
    extern "C" void me_handle() {
      try {
        throw;
      } catch (me_exc& e) {
        e.handled = true;
      }
    }
    struct me_box { int* v; };
    struct me_boxes { me_box* head; };
    __attribute__((noinline)) me_box* me_push(me_boxes* l) {
      me_box* b = new me_box{};
      if (l)
        l->head = b;
      return b;
    }
    extern "C" me_box* me_push_null(int* q) {
      me_box* b = me_push(nullptr);
      b->v = q;
      return b;
    }
    extern "C" unsigned long me_hide(int* p) {
      return reinterpret_cast<unsigned long>(new me_box{p});
    }
  )",
      {"me_compact", "me_handle", "me_push_null", "me_hide"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 4u);
  // An 8-byte struct copy becomes an integer load and store.
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].ParamsReachable[0]) || Writes(MEs[0].Other));
  // The caught object is shared with the handlers that rethrow it.
  ASSERT_TRUE(MEs[1].Valid);
  EXPECT_TRUE(Writes(MEs[1].TrustedOther));
  ASSERT_TRUE(MEs[2].Valid);
  EXPECT_TRUE(MEs[2].Captured[0]);
  ASSERT_TRUE(MEs[3].Valid);
  EXPECT_TRUE(MEs[3].Captured[0]);
}

// A vector compares and subtracts the addresses of the buffer it allocates;
// the address leaves nothing as an integer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressArithmeticOfAFreshBuffer) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("libc++ inlines its throws around out-of-line exception "
                 "constructors, which are opaque calls");
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("#include <vector>"), 0);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    extern "C" unsigned long me_push_size(std::vector<long>* v, const long* x) {
      v->push_back(*x);
      return v->size();
    }
  )",
                                                        "me_push_size");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(ME.Opaque.empty()) << OpaqueNames(ME);
  EXPECT_EQ(ME.Other, ModRef::None);
  EXPECT_FALSE(ME.Captured[0]);
  EXPECT_FALSE(ME.AddressCaptured[1]);
}

// A lookup that returns a copy through a temporary: the callee builds the
// result from fresh memory and the result's own buffer, and the caller frees
// it again.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ResultInATemporary) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("expects the char_traits and string layout of libstdc++");
  TestFixture::CreateInterpreter({"-std=c++20"});
  ASSERT_EQ(Cpp::Declare("#include <cstdint>\n#include <optional>\n"
                         "#include <string>\n#include <unordered_map>"),
            0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_info { int64_t id; std::string group; uint64_t mult; };
    struct me_infos {
      std::optional<me_info> get(int64_t k) const {
        const auto it = m.find(k);
        return it != m.end() ? std::make_optional<>(it->second) : std::nullopt;
      }
      std::unordered_map<int64_t, me_info> m;
    };
    extern "C" void me_discard(const me_infos* h, const int64_t* k) {
      (void)h->get(*k);
    }
    extern "C" void me_keep(const me_infos* h, const int64_t* k, void* out) {
      ::new (out) std::optional<me_info>(h->get(*k));
    }
  )",
      {"me_discard", "me_keep"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(ME.Opaque.empty()) << OpaqueNames(ME);
    EXPECT_EQ(ME.Other, ModRef::None);
    EXPECT_FALSE(Writes(ME.ParamsDirect[0]));
    EXPECT_FALSE(Writes(ME.ParamsReachable[0]));
  }
  // The kept result holds copied bytes of the receiver's memory.
  EXPECT_FALSE(MEs[0].Captured[0]);
}

// A pointer that the callee stores into the result, or into fresh memory
// that the result holds, keeps its origin in the caller.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ForeignPointerInAResult) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    int me_global;
    struct me_res { int* p; long a, b, c; };
    struct me_cell { int** cell; long a, b, c; };
    __attribute__((noinline)) me_res me_wrap(int* q) { return {q, 0, 0, 0}; }
    __attribute__((noinline)) me_res me_wrap_global() {
      return {&me_global, 0, 0, 0};
    }
    __attribute__((noinline)) me_cell me_wrap_deep(int* q) {
      return {new int*(q), 0, 0, 0};
    }
    extern "C" void me_through(int* q) { *me_wrap(q).p = 1; }
    extern "C" void me_through_global() { *me_wrap_global().p = 1; }
    extern "C" void me_through_deep(int* q) {
      me_cell c = me_wrap_deep(q);
      **c.cell = 1;
      delete c.cell;
    }
  )",
      {"me_through", "me_through_global", "me_through_deep"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 3u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(Writes(MEs[0].ParamsDirect[0]) || Writes(MEs[0].Other));
  EXPECT_TRUE(Writes(MEs[1].Other));
  EXPECT_TRUE(Writes(MEs[2].ParamsDirect[0]) || Writes(MEs[2].Other));
}

// Integers of an unknown source, and integer fields that hold an address,
// still carry it.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressesInIntegers) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    int me_int_global;
    __attribute__((noinline)) void me_put(long* slot, long v) { *slot = v; }
    __attribute__((noinline)) void me_set(unsigned long* s) {
      *s = reinterpret_cast<unsigned long>(&me_int_global);
    }
    extern "C" void me_via_put(int** p) {
      int* x = nullptr;
      me_put(reinterpret_cast<long*>(&x), *reinterpret_cast<long*>(p));
      *x = 1;
    }
    extern "C" void me_via_set() {
      int* x;
      me_set(reinterpret_cast<unsigned long*>(&x));
      *x = 1;
    }
  )",
      {"me_via_put", "me_via_set"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].ParamsReachable[0]) || Writes(MEs[0].Other));
  ASSERT_TRUE(MEs[1].Valid);
  EXPECT_TRUE(Writes(MEs[1].Other));
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressInAnIntegerField) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_handle { unsigned long bits; };
    inline void me_copy_handle(me_handle* d, const me_handle* s) {
      d->bits = s->bits;
    }
    extern "C" void me_keep_address(int* p, me_handle* out) {
      me_handle h{reinterpret_cast<unsigned long>(p)};
      me_copy_handle(out, &h);
    }
  )",
      {"me_keep_address"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(MEs[0].Captured[0]);
}

// A memcpy into an integer field may copy a pointer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IntegerCopies) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_ints { long v; };
    struct me_ptrs { int* p; };
    extern "C" void me_copy_bytes(me_ints* dst, const me_ptrs* src) {
      __builtin_memcpy(&dst->v, &src->p, sizeof(long));
    }
  )",
      {"me_copy_bytes"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(MEs[0].Captured[1]);
}

// A string copy copies characters: the result's own memory stays local.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CharacterCopies) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("expects the char_traits of libstdc++");
  TestFixture::CreateInterpreter({"-std=c++20"});
  ASSERT_EQ(Cpp::Declare("#include <string>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    extern "C" void me_copy_string(const std::string* s, void* out) {
      ::new (out) std::string(*s);
    }
    extern "C" void me_bytes_of_a_pointer(std::string* out, int* const* src) {
      out->assign(reinterpret_cast<const char*>(src), sizeof(int*));
    }
  )",
      {"me_copy_string", "me_bytes_of_a_pointer"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(MEs[0].Opaque.empty()) << OpaqueNames(MEs[0]);
  EXPECT_EQ(MEs[0].Other, ModRef::None);
  // Characters may be the bytes of a pointer: here those of an int*.
  ASSERT_TRUE(MEs[1].Valid);
  EXPECT_TRUE(MEs[1].Captured[1]);
  EXPECT_TRUE(MEs[1].PointerCaptured[1]);
}

// A copy of a string, or of the fields of a class, copies data: the copy
// keeps no pointer into the source.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_StringCopyKeepsNoPointer) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("expects the char_traits of libstdc++");
  TestFixture::CreateInterpreter({"-std=c++20"});
  ASSERT_EQ(Cpp::Declare("#include <string>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_params { std::string key; double x; bool invalid; };
    struct me_word { std::string key; int n; float x; };
    extern "C" void me_assign_string(std::string* d, const std::string* s) {
      *d = *s;
    }
    extern "C" void me_assign_struct(me_params* d, const me_params* s) {
      *d = *s;
    }
    extern "C" void me_construct_string(const std::string* s, void* out) {
      ::new (out) std::string(*s);
    }
    extern "C" void me_construct_struct(const me_params* s, void* out) {
      ::new (out) me_params(*s);
    }
    extern "C" void me_assign_word(me_word* d, const me_word* s) { *d = *s; }
  )",
      {"me_assign_string", "me_assign_struct", "me_construct_string",
       "me_construct_struct", "me_assign_word"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 5u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(ME.Opaque.empty()) << OpaqueNames(ME);
    EXPECT_EQ(ME.Other, ModRef::None);
  }
  // The source is parameter 1 of an assignment, 0 of a construction.
  for (unsigned I : {0u, 1u, 4u}) {
    EXPECT_TRUE(MEs[I].Captured[1]) << I;
    EXPECT_FALSE(MEs[I].PointerCaptured[1]) << I;
    EXPECT_FALSE(MEs[I].AddressCaptured[1]) << I;
  }
  for (unsigned I : {2u, 3u}) {
    EXPECT_TRUE(MEs[I].Captured[0]) << I;
    EXPECT_FALSE(MEs[I].PointerCaptured[0]) << I;
    EXPECT_FALSE(MEs[I].AddressCaptured[0]) << I;
  }
}

// A kept inner pointer, or a pointer field in a copied run of fields, is a
// pointer capture. An address in an integer field is data.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_PointerCaptureFollowsTypes) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("expects the char_traits of libstdc++");
  TestFixture::CreateInterpreter({"-std=c++20"});
  ASSERT_EQ(Cpp::Declare("#include <string>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_raw { std::string key; int* p; double x; };
    struct me_bits { std::string key; __UINTPTR_TYPE__ a; double x; };
    const char* me_kept_chars;
    extern "C" void me_keep_data(const std::string* s) {
      me_kept_chars = s->data();
    }
    extern "C" void me_data_into(const char** d, const std::string* s) {
      *d = s->data();
    }
    extern "C" void me_assign_raw(me_raw* d, const me_raw* s) { *d = *s; }
    extern "C" void me_construct_raw(const me_raw* s, void* out) {
      ::new (out) me_raw(*s);
    }
    extern "C" void me_assign_bits(me_bits* d, const me_bits* s) { *d = *s; }
  )",
      {"me_keep_data", "me_data_into", "me_assign_raw", "me_construct_raw",
       "me_assign_bits"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 5u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(MEs[0].PointerCaptured[0]);
  EXPECT_FALSE(MEs[0].AddressCaptured[0]);
  EXPECT_TRUE(MEs[1].PointerCaptured[1]);
  EXPECT_TRUE(MEs[2].PointerCaptured[1]);
  EXPECT_TRUE(MEs[3].PointerCaptured[0]);
  EXPECT_TRUE(MEs[4].Captured[1]);
  EXPECT_FALSE(MEs[4].PointerCaptured[1]);
  EXPECT_FALSE(MEs[4].AddressCaptured[1]);
}

// A summary carries the kind of the capture.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SummaryCarriesPointerCapture) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  TestFixture::CreateInterpreter();
  // It reads other memory too, so its captures are not internal.
  std::string Effects =
      llvm::utohexstr(llvm::MemoryEffects::readOnly().toIntValue());
  auto Keeps = [&](const std::string& Name, char Capture) {
    return Name + "\t" + Effects + "\tr.\tr\tr\t" + Capture + "\tr\t..\t.\n";
  };
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries((SummaryHeader() +
                                             Keeps("_Z9me_keep_iPKi", 'i') +
                                             Keeps("_Z9me_keep_rPKi", 'r'))
                                                .c_str()),
            2);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    void me_keep_i(const int* x);
    void me_keep_r(const int* x);
    extern "C" void me_uses_i(const int* x) { me_keep_i(x); }
    extern "C" void me_uses_r(const int* x) { me_keep_r(x); }
  )",
      {"me_uses_i", "me_uses_r"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(ME.Opaque.empty()) << OpaqueNames(ME);
    EXPECT_TRUE(ME.Captured[0]);
    EXPECT_FALSE(ME.AddressCaptured[0]);
  }
  EXPECT_FALSE(MEs[0].PointerCaptured[0]);
  EXPECT_TRUE(MEs[1].PointerCaptured[0]);
}
