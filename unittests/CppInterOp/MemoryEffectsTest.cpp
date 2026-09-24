#include "Utils.h"

#include "CppInterOp/CppInterOp.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/ModRef.h"

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
           MemoryEffects_HandlePointeeWriteIsOtherMemory) {
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
  // The handle itself does not change; its pointee does.
  EXPECT_EQ(ME.m_Params[0], ModRef::Read);
  EXPECT_EQ(ME.m_Other, ModRef::ReadWrite);
  EXPECT_TRUE(ME.m_Opaque.empty());
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

namespace {
std::string SummaryHeader() {
  return "# cppinterop-memory-effects v1 llvm " +
         std::to_string(LLVM_VERSION_MAJOR) + "\n";
}
} // namespace

// A summary from a build-time table stands in for a body the module lacks.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_SummaryMakesADeclarationPrecise) {
  TestFixture::CreateInterpreter();
  std::string Effects = llvm::utohexstr(
      llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref).toIntValue());
  std::string Table =
      SummaryHeader() + "_Z10me_summaryRKi\t" + Effects + "\trc\n";
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
  EXPECT_EQ(Cpp::LoadMemoryEffectsSummaries(
                "# cppinterop-memory-effects v1 llvm 0\nf\t0\t\n"),
            -1);
  EXPECT_EQ(
      Cpp::LoadMemoryEffectsSummaries((SummaryHeader() + "f\tzz\t\n").c_str()),
      -1);
  EXPECT_EQ(
      Cpp::LoadMemoryEffectsSummaries((SummaryHeader() + "f\t0\tr\n").c_str()),
      -1);
  EXPECT_EQ(Cpp::LoadMemoryEffectsSummaries(SummaryHeader().c_str()), 0);
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
