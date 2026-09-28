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

namespace {
bool Mentions(const Cpp::MemoryEffects& ME, const std::string& Name) {
  return std::any_of(
      ME.m_Opaque.begin(), ME.m_Opaque.end(),
      [&](const std::string& O) { return O.find(Name) != std::string::npos; });
}

bool Writes(ModRef MR) {
  return static_cast<unsigned>(MR) & static_cast<unsigned>(ModRef::Write);
}
} // namespace

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_PureRead) {
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Box { int v; };
    inline int twice(const Box& b) { return 2 * b.v; }
    extern "C" int me_pure(Box* b) { return twice(*b); }
  )",
                                                        "me_pure");
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_EQ(ME.m_ArgMem, ModRef::Read);
  EXPECT_EQ(ME.m_Other, ModRef::None);
  ASSERT_EQ(ME.m_Params.size(), 1u);
  EXPECT_EQ(ME.m_Params[0], ModRef::Read);
  EXPECT_FALSE(ME.m_Captured[0]);
  EXPECT_TRUE(ME.m_Opaque.empty());
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_WritesOnlyTheWrittenParameter) {
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    inline void bump(int& x) { x += 1; }
    extern "C" void me_bump(int* a, int* b) { bump(*a); (void)*b; }
  )",
                                                        "me_bump");
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_EQ(ME.m_Other, ModRef::None);
  ASSERT_EQ(ME.m_Params.size(), 2u);
  EXPECT_EQ(ME.m_Params[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.m_Params[1], ModRef::None);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_MutableMemberIsAWrite) {
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Counted { mutable int hits = 0; int get() const { return ++hits; } };
    extern "C" int me_mutable(Counted* c) {
      const Counted& view = *c;
      return view.get();
    }
  )",
                                                        "me_mutable");
  ASSERT_TRUE(ME.m_Valid);
  ASSERT_EQ(ME.m_Params.size(), 1u);
  EXPECT_EQ(ME.m_Params[0], ModRef::ReadWrite);
}

TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_HandlePointeeWriteIsReachableFromTheParameter) {
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
  ASSERT_TRUE(ME.m_Valid);
  ASSERT_EQ(ME.m_Params.size(), 1u);
  // The handle itself is only read; the write goes through the pointer it
  // holds. Whether that memory is the handle's is the caller's decision.
  EXPECT_EQ(ME.m_Params[0], ModRef::Read);
  EXPECT_EQ(ME.m_ParamsDirect[0], ModRef::Read);
  EXPECT_EQ(ME.m_ParamsReachable[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.m_Other, ModRef::None);
  EXPECT_FALSE(ME.m_Captured[0]);
  EXPECT_TRUE(ME.m_Opaque.empty());
}

// A container's heap is memory reachable from the parameter that owns it,
// not other memory.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ContainerHeapIsReachable) {
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("#include <vector>"), 0);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    extern "C" void me_push(std::vector<int>* v, const int* x) { v->push_back(*x); }
  )",
                                                        "me_push");
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_TRUE(ME.m_Opaque.empty());
  EXPECT_EQ(ME.m_Other, ModRef::None);
  EXPECT_TRUE(Writes(ME.m_ParamsDirect[0]));
  EXPECT_TRUE(Writes(ME.m_ParamsReachable[0]));
  EXPECT_FALSE(ME.m_Captured[0]);
  EXPECT_EQ(ME.m_ParamsDirect[1], ModRef::Read);
  EXPECT_EQ(ME.m_ParamsReachable[1], ModRef::None);
  EXPECT_FALSE(ME.m_Captured[1]);
}

// A ring buffer over a vector, as a sliding-window state uses it.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RingBufferOverAVector) {
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
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_TRUE(ME.m_Opaque.empty());
  EXPECT_EQ(ME.m_Other, ModRef::None);
  EXPECT_EQ(ME.m_ParamsDirect[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.m_ParamsReachable[0], ModRef::ReadWrite);
  EXPECT_FALSE(ME.m_Captured[0]);
  EXPECT_EQ(ME.m_ParamsDirect[1], ModRef::Read);
  EXPECT_EQ(ME.m_ParamsDirect[2], ModRef::Read);
}

// Comparing a parameter's address keeps no pointer and writes nothing.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressCompareIsNotACapture) {
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
  ASSERT_TRUE(Find.m_Valid);
  EXPECT_EQ(Find.m_Other, ModRef::None);
  EXPECT_EQ(Find.m_ParamsDirect[0], ModRef::Read);
  EXPECT_EQ(Find.m_ParamsReachable[0], ModRef::Read);
  EXPECT_EQ(Find.m_ParamsDirect[1], ModRef::None);
  EXPECT_EQ(Find.m_ParamsReachable[1], ModRef::None);
  EXPECT_FALSE(Find.m_Captured[1]);

  const Cpp::MemoryEffects& Erase = MEs[1];
  ASSERT_TRUE(Erase.m_Valid);
  EXPECT_TRUE(Erase.m_Opaque.empty());
  EXPECT_EQ(Erase.m_Other, ModRef::None);
  EXPECT_TRUE(Writes(Erase.m_ParamsDirect[0]) ||
              Writes(Erase.m_ParamsReachable[0]));
  EXPECT_FALSE(Writes(Erase.m_ParamsDirect[1]));
  EXPECT_EQ(Erase.m_ParamsReachable[1], ModRef::None);
  EXPECT_FALSE(Erase.m_Captured[1]);
}

// A write through a const parameter shows on that parameter, whatever else
// the function writes.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_MutableWriteNextToAWrittenParameter) {
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Counted { mutable int hits = 0; int get() const { return ++hits; } };
    extern "C" void me_mutable_pair(int* out, const Counted* c) { *out = c->get(); }
  )",
                                                        "me_mutable_pair");
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_EQ(ME.m_Other, ModRef::None);
  EXPECT_EQ(ME.m_ParamsDirect[0], ModRef::Write);
  EXPECT_EQ(ME.m_ParamsDirect[1], ModRef::ReadWrite);
  EXPECT_EQ(ME.m_ParamsReachable[1], ModRef::None);
}

// Two handles may point to one object: the write is reachable from the
// written handle, the read from the other, and a pointer that may come from
// either counts for both.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TwoHandlesToOneObject) {
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
  ASSERT_TRUE(Two.m_Valid);
  EXPECT_EQ(Two.m_Other, ModRef::None);
  EXPECT_EQ(Two.m_ParamsDirect[0], ModRef::Read);
  EXPECT_EQ(Two.m_ParamsReachable[0], ModRef::Write);
  EXPECT_EQ(Two.m_ParamsDirect[1], ModRef::Read);
  EXPECT_EQ(Two.m_ParamsReachable[1], ModRef::Read);
  EXPECT_FALSE(Two.m_Captured[0]);
  EXPECT_FALSE(Two.m_Captured[1]);

  const Cpp::MemoryEffects& Either = MEs[1];
  ASSERT_TRUE(Either.m_Valid);
  EXPECT_EQ(Either.m_Other, ModRef::None);
  EXPECT_TRUE(Writes(Either.m_ParamsReachable[0]));
  EXPECT_TRUE(Writes(Either.m_ParamsReachable[1]));
}

// A pointer into a parameter's memory that another parameter's memory or a
// global keeps is a capture; one that a fresh node of the same parameter
// keeps is not.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CaptureIsAStoreOutsideTheOwner) {
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
  ASSERT_TRUE(MEs[0].m_Valid);
  EXPECT_FALSE(MEs[0].m_Captured[0]);
  EXPECT_TRUE(MEs[0].m_Captured[1]);
  EXPECT_EQ(MEs[0].m_ParamsDirect[0], ModRef::Write);
  ASSERT_TRUE(MEs[1].m_Valid);
  EXPECT_TRUE(MEs[1].m_Captured[0]);
  EXPECT_EQ(MEs[1].m_Other, ModRef::Write);
  ASSERT_TRUE(MEs[2].m_Valid);
  EXPECT_TRUE(MEs[2].m_Opaque.empty());
  EXPECT_EQ(MEs[2].m_Other, ModRef::None);
  EXPECT_EQ(MEs[2].m_ParamsDirect[0], ModRef::ReadWrite);
  EXPECT_FALSE(MEs[2].m_Captured[0]);
  EXPECT_FALSE(MEs[2].m_Captured[1]);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_GlobalWriteIsOtherMemory) {
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    int me_counter = 0;
    extern "C" int me_global(const int* x) { ++me_counter; return *x; }
  )",
                                                        "me_global");
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_EQ(ME.m_Params[0], ModRef::Read);
  EXPECT_NE(static_cast<unsigned>(ME.m_Other) &
                static_cast<unsigned>(ModRef::Write),
            0u);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ExternalCallIsOpaque) {
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    int me_external(const int& x);
    extern "C" int me_calls_external(int* x) { return me_external(*x); }
  )",
                                                        "me_calls_external");
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_EQ(ME.m_Other, ModRef::ReadWrite);
  EXPECT_TRUE(Mentions(ME, "me_external"));
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IndirectCallIsOpaque) {
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct Callback { int (*fn)(int); };
    extern "C" int me_indirect(Callback* c) { return c->fn(1); }
  )",
                                                        "me_indirect");
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_TRUE(Mentions(ME, "<indirect>"));
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ThrowPathIsNotAnEffect) {
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
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_EQ(ME.m_Other, ModRef::None);
  EXPECT_EQ(ME.m_Params[0], ModRef::Read);
  EXPECT_TRUE(ME.m_Opaque.empty());
}

// An inline function that an earlier input already emitted must still be
// analyzed through its body.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_InlineBodyFromEarlierInput) {
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
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_TRUE(ME.m_Opaque.empty());
  EXPECT_EQ(ME.m_Other, ModRef::None);
  ASSERT_EQ(ME.m_Params.size(), 2u);
  EXPECT_EQ(ME.m_Params[0], ModRef::Write);
  EXPECT_EQ(ME.m_Params[1], ModRef::None);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_InvalidCode) {
  TestFixture::CreateInterpreter();
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(
      R"(extern "C" int me_bad(int* p) { return no_such_function(*p); })",
      "me_bad");
  EXPECT_FALSE(ME.m_Valid);
  ME = Cpp::GetFunctionMemoryEffects(
      R"(extern "C" int me_other(int) { return 0; })", "me_missing");
  EXPECT_FALSE(ME.m_Valid);
}

// An address that leaves as an integer is a capture; one that is only
// compared or subtracted is not.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_AddressAsIntegerCapturesOnlyWhenKept) {
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::GetFunctionsMemoryEffects(R"(
    unsigned long me_int_global = 0;
    extern "C" void me_int_escape(const int* x) { me_int_global = (unsigned long)x; }
    extern "C" int me_int_compare(const int* a, const int* b) {
      return (unsigned long)a < (unsigned long)b ? (int)((unsigned long)b - (unsigned long)a) : 0;
    }
  )",
                                     {"me_int_escape", "me_int_compare"},
                                     /*rewind=*/false);
  ASSERT_EQ(MEs.size(), 2u);
  ASSERT_TRUE(MEs[0].m_Valid);
  EXPECT_TRUE(MEs[0].m_Captured[0]);
  ASSERT_TRUE(MEs[1].m_Valid);
  EXPECT_FALSE(MEs[1].m_Captured[0]);
  EXPECT_FALSE(MEs[1].m_Captured[1]);
  EXPECT_EQ(MEs[1].m_Other, ModRef::None);
}

namespace {
std::string SummaryHeader() {
  return "# cppinterop-memory-effects v2 llvm " +
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
  TestFixture::CreateInterpreter();
  std::string Table =
      SummaryHeader() + "_Z10me_summaryRKi\t" + ReadsItsParameter() + "\t.\n";
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    int me_summary(const int& x);
    extern "C" int me_uses_summary(int* x) { return me_summary(*x); }
  )",
                                                        "me_uses_summary");
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_TRUE(ME.m_Opaque.empty());
  EXPECT_EQ(ME.m_Other, ModRef::None);
  ASSERT_EQ(ME.m_Params.size(), 1u);
  EXPECT_EQ(ME.m_Params[0], ModRef::Read);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SummaryTableIsValidated) {
  TestFixture::CreateInterpreter();
  EXPECT_EQ(
      Cpp::LoadMemoryEffectsSummaries(
          "# cppinterop-memory-effects v2 llvm 0\nf\t0\t\t\t\t\tn\t.\t.\n"),
      -1);
  // A v1 table has no reachable-memory fields.
  EXPECT_EQ(Cpp::LoadMemoryEffectsSummaries(
                ("# cppinterop-memory-effects v1 llvm " +
                 std::to_string(LLVM_VERSION_MAJOR) + "\nf\t0\t\n")
                    .c_str()),
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
  TestFixture::CreateInterpreter();
  std::string Table = SummaryHeader() + "_Z13me_incompleteRKi\t" + Anything() +
                      "\ti\t" + ReadsItsParameter() +
                      "\tme_logger(int const&)\n" + "_Z12me_unboundedRKi\t" +
                      Anything() + "\ti\t" + Anything() + "\t!me_sink\n";
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
  ASSERT_TRUE(Inc.m_Valid);
  ASSERT_EQ(Inc.m_Opaque, std::vector<std::string>{"me_logger(int const&)"});
  EXPECT_TRUE(Inc.m_Untrusted.empty());
  EXPECT_EQ(Inc.m_Other, ModRef::ReadWrite);
  EXPECT_EQ(Inc.m_TrustedOther, ModRef::None);
  EXPECT_EQ(Inc.m_TrustedParamsDirect[0], ModRef::Read);
  EXPECT_FALSE(Inc.m_TrustedCaptured[0]);

  const Cpp::MemoryEffects& Unb = MEs[1];
  ASSERT_TRUE(Unb.m_Valid);
  ASSERT_EQ(Unb.m_Opaque, std::vector<std::string>{"me_sink"});
  EXPECT_EQ(Unb.m_Untrusted, std::vector<std::string>{"me_sink"});
  EXPECT_EQ(Unb.m_TrustedOther, ModRef::ReadWrite);
}

// A complete summary is a proof: a write it records stays under trust.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CompleteSummaryBeatsTrust) {
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
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_TRUE(Mentions(ME, "me_trust_bait"));
  EXPECT_FALSE(Mentions(ME, "me_complete"));
  EXPECT_EQ(ME.m_ParamsDirect[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.m_TrustedParamsDirect[0], ModRef::ReadWrite);
  EXPECT_EQ(ME.m_TrustedOther, ModRef::None);
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
      EXPECT_EQ(Fields[16], "me_sum_log(int const&)");
    } else if (Fields[0] == "me_sum_unbounded") {
      ASSERT_EQ(Fields.size(), 17u);
      EXPECT_EQ(Fields[16], "!me_sum_c_log");
    }
  }
  EXPECT_EQ(CppInternal::LoadMemoryEffectsSummaries(Table), 4);
}

// The out-of-line hash of the prebuilt libstdc++ sits behind a noexcept
// std::hash: with its summary, the terminate path of the invoke adds no
// effect.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_NoexceptTerminatePathIsNotAnEffect) {
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
  ASSERT_TRUE(ME.m_Valid);
  EXPECT_TRUE(ME.m_Opaque.empty()) << llvm::join(ME.m_Opaque, ", ");
  EXPECT_EQ(ME.m_Other, ModRef::None);
  EXPECT_EQ(ME.m_ParamsDirect[0], ModRef::Read);
  EXPECT_EQ(ME.m_ParamsReachable[0], ModRef::Read);
  EXPECT_EQ(ME.m_ParamsDirect[1], ModRef::Read);
  EXPECT_FALSE(Writes(ME.m_ParamsReachable[1]));
  EXPECT_FALSE(ME.m_Captured[0]);
  EXPECT_FALSE(ME.m_Captured[1]);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SeveralFunctionsOfOneUnit) {
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
  ASSERT_TRUE(MEs[0].m_Valid);
  EXPECT_EQ(MEs[0].m_Params[0], ModRef::Write);
  EXPECT_EQ(MEs[0].m_Params[1], ModRef::None);
  ASSERT_TRUE(MEs[1].m_Valid);
  EXPECT_EQ(MEs[1].m_Params[0], ModRef::Read);
  EXPECT_FALSE(MEs[2].m_Valid);
  EXPECT_TRUE(Cpp::GetFunctionAddress("me_second"));
}

// The rewind undoes exactly the probe: the unit before it stays, and one
// Undo afterwards removes that unit.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RewindUndoesExactlyTheProbe) {
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
  ASSERT_TRUE(MEs[0].m_Valid);
  EXPECT_EQ(MEs[0].m_Params[0], ModRef::Read);
  EXPECT_FALSE(Cpp::GetNamed("me_probe_ns"));
  EXPECT_FALSE(Cpp::GetFunctionAddress("me_rewound"));
  EXPECT_TRUE(Cpp::GetNamed("me_before_ns"));
  ASSERT_EQ(Cpp::Undo(1), 0);
  EXPECT_FALSE(Cpp::GetNamed("me_before_ns"));
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
extern "C" __attribute__((visibility("default"))) __thread int me_native_tls =
    1;

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
  ASSERT_TRUE(MEs[0].m_Valid);
  EXPECT_FALSE(Cpp::GetNamed("me_tls_probe_ns"));
  EXPECT_FALSE(Cpp::GetFunctionAddress("me_reads_tls"));
  EXPECT_TRUE(Cpp::GetNamed("me_tls_before_ns"));
  ASSERT_EQ(Cpp::Undo(1), 0);
  EXPECT_FALSE(Cpp::GetNamed("me_tls_before_ns"));
}

// Code that a rewound probe emitted first is emitted again by a later unit.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RewoundProbeLeavesCodeReusable) {
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
  ASSERT_TRUE(MEs[0].m_Valid);
  EXPECT_FALSE(Cpp::GetFunctionAddress("me_probe_uses"));
  ASSERT_EQ(Cpp::Declare(R"(
    extern "C" float me_after_probe(float x, int y) {
      return me_thrice(x) + me_inline_twice(y);
    }
  )"),
            0);
  auto* Fn = reinterpret_cast<float (*)(float, int)>(
      Cpp::GetFunctionAddress("me_after_probe"));
  ASSERT_NE(Fn, nullptr);
  EXPECT_EQ(Fn(1.0f, 2), 7.0f);
}

TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CalleesNameTheSelectedOverload) {
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
  ASSERT_EQ(MEs[0].m_Callees.size(), 1u);
  EXPECT_EQ(MEs[0].m_Callees[0].m_Mangled, "_ZN12me_callee_ns4pickERKNS_3BoxE");
  EXPECT_EQ(MEs[0].m_Callees[0].m_Demangled,
            "me_callee_ns::pick(me_callee_ns::Box const&)");
  ASSERT_EQ(MEs[1].m_Callees.size(), 1u);
  EXPECT_EQ(MEs[1].m_Callees[0].m_Demangled,
            "me_callee_ns::Holder::get(int) const");
}

// The trusted fields show what the visible code does around opaque calls.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TrustedFieldsIgnoreOpaqueCalls) {
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
  ASSERT_TRUE(Only.m_Valid);
  EXPECT_TRUE(Mentions(Only, "me_trust_opaque"));
  EXPECT_EQ(Only.m_Other, ModRef::ReadWrite);
  EXPECT_TRUE(Only.m_Captured[0]);
  EXPECT_EQ(Only.m_TrustedOther, ModRef::None);
  EXPECT_FALSE(Only.m_TrustedCaptured[0]);

  const Cpp::MemoryEffects& Global = MEs[1];
  ASSERT_TRUE(Global.m_Valid);
  EXPECT_NE(static_cast<unsigned>(Global.m_TrustedOther) &
                static_cast<unsigned>(ModRef::Write),
            0u);

  const Cpp::MemoryEffects& Indirect = MEs[2];
  ASSERT_TRUE(Indirect.m_Valid);
  EXPECT_TRUE(Mentions(Indirect, "<indirect>"));
  EXPECT_EQ(Indirect.m_TrustedOther, ModRef::None);
  EXPECT_FALSE(Indirect.m_TrustedCaptured[1]);

  const Cpp::MemoryEffects& None = MEs[3];
  ASSERT_TRUE(None.m_Valid);
  EXPECT_TRUE(None.m_Opaque.empty());
  EXPECT_EQ(None.m_TrustedOther, None.m_Other);
  EXPECT_EQ(None.m_TrustedCaptured, None.m_Captured);
}

// The trusted parameters follow the signature of each opaque call.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TrustedParamsFollowSignatures) {
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
  ASSERT_TRUE(Const.m_Valid);
  EXPECT_EQ(Const.m_Params[0], ModRef::ReadWrite);
  EXPECT_EQ(Const.m_TrustedParams[0], ModRef::Read);
  EXPECT_EQ(Const.m_TrustedParams[1], ModRef::Read);
  EXPECT_EQ(Const.m_TrustedOther, ModRef::None);
  EXPECT_TRUE(Const.m_Untrusted.empty());

  const Cpp::MemoryEffects& Ref = MEs[1];
  ASSERT_TRUE(Ref.m_Valid);
  EXPECT_NE(static_cast<unsigned>(Ref.m_TrustedParams[0]) &
                static_cast<unsigned>(ModRef::Write),
            0u);
  EXPECT_TRUE(Ref.m_Untrusted.empty());

  const Cpp::MemoryEffects& Methods = MEs[2];
  ASSERT_TRUE(Methods.m_Valid);
  EXPECT_EQ(Methods.m_TrustedParams[0], ModRef::Read);
  EXPECT_NE(static_cast<unsigned>(Methods.m_TrustedParams[1]) &
                static_cast<unsigned>(ModRef::Write),
            0u);
  EXPECT_TRUE(Methods.m_Untrusted.empty());

  const Cpp::MemoryEffects& Unmapped = MEs[3];
  ASSERT_TRUE(Unmapped.m_Valid);
  ASSERT_EQ(Unmapped.m_Untrusted.size(), 1u);
  EXPECT_NE(Unmapped.m_Untrusted[0].find("by_pair"), std::string::npos);
  EXPECT_NE(static_cast<unsigned>(Unmapped.m_TrustedParams[1]) &
                static_cast<unsigned>(ModRef::Write),
            0u);
}
