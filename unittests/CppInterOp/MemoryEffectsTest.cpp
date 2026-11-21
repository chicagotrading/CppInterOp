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

// A pointer that code loads from a constant global points where the
// initializer of that global says.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_PointerFromAConstantGlobal) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_cg_a = 0, me_cg_b = 0, me_cg_c = 0, me_cg_d = 0;
    inline long* const me_cg_tbl[4] = {&me_cg_a, &me_cg_b, &me_cg_c, &me_cg_d};
    struct me_cg_entry { int k; long* p; };
    inline const me_cg_entry me_cg_entries[4] = {
        {1, &me_cg_a}, {2, &me_cg_b}, {3, &me_cg_c}, {4, &me_cg_d}};
    extern long* const me_cg_ext;
    extern "C" void me_cg_table(const int* i) { ++*me_cg_tbl[*i & 3]; }
    extern "C" long me_cg_table_read(const int* i) { return *me_cg_tbl[*i & 3]; }
    extern "C" void me_cg_struct(const int* i) { ++*me_cg_entries[*i & 3].p; }
    extern "C" void me_cg_local(const int* i) {
      static long s0, s1, s2, s3;
      static long* const t[4] = {&s0, &s1, &s2, &s3};
      ++*t[*i & 3];
    }
    extern "C" void me_cg_plain() { ++me_cg_a; }
    extern "C" void me_cg_extern() { ++*me_cg_ext; }
    extern "C" void me_cg_switch(const int* i) {
      switch (*i & 3) {
      case 0: ++me_cg_a; break;
      case 1: ++me_cg_b; break;
      case 2: ++me_cg_c; break;
      default: ++me_cg_d; break;
      }
    }
    extern "C" int me_cg_numbers(const int* i) {
      static const int t[4] = {3, 1, 4, 1};
      return t[*i & 3];
    }
  )",
      {"me_cg_table", "me_cg_table_read", "me_cg_struct", "me_cg_local",
       "me_cg_plain", "me_cg_extern", "me_cg_switch", "me_cg_numbers"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 8u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_EQ(MEs[0].Other, ModRef::ReadWrite);
  EXPECT_EQ(MEs[1].Other, ModRef::Read);
  EXPECT_EQ(MEs[2].Other, ModRef::ReadWrite);
  EXPECT_EQ(MEs[3].Other, ModRef::ReadWrite);
  EXPECT_EQ(MEs[4].Other, ModRef::ReadWrite);
  EXPECT_EQ(MEs[5].Other, ModRef::ReadWrite);
  EXPECT_EQ(MEs[6].Other, ModRef::ReadWrite);
  // A table of numbers is constant memory.
  EXPECT_EQ(MEs[7].Other, ModRef::None);
  for (const Cpp::MemoryEffects& ME : MEs)
    EXPECT_FALSE(
        Writes(ME.ParamsDirect.empty() ? ModRef::None : ME.ParamsDirect[0]));
}

// A pointer into a constant table, or an address in a constant integer,
// keeps its content known through a callee and a local object.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ConstantGlobalThroughACallee) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_cc_a = 0, me_cc_b = 0, me_cc_c = 0, me_cc_d = 0;
    inline long* const me_cc_tbl[4] = {&me_cc_a, &me_cc_b, &me_cc_c, &me_cc_d};
    struct me_cc_view { long* const* t; };
    __attribute__((noinline)) void me_cc_bump(long* const* t, int i) {
      ++*t[i & 3];
    }
    __attribute__((noinline)) long* const* me_cc_get() { return me_cc_tbl; }
    __attribute__((noinline)) void me_cc_bump_view(const me_cc_view* v, int i) {
      ++*v->t[i & 3];
    }
    extern "C" void me_cc_call(const int* i) { me_cc_bump(me_cc_tbl, *i); }
    extern "C" void me_cc_result(const int* i) { ++*me_cc_get()[*i & 3]; }
    extern "C" void me_cc_object(const int* i) {
      me_cc_view v{me_cc_tbl};
      me_cc_bump_view(&v, *i);
    }
    struct me_cc_ref { long* p; };
    __attribute__((noinline)) void me_cc_bump_ref(const me_cc_ref* r) {
      ++*r->p;
    }
    extern "C" void me_cc_global() {
      me_cc_ref r{&me_cc_a};
      me_cc_bump_ref(&r);
    }
  )",
      {"me_cc_call", "me_cc_result", "me_cc_object", "me_cc_global"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 4u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(Writes(ME.Other));
  }
}

// Only a declaration of type data counts as constant by its name: a
// definition with such a name is inspected as any other global.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TypeDataNameIsNotEnough) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    extern "C" { long _ZTVme_tn_a = 0, _ZTVme_tn_b = 0, _ZTVme_tn_c = 0; }
    inline long* const me_tn_tbl[4] = {&_ZTVme_tn_a, &_ZTVme_tn_b,
                                       &_ZTVme_tn_c, &_ZTVme_tn_a};
    extern "C" void me_tn_name(const int* i) { ++*me_tn_tbl[*i & 3]; }
    inline long me_tn_e = 0, me_tn_f = 0, me_tn_g = 0, me_tn_h = 0;
    extern long* const me_tn_asm_tbl[4] asm("_ZTSme_tn_table");
    long* const me_tn_asm_tbl[4] = {&me_tn_e, &me_tn_f, &me_tn_g, &me_tn_h};
    extern "C" void me_tn_asm(const int* i) { ++*me_tn_asm_tbl[*i & 3]; }
    inline long* const me_tn_in0[2] = {&me_tn_e, &me_tn_f};
    inline long* const me_tn_in1[2] = {&me_tn_g, &me_tn_h};
    inline long* const* const me_tn_out[2] = {me_tn_in0, me_tn_in1};
    extern "C" void me_tn_chain(const int* i) {
      ++*me_tn_out[*i & 1][(*i >> 1) & 1];
    }
  )",
      {"me_tn_name", "me_tn_asm", "me_tn_chain"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 3u);
  for (size_t I = 0; I < MEs.size(); ++I) {
    ASSERT_TRUE(MEs[I].Valid);
    EXPECT_TRUE(Writes(MEs[I].Other)) << I;
  }
}

// A vtable and the type information it points to are constant data of the
// C++ runtime, so an object that holds a vtable pointer stays local.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_VtablePointerIsConstantData) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("expects the Itanium names of vtables and type information");
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_vt_base {
      virtual ~me_vt_base() {}
      virtual int f() const { return 1; }
      long* q = nullptr;
    };
    struct me_vt_derived : me_vt_base {
      int f() const override { return 2; }
    };
    __attribute__((noinline)) long me_vt_read(const me_vt_base* b) {
      return *b->q;
    }
    extern "C" long me_vt_local(const long* x) {
      long v = *x;
      me_vt_derived d;
      d.q = &v;
      return me_vt_read(&d);
    }
  )",
      {"me_vt_local"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(MEs[0].Other, ModRef::None);
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

namespace {
const char* ModRefName(ModRef MR) {
  switch (MR) {
  case ModRef::None:
    return "None";
  case ModRef::Read:
    return "Read";
  case ModRef::Write:
    return "Write";
  case ModRef::ReadWrite:
    return "ReadWrite";
  }
  return "?";
}

std::string Describe(const Cpp::FunctionSignature& S) {
  std::string Out = S.Known ? "known " : "unknown ";
  Out += S.BaseName + "(";
  for (Cpp::ParamKind K : S.Params)
    Out += std::to_string(static_cast<int>(K)) + " ";
  Out += ")";
  Out += S.ConstMethod ? " const" : "";
  Out += S.Variadic ? " variadic" : "";
  return Out;
}

// Every field, so that a mismatch shows in the failure message.
std::string Describe(const Cpp::MemoryEffects& ME) {
  std::string Out = ME.Valid ? "valid\n" : "invalid\n";
  auto ModRefs = [&](const char* Name, const std::vector<ModRef>& V) {
    Out += Name;
    for (ModRef MR : V)
      Out += std::string(" ") + ModRefName(MR);
    Out += "\n";
  };
  auto Bools = [&](const char* Name, const std::vector<bool>& V) {
    Out += Name;
    for (bool B : V)
      Out += B ? " 1" : " 0";
    Out += "\n";
  };
  Out += std::string("ArgMem ") + ModRefName(ME.ArgMem) + "\n";
  Out += std::string("Other ") + ModRefName(ME.Other) + "\n";
  ModRefs("Params", ME.Params);
  ModRefs("ParamsDirect", ME.ParamsDirect);
  ModRefs("ParamsReachable", ME.ParamsReachable);
  Bools("Captured", ME.Captured);
  Bools("AddressCaptured", ME.AddressCaptured);
  Bools("PointerCaptured", ME.PointerCaptured);
  Out += std::string("TrustedOther ") + ModRefName(ME.TrustedOther) + "\n";
  ModRefs("TrustedParams", ME.TrustedParams);
  ModRefs("TrustedParamsDirect", ME.TrustedParamsDirect);
  ModRefs("TrustedParamsReachable", ME.TrustedParamsReachable);
  Bools("TrustedCaptured", ME.TrustedCaptured);
  Bools("TrustedAddressCaptured", ME.TrustedAddressCaptured);
  Bools("TrustedPointerCaptured", ME.TrustedPointerCaptured);
  for (const Cpp::OpaqueCall& O : ME.Opaque)
    Out += "opaque " + std::to_string(static_cast<int>(O.Kind)) + " " +
           O.Mangled + " | " + O.Demangled + " | " + Describe(O.Signature) +
           (O.Bounded ? " bounded\n" : "\n");
  for (const Cpp::MemoryEffects::Callee& C : ME.Callees)
    Out += "callee " + C.Mangled + " | " + C.Demangled + " | " +
           Describe(C.Signature) + "\n";
  return Out;
}

std::vector<std::string> Describe(const std::vector<Cpp::MemoryEffects>& MEs) {
  std::vector<std::string> Out;
  for (const Cpp::MemoryEffects& ME : MEs)
    Out.push_back(Describe(ME));
  return Out;
}
} // namespace

// A unit that cannot link still has optimized IR: the analysis without a
// run gives its effects, where the run fails.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AnalyseOnlyNeedsNoDefinition) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  const char* Code = R"(
    extern "C" void me_unlinked(int* p);
    extern "C" int me_unlinked_seed();
    int me_unlinked_seeded = me_unlinked_seed();
    extern "C" int me_calls_unlinked(int* p, int* q) {
      me_unlinked(p);
      return *q;
    }
  )";
  const size_t Before = Cpp::GetInputCount();
  // Without the in-process link check, the failed link breaks later inputs.
  if (!TypeParam::isOutOfProcess) {
    std::vector<Cpp::MemoryEffects> Run = Cpp::GetFunctionsMemoryEffects(
        Code, {"me_calls_unlinked"}, /*rewind=*/true);
    ASSERT_EQ(Run.size(), 1u);
    EXPECT_FALSE(Run[0].Valid);
    EXPECT_EQ(Cpp::GetInputCount(), Before);
  }

  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::AnalyzeFunctionsMemoryEffects(Code, {"me_calls_unlinked"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(MEs[0].Other, ModRef::ReadWrite);
  ASSERT_EQ(MEs[0].Params.size(), 2u);
  EXPECT_EQ(MEs[0].Params[0], ModRef::ReadWrite);
  EXPECT_TRUE(MEs[0].Captured[0]);
  EXPECT_TRUE(Mentions(MEs[0], "me_unlinked")) << OpaqueNames(MEs[0]);
  EXPECT_EQ(MEs[0].TrustedOther, ModRef::None);
  EXPECT_EQ(MEs[0].TrustedParams[1], ModRef::Read);
  ASSERT_EQ(MEs[0].Callees.size(), 1u);
  EXPECT_EQ(MEs[0].Callees[0].Mangled, "me_unlinked");
  EXPECT_EQ(Cpp::GetInputCount(), Before);
}

// The analysis without a run withdraws the unit: its names are free again,
// and the units before it stay.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AnalyseOnlyLeavesNoState) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("namespace me_ao_before_ns { int kept = 3; }"), 0);
  const size_t Before = Cpp::GetInputCount();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    namespace me_ao_ns {
    struct Box { int v; };
    inline int twice(const Box& b) { return 2 * b.v; }
    extern "C" int me_ao_defined(Box* b) { return twice(*b); }
    }
  )",
      {"me_ao_defined"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(MEs[0].Params[0], ModRef::Read);
  EXPECT_EQ(Cpp::GetInputCount(), Before);
  EXPECT_FALSE(Cpp::GetNamed("me_ao_ns"));
  EXPECT_FALSE(Cpp::GetFunctionAddress("me_ao_defined"));
  EXPECT_TRUE(Cpp::GetNamed("me_ao_before_ns"));

  ASSERT_EQ(Cpp::Declare(R"(
    namespace me_ao_ns {
    struct Box { int v; int w; };
    inline int twice(const Box& b) { return 2 * b.w; }
    extern "C" int me_ao_defined(Box* b) { return twice(*b) + 1; }
    }
  )"),
            0);
  void* Addr = Cpp::GetFunctionAddress("me_ao_defined");
  ASSERT_NE(Addr, nullptr);
  ASSERT_EQ(Cpp::Undo(Cpp::GetInputCount() - Before), 0);
  ASSERT_EQ(Cpp::Undo(1), 0);
  EXPECT_FALSE(Cpp::GetNamed("me_ao_before_ns"));
}

// The analysis without a run does not run the static initializers.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AnalyseOnlyRunsNoInitializer) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  if (TypeParam::isOutOfProcess)
    GTEST_SKIP() << "Evaluate does not work with the OOP JIT";
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("int me_init_runs = 0;"), 0);
  const char* Code = R"(
    struct MeCountsInit { MeCountsInit() { ++me_init_runs; } };
    MeCountsInit me_counts_init;
    me_init_runs += 10;
    extern "C" int me_init_reader(int* p) { return *p; }
  )";
  std::vector<Cpp::MemoryEffects> MEs =
      Cpp::AnalyzeFunctionsMemoryEffects(Code, {"me_init_reader"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(Cpp::Evaluate("me_init_runs").unbox<int>(), 0);

  // The run does run them, so the counter shows a run.
  std::vector<Cpp::MemoryEffects> Run =
      Cpp::GetFunctionsMemoryEffects(Code, {"me_init_reader"}, /*rewind=*/true);
  ASSERT_TRUE(Run[0].Valid);
  EXPECT_EQ(Cpp::Evaluate("me_init_runs").unbox<int>(), 11);
}

// A withdrawn unit that enters a standard header first leaves a session
// that can declare, look up and analyse again.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_WithdrawnUnitEntersAHeader) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  const char* Code = R"(
    #include <stdexcept>
    extern "C" int me_hdr_check(int* p) {
      if (!p)
        throw std::runtime_error("null");
      return *p;
    }
  )";
  // From C++17 on, the runtime unit declares std first.
  for (const char* Std : {"-std=c++14", "-std=c++17", "-std=c++20"})
    for (bool Run : {true, false}) {
      SCOPED_TRACE(std::string(Std) +
                   (Run ? " GetFunctionsMemoryEffects" : " Analyze"));
      TestFixture::CreateInterpreter({Std});
      std::vector<Cpp::MemoryEffects> MEs =
          Run ? Cpp::GetFunctionsMemoryEffects(Code, {"me_hdr_check"},
                                               /*rewind=*/true)
              : Cpp::AnalyzeFunctionsMemoryEffects(Code, {"me_hdr_check"});
      ASSERT_EQ(MEs.size(), 1u);
      ASSERT_TRUE(MEs[0].Valid);
      EXPECT_EQ(MEs[0].Params[0], ModRef::Read);
      EXPECT_FALSE(Cpp::GetFunctionAddress("me_hdr_check"));
      EXPECT_FALSE(Cpp::GetNamed("runtime_error", Cpp::GetNamed("std")));

      ASSERT_EQ(Cpp::Declare(R"(
      #include <stdexcept>
      extern "C" int me_hdr_later(int x) { return x + 1; }
    )"),
                0);
      EXPECT_TRUE(Cpp::GetFunctionAddress("me_hdr_later"));
      Cpp::DeclRef Std = Cpp::GetNamed("std");
      ASSERT_TRUE(Std);
      EXPECT_TRUE(Cpp::GetNamed("runtime_error", Std));

      std::vector<Cpp::MemoryEffects> Again =
          Run ? Cpp::GetFunctionsMemoryEffects(Code, {"me_hdr_check"},
                                               /*rewind=*/true)
              : Cpp::AnalyzeFunctionsMemoryEffects(Code, {"me_hdr_check"});
      ASSERT_EQ(Again.size(), 1u);
      ASSERT_TRUE(Again[0].Valid);
      EXPECT_EQ(Again[0].Params[0], ModRef::Read);
    }
}

// An out-of-line member definition is lexically in the withdrawn unit; a
// local extern in its body must not stay in the namespace lookup.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_WithdrawnOutOfLineLocalExtern) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare(R"(
    namespace me_ool_ns { struct C { int f(); }; }
  )"),
            0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    int me_ool_ns::C::f() { extern int me_ool_leaked; return me_ool_leaked; }
    extern "C" int me_ool_probe(int* p) { return *p; }
  )",
      {"me_ool_probe"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(Cpp::Declare("namespace me_ool_ns { double me_ool_leaked; }"), 0);
}

// Kept declarations whose semantic context differs from the lexical one stay.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_WithdrawKeepsSemanticMembers) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare(R"(
    namespace me_sem_ns {
    struct C { int f(); friend int me_sem_friend(C); };
    int me_sem_friend(C);
    }
    int me_sem_ns::C::f() { extern int me_sem_kept_extern; return 1; }
  )"),
            0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    namespace me_sem_ns { int me_sem_probe_value = 1; }
    extern "C" int me_sem_probe(int* p) { return *p; }
  )",
      {"me_sem_probe"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(Cpp::Declare(R"(
    int me_sem_kept_extern = 3;
    int me_sem_use(me_sem_ns::C c) { return me_sem_friend(c) + c.f(); }
  )"),
            0);
  EXPECT_NE(Cpp::Declare("namespace me_sem_ns { double me_sem_friend; }",
                         /*silent=*/true),
            0);
}

// C++20 caches the comparison category types of <compare>.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_WithdrawnUnitEntersCompare) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter({"-std=c++20"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    #include <compare>
    extern "C" int me_cmp_probe(int* p) { return (*p <=> 3) < 0; }
  )",
      {"me_cmp_probe"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(Cpp::Declare(R"(
    #include <compare>
    extern "C" int me_cmp_later(int x) { return (x <=> 3) < 0; }
  )"),
            0);
  EXPECT_TRUE(Cpp::GetFunctionAddress("me_cmp_later"));
}

// Disabled: the withdrawn anonymous namespace stays the anonymous namespace
// of me_an_ns, so a later one is not visible.
TYPED_TEST(CPPINTEROP_TEST_MODE, DISABLED_MemoryEffects_WithdrawnAnonymousNs) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("namespace me_an_ns { int kept = 1; }"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    namespace me_an_ns { namespace { int old_v = 1; } }
    extern "C" int me_an_probe(int* p) { return *p; }
  )",
      {"me_an_probe"});
  ASSERT_TRUE(MEs[0].Valid);
  ASSERT_EQ(Cpp::Declare("namespace me_an_ns { namespace { int new_v = 2; } }"),
            0);
  EXPECT_EQ(Cpp::Declare("int me_an_use = me_an_ns::new_v;"), 0);
}

// Disabled: the withdrawn explicit specialization stays registered in the kept
// template.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           DISABLED_MemoryEffects_WithdrawnExplicitSpecialization) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("template <class T> struct MeSpecC { int v = 1; };"),
            0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    template <> struct MeSpecC<int> { int v = 2; };
    extern "C" int me_spec_probe(int* p) { return *p; }
  )",
      {"me_spec_probe"});
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(Cpp::Declare("template <> struct MeSpecC<int> { int v = 3; };"), 0);
}

// Disabled: a kept forward declaration of a class keeps the definition data of
// the withdrawn definition.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           DISABLED_MemoryEffects_WithdrawnDefinitionOfKeptClass) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("namespace std { class bad_alloc; }"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    namespace std { class bad_alloc { public: int x; }; }
    extern "C" int me_fwd_probe(int* p) { return *p; }
  )",
      {"me_fwd_probe"});
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(
      Cpp::Declare("namespace std { class bad_alloc { public: int y; }; }"), 0);
}

// A unit that does not compile gives invalid effects and leaves nothing:
// no input, no name, no macro. The error comes first: clang-repl keeps the
// IR of the definitions before it (llvm/llvm-project#226285).
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AnalyseOnlyCompileError) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  const size_t Before = Cpp::GetInputCount();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    #define ME_AO_BROKEN_MACRO 1
    extern "C" int me_ao_broken(int* p) { return *p + me_ao_undeclared; }
    namespace me_ao_broken_ns { int kept = 1; }
    extern "C" int me_ao_fine(int* p) { return *p; }
  )",
      {"me_ao_broken", "me_ao_fine"});
  ASSERT_EQ(MEs.size(), 2u);
  EXPECT_FALSE(MEs[0].Valid);
  EXPECT_FALSE(MEs[1].Valid);
  EXPECT_EQ(Cpp::GetInputCount(), Before);
#if LLVM_VERSION_MAJOR >= 22
  // The rollback of names and macros of a failed parse needs LLVM 22.
  EXPECT_FALSE(Cpp::GetNamed("me_ao_broken_ns"));
  EXPECT_EQ(Cpp::Declare(R"(
    #ifdef ME_AO_BROKEN_MACRO
    #error the macro of the failed unit stays
    #endif
  )"),
            0);
#endif
  EXPECT_NE(Cpp::Declare("int me_ao_use = me_ao_broken_ns::kept;",
                         /*silent=*/true),
            0);
  EXPECT_EQ(Cpp::Declare(R"(
    namespace me_ao_broken_ns { int kept = 2; }
    extern "C" int me_ao_fine(int* p) { return *p; }
  )"),
            0);
}

// For a unit that links, the analysis without a run gives the same effects
// and callees as the analysis that runs the unit.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AnalyseOnlyEqualsRun) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  // Each unit runs twice. A header that a withdrawn unit enters first does
  // not parse again, and an instantiation of a template that a withdrawn
  // unit defines is not emitted again.
  ASSERT_EQ(Cpp::Declare(R"(
    #include <cstring>
    template <class T> T me_eq_thrice(T x) { return x + x + x; }
  )"),
            0);
  struct Unit {
    std::string Code;
    std::vector<std::string> Names;
  };
  std::vector<Unit> Units = {
      {R"(
    struct Box { int v; };
    inline int twice(const Box& b) { return 2 * b.v; }
    extern "C" int me_eq_pure(Box* b) { return twice(*b); }
    extern "C" void me_eq_bump(int* a, int* b) { *a += 1; (void)*b; }
  )",
       {"me_eq_pure", "me_eq_bump", "me_eq_absent"}},
      {R"(
    extern "C" unsigned long me_eq_len(const char* s, char* d) {
      std::memcpy(d, s, 4);
      return std::strlen(s);
    }
  )",
       {"me_eq_len"}},
      {R"(
    namespace me_eq_ns {
    struct Box { int v; Box(int x) : v(x) {} ~Box() {} };
    inline int pick(const Box& b) { return b.v; }
    inline int pick(Box& b) { return -b.v; }
    extern "C" int me_eq_callees(const Box* b, int* x) {
      return pick(*b) + pick(Box(*x)) + me_eq_thrice(*x);
    }
    }
  )",
       {"me_eq_callees"}},
      {R"(
    int* me_eq_global = nullptr;
    extern "C" void me_eq_escape(int* p, int** q) { me_eq_global = p; *q = p; }
  )",
       {"me_eq_escape"}},
      {R"(
    extern "C" int me_eq_seed() { return 3; }
    int me_eq_seeded = me_eq_seed();
    extern "C" int me_eq_reads_seeded(int* p) { return *p + me_eq_seeded; }
  )",
       {"me_eq_reads_seeded"}},
  };
#if !defined(_WIN32) && !defined(EMSCRIPTEN)
  // The native-TLS rewrite changes the module that the analysis sees.
  if (!TypeParam::isOutOfProcess)
    Units.push_back({R"(
    extern "C" __thread int me_native_tls;
    extern "C" int me_eq_reads_tls(int* p) { return *p + me_native_tls; }
  )",
                     {"me_eq_reads_tls"}});
#endif
  for (const Unit& U : Units) {
    SCOPED_TRACE(U.Names.front());
    const size_t Before = Cpp::GetInputCount();
    std::vector<Cpp::MemoryEffects> Analysed =
        Cpp::AnalyzeFunctionsMemoryEffects(U.Code.c_str(), U.Names);
    EXPECT_EQ(Cpp::GetInputCount(), Before);
    std::vector<Cpp::MemoryEffects> Run = Cpp::GetFunctionsMemoryEffects(
        U.Code.c_str(), U.Names, /*rewind=*/true);
    EXPECT_EQ(Cpp::GetInputCount(), Before);
    ASSERT_TRUE(Run.front().Valid);
    EXPECT_EQ(Describe(Analysed), Describe(Run));
  }
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

  // Untrusted, the callee may store any pointer into the allocation.
  // Trusted, it stores only pointers into the memory that its argument
  // reaches: the allocation, which holds a pointer into *t.
  const Cpp::MemoryEffects& Filled = MEs[1];
  ASSERT_TRUE(Filled.Valid);
  EXPECT_TRUE(Mentions(Filled, "me_fill"));
  EXPECT_TRUE(Writes(Filled.Other));
  EXPECT_TRUE(Writes(Filled.TrustedParamsDirect[0]) ||
              Writes(Filled.TrustedParamsReachable[0]))
      << Describe(Filled);
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

// A sort of a local copy is recursive; it writes only the copy.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SortedCopyOfAMember) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("libc++ inlines its throws around out-of-line exception "
                 "constructors, which are opaque calls");
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("#include <algorithm>\n#include <vector>"), 0);
  Cpp::MemoryEffects ME = Cpp::GetFunctionMemoryEffects(R"(
    struct me_sc_window {
      double q = 0.5;
      unsigned long n = 0;
      std::vector<double> init;
      double median() const {
        if (n == 0)
          return 0.0;
        std::vector<double> v = init;
        std::sort(v.begin(), v.end());
        return v[static_cast<unsigned long>(q * v.size())];
      }
    };
    extern "C" double me_sorted_copy(const me_sc_window* w) {
      return w->median();
    }
  )",
                                                        "me_sorted_copy");
  ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(ME.Opaque.empty()) << OpaqueNames(ME);
  EXPECT_EQ(ME.Other, ModRef::None);
  EXPECT_FALSE(Writes(ME.ParamsDirect[0]));
  EXPECT_FALSE(Writes(ME.ParamsReachable[0]));
}

// A recursive call has the effects of its callee at any depth.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RecursiveCalls) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    long me_rc_g = 0;
    long* me_rc_kept = nullptr;
    __attribute__((noinline)) void me_rc_count(const int* n) {
      if (*n > 0) {
        int m = *n - 1;
        me_rc_count(&m);
      } else {
        ++me_rc_g;
      }
    }
    __attribute__((noinline)) void me_rc_keep(long* p, const int* n) {
      if (*n > 0) {
        int m = *n - 1;
        me_rc_keep(p, &m);
      } else {
        me_rc_kept = p;
      }
    }
    __attribute__((noinline)) void me_rc_odd(long* p, const int* n);
    __attribute__((noinline)) void me_rc_even(long* p, const int* n) {
      if (*n > 0) {
        int m = *n - 1;
        me_rc_odd(p, &m);
      }
    }
    __attribute__((noinline)) void me_rc_odd(long* p, const int* n) {
      if (*n > 0) {
        int m = *n - 1;
        me_rc_even(p, &m);
      } else {
        *p = 1;
      }
    }
    __attribute__((noinline)) long me_rc_sum(const long* p, const int* n) {
      if (*n <= 0)
        return 0;
      int m = *n - 1;
      return p[m] + me_rc_sum(p, &m);
    }
    extern "C" void me_rc_count_top(const int* n) { me_rc_count(n); }
    extern "C" void me_rc_keep_top(long* p, const int* n) { me_rc_keep(p, n); }
    extern "C" void me_rc_even_top(long* p, const int* n) { me_rc_even(p, n); }
    extern "C" long me_rc_sum_top(const long* p, const int* n) {
      return me_rc_sum(p, n);
    }
  )",
      {"me_rc_count_top", "me_rc_keep_top", "me_rc_even_top", "me_rc_sum_top"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 4u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(Writes(MEs[0].Other));
  EXPECT_TRUE(MEs[1].Captured[0]);
  EXPECT_TRUE(Writes(MEs[2].ParamsDirect[0]));
  EXPECT_FALSE(MEs[2].Captured[0]);
  EXPECT_EQ(MEs[3].Other, ModRef::None);
  EXPECT_EQ(MEs[3].ParamsDirect[0], ModRef::Read);
  EXPECT_FALSE(MEs[3].Captured[0]);
}

// A recursive call that returns a pointer: a global or another argument
// reaches the result only after more than one round of the fixpoint.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RecursivePointerResults) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_rp_g = 0;
    __attribute__((noinline)) long* me_rp_pick(long* p, int d) {
      if (d <= 0)
        return p;
      long* a = me_rp_pick(&me_rp_g, d - 1);
      long* b = me_rp_pick(p, d - 2);
      return (d & 1) ? a : b;
    }
    extern "C" void me_rp_ret(long* p, int d) { *me_rp_pick(p, d) = 1; }
    __attribute__((noinline)) long* me_rp_rot4(long* a0, long* a1, long* a2,
                                               long* a3, int d) {
      if (d <= 0)
        return a0;
      long* x = me_rp_rot4(a1, a2, a3, a0, d - 1);
      long* y = me_rp_rot4(a1, a2, a3, a0, d - 2);
      return (d & 1) ? x : y;
    }
    extern "C" void me_rp_rot4_top(long* a0, long* a1, long* a2, long* a3,
                                   int d) {
      *me_rp_rot4(a0, a1, a2, a3, d) = 1;
    }
    __attribute__((noinline)) long* me_rp_rot10(long* a0, long* a1, long* a2,
        long* a3, long* a4, long* a5, long* a6, long* a7, long* a8,
        long* a9, int d) {
      if (d <= 0)
        return a0;
      long* x = me_rp_rot10(a1, a2, a3, a4, a5, a6, a7, a8, a9, a0, d - 1);
      long* y = me_rp_rot10(a1, a2, a3, a4, a5, a6, a7, a8, a9, a0, d - 2);
      return (d & 1) ? x : y;
    }
    extern "C" void me_rp_rot10_top(long* a0, long* a1, long* a2, long* a3,
        long* a4, long* a5, long* a6, long* a7, long* a8, long* a9, int d) {
      *me_rp_rot10(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, d) = 1;
    }
    // The global reaches the result only at depth 2.
    __attribute__((noinline)) long* me_rp_b(long* p, int d);
    __attribute__((noinline)) long* me_rp_a(long* p, int d) {
      if (d <= 0)
        return p;
      long* x = me_rp_b(p, d - 1);
      long* y = me_rp_b(p, d - 2);
      return (d & 1) ? x : y;
    }
    __attribute__((noinline)) long* me_rp_b(long* p, int d) {
      if (d <= 0)
        return p;
      long* x = me_rp_a(&me_rp_g, d - 1);
      long* y = me_rp_a(p, d - 2);
      return (d & 1) ? x : y;
    }
    extern "C" void me_rp_b_top(long* p, int d) { *me_rp_b(p, d) = 1; }
    extern "C" void me_rp_a_top(long* p, int d) { *me_rp_a(p, d) = 1; }
  )",
      {"me_rp_ret", "me_rp_rot4_top", "me_rp_rot10_top", "me_rp_b_top",
       "me_rp_a_top"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 5u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(Writes(MEs[0].Other));
  for (int K = 0; K < 4; ++K)
    EXPECT_TRUE(Writes(MEs[1].ParamsDirect[K]) || Writes(MEs[1].Other)) << K;
  for (int K = 0; K < 10; ++K)
    EXPECT_TRUE(Writes(MEs[2].ParamsDirect[K]) || Writes(MEs[2].Other)) << K;
  EXPECT_TRUE(Writes(MEs[3].Other));
  EXPECT_TRUE(Writes(MEs[4].Other));
}

// The difference of two addresses is an address when one of them is null,
// so a callee that stores it may store a pointer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_DifferenceWithANullBase) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  ASSERT_EQ(Cpp::Declare("#include <cstring>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_nb_range { long* b; long* e; };
    struct me_nb_out { long n; long* p; };
    __attribute__((noinline)) void me_nb_diff(me_nb_out* o,
                                              const me_nb_range* r) {
      o->n = reinterpret_cast<long>(r->e) - reinterpret_cast<long>(r->b);
    }
    // With r->b == nullptr, o.n is the address r->e.
    extern "C" void me_nb_write(const me_nb_range* r) {
      me_nb_out o{0, nullptr};
      me_nb_diff(&o, r);
      long* p;
      std::memcpy(&p, &o.n, sizeof p);
      *p = 1;
    }
  )",
      {"me_nb_write"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].Other) || Writes(MEs[0].ParamsReachable[0]));
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

namespace {
// The fields of the row of \p Name in a summary table; empty when absent.
std::vector<std::string> RowOf(const std::string& Table, llvm::StringRef Name) {
  llvm::SmallVector<llvm::StringRef, 8> Lines;
  llvm::StringRef(Table).split(Lines, '\n', -1, false);
  for (llvm::StringRef Line : Lines) {
    llvm::SmallVector<llvm::StringRef, 20> Fields;
    Line.split(Fields, '\t');
    if (Fields[0] == Name)
      return {Fields.begin(), Fields.end()};
  }
  return {};
}
} // namespace

// A callee that copies only characters out of a parameter's memory keeps no
// pointer to it: what the callee stores there stays known to the caller.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_DataCaptureKeepsStoresOwn) {
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  // InstCombine makes an unmarked load and store of a copy of 8 bytes or less.
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    declare noalias ptr @_Znwm(i64)
    declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
    define linkonce_odr ptr @_ZNSt11char_traitsIcE4copyEPcPKcm(ptr %d, ptr %s,
                                                               i64 %n) {
      call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
      ret ptr %d
    }
    define void @me_fill_chars(ptr %p, ptr %out) noinline {
      %buf = load ptr, ptr %p
      call ptr @_ZNSt11char_traitsIcE4copyEPcPKcm(ptr %out, ptr %buf, i64 32)
      %n = call ptr @_Znwm(i64 8)
      store ptr %n, ptr %p
      ret void
    }
    define void @me_fill_pointer(ptr %p, ptr %out) noinline {
      %buf = load ptr, ptr %p
      store ptr %buf, ptr %out
      %n = call ptr @_Znwm(i64 8)
      store ptr %n, ptr %p
      ret void
    }
    define void @me_use_chars(ptr %out) {
      %x = alloca ptr
      %b = call ptr @_Znwm(i64 8)
      store ptr %b, ptr %x
      call void @me_fill_chars(ptr %x, ptr %out)
      %y = load ptr, ptr %x
      store i8 1, ptr %y
      ret void
    }
    define void @me_use_pointer(ptr %out) {
      %x = alloca ptr
      %b = call ptr @_Znwm(i64 8)
      store ptr %b, ptr %x
      call void @me_fill_pointer(ptr %x, ptr %out)
      %y = load ptr, ptr %x
      store i8 1, ptr %y
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  std::vector<std::string> Chars = RowOf(Table, "me_fill_chars");
  ASSERT_EQ(Chars.size(), 9u) << Table;
  EXPECT_EQ(Chars[5][0], 'i');
  std::vector<std::string> Pointer = RowOf(Table, "me_fill_pointer");
  ASSERT_EQ(Pointer.size(), 9u) << Table;
  EXPECT_EQ(Pointer[5][0], 'r');
  // The write goes into the fresh memory that the callee stored into x.
  std::vector<std::string> UseChars = RowOf(Table, "me_use_chars");
  ASSERT_EQ(UseChars.size(), 9u) << Table;
  EXPECT_EQ(UseChars[6], "n");
  // Through the kept pointer, x may hold any address.
  std::vector<std::string> UsePointer = RowOf(Table, "me_use_pointer");
  ASSERT_EQ(UsePointer.size(), 9u) << Table;
  EXPECT_EQ(UsePointer[6], "w");
}

// A wrapper that forwards its result to a callee keeps the callee's "stores
// own" result when the callee keeps only data of the other argument.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ForwardedResultStoresOwn) {
  std::string Effects =
      llvm::utohexstr(llvm::MemoryEffects::unknown().toIntValue());
  auto Get = [&](const std::string& Name, char Capture) {
    return Name + "\t" + Effects + "\t-.rc\twr\tnr\ts" + Capture +
           "\tn\t...\t.\n";
  };
  ASSERT_EQ(CppInternal::LoadMemoryEffectsSummaries(SummaryHeader() +
                                                    Get("me_get_data", 'i') +
                                                    Get("me_get_pointer", 'r')),
            2);
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    declare void @me_get_data(ptr, ptr)
    declare void @me_get_pointer(ptr, ptr)
    define void @me_wrap_data(ptr %r, ptr %this) {
      %m = load ptr, ptr %this
      call void @me_get_data(ptr %r, ptr %m)
      ret void
    }
    define void @me_wrap_pointer(ptr %r, ptr %this) {
      %m = load ptr, ptr %this
      call void @me_get_pointer(ptr %r, ptr %m)
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  std::vector<std::string> Data = RowOf(Table, "me_wrap_data");
  ASSERT_EQ(Data.size(), 9u) << Table;
  EXPECT_EQ(Data[5], "si");
  // The callee may store a pointer that it loads through m into the result.
  std::vector<std::string> Pointer = RowOf(Table, "me_wrap_pointer");
  ASSERT_EQ(Pointer.size(), 9u) << Table;
  EXPECT_EQ(Pointer[5], ".r");
}

// A forwarded integer that the callee only compares or uses as an index
// cannot become a pointer in the result. One that the callee keeps or makes
// a pointer can.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ForwardedIntegerArgument) {
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    declare noalias ptr @_Znwm(i64)
    @me_kept_id = global i64 0
    define void @me_get_id(ptr %r, ptr %this, i64 %id) noinline {
      %h = urem i64 %id, 7
      %slot = getelementptr i64, ptr %this, i64 %h
      %k = load i64, ptr %slot
      %hit = icmp eq i64 %k, %id
      %n = call ptr @_Znwm(i64 8)
      %v = select i1 %hit, ptr %n, ptr null
      store ptr %v, ptr %r
      ret void
    }
    define void @me_get_keep(ptr %r, ptr %this, i64 %id) noinline {
      store i64 %id, ptr @me_kept_id
      %n = call ptr @_Znwm(i64 8)
      store ptr %n, ptr %r
      ret void
    }
    define void @me_get_ptr(ptr %r, ptr %this, i64 %id) noinline {
      %p = inttoptr i64 %id to ptr
      store ptr %p, ptr %r
      ret void
    }
    define void @me_wrap_id(ptr %r, ptr %this, i64 %id) {
      %m = load ptr, ptr %this
      call void @me_get_id(ptr %r, ptr %m, i64 %id)
      ret void
    }
    define void @me_wrap_keep(ptr %r, ptr %this, i64 %id) {
      %m = load ptr, ptr %this
      call void @me_get_keep(ptr %r, ptr %m, i64 %id)
      ret void
    }
    define void @me_wrap_ptr(ptr %r, ptr %this, i64 %id) {
      %m = load ptr, ptr %this
      call void @me_get_ptr(ptr %r, ptr %m, i64 %id)
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  // The capture of the result and of the integer.
  auto Capture = [&](llvm::StringRef Name) {
    std::vector<std::string> Row = RowOf(Table, Name);
    return Row.size() == 9u ? std::string{Row[5][0], Row[5][2]} : Table;
  };
  EXPECT_EQ(Capture("me_get_id"), "o.");
  EXPECT_EQ(Capture("me_wrap_id"), "o.");
  EXPECT_EQ(Capture("me_get_keep"), "oc");
  EXPECT_EQ(Capture("me_wrap_keep"), ".c");
  EXPECT_EQ(Capture("me_get_ptr"), ".c");
  EXPECT_EQ(Capture("me_wrap_ptr"), ".c");
}

// Under trust, an opaque call keeps no copy of an integer argument, as of a
// pointer argument, except in its result.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TrustedCallKeepsNoInteger) {
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    declare noalias ptr @_Znwm(i64)
    declare void @_Z7me_sinkl(i64)
    declare i64 @_Z7me_hashl(i64)
    @me_kept_hash = global i64 0
    define void @me_get_logged(ptr %r, ptr %this, i64 %id) noinline {
      call void @_Z7me_sinkl(i64 %id)
      %n = call ptr @_Znwm(i64 8)
      store ptr %n, ptr %r
      ret void
    }
    define void @me_get_hashed(ptr %r, ptr %this, i64 %id) noinline {
      %h = call i64 @_Z7me_hashl(i64 %id)
      store i64 %h, ptr @me_kept_hash
      %n = call ptr @_Znwm(i64 8)
      store ptr %n, ptr %r
      ret void
    }
    define void @me_wrap_logged(ptr %r, ptr %this, i64 %id) {
      %m = load ptr, ptr %this
      call void @me_get_logged(ptr %r, ptr %m, i64 %id)
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  // The capture of the result and of the integer, untrusted then trusted.
  auto Capture = [&](llvm::StringRef Name) {
    std::vector<std::string> Row = RowOf(Table, Name);
    if (Row.size() < 14u || Row[8] != "i")
      return Table;
    return std::string{Row[5][0], Row[5][2], ' ', Row[13][0], Row[13][2]};
  };
  EXPECT_EQ(Capture("me_get_logged"), "oc o.");
  EXPECT_EQ(Capture("me_wrap_logged"), ".c o.");
  // The hash may carry the integer into memory.
  EXPECT_EQ(Capture("me_get_hashed"), "oc oc");
}

// An integer in a stack slot that only goes by reference to a trusted call
// stays in the function. A slot whose address leaves does not.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IntegerInAPrivateSlot) {
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    declare noalias ptr @_Znwm(i64)
    declare void @_Z6me_logRKl(ptr)
    @me_slot_ref = global ptr null
    define void @me_get_spilled(ptr %r, ptr %this, i64 %id) noinline {
      %slot = alloca i64
      store i64 %id, ptr %slot
      call void @_Z6me_logRKl(ptr %slot)
      %n = call ptr @_Znwm(i64 8)
      store ptr %n, ptr %r
      ret void
    }
    define void @me_get_leaked(ptr %r, ptr %this, i64 %id) noinline {
      %slot = alloca i64
      store i64 %id, ptr %slot
      store ptr %slot, ptr @me_slot_ref
      call void @_Z6me_logRKl(ptr %slot)
      %n = call ptr @_Znwm(i64 8)
      store ptr %n, ptr %r
      ret void
    }
    define void @me_wrap_spilled(ptr %r, ptr %this, i64 %id) {
      %m = load ptr, ptr %this
      call void @me_get_spilled(ptr %r, ptr %m, i64 %id)
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  // The capture of the result and of the integer, untrusted then trusted.
  auto Capture = [&](llvm::StringRef Name) {
    std::vector<std::string> Row = RowOf(Table, Name);
    if (Row.size() < 14u || Row[8] != "i")
      return Table;
    return std::string{Row[5][0], Row[5][2], ' ', Row[13][0], Row[13][2]};
  };
  EXPECT_EQ(Capture("me_get_spilled"), ".c o.");
  EXPECT_EQ(Capture("me_wrap_spilled"), ".c o.");
  EXPECT_EQ(Capture("me_get_leaked"), ".c .c");
}

// A callee reads through two levels of local objects to a global.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ReadThroughNestedObjects) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_nr_g = 0;
    struct me_nr_in { long* p; };
    struct me_nr_out { me_nr_in* in; };
    __attribute__((noinline)) long me_nr_read(const me_nr_out* o) {
      return *o->in->p;
    }
    extern "C" long me_nr_nested() {
      me_nr_in i{&me_nr_g};
      me_nr_out o{&i};
      return me_nr_read(&o);
    }
    inline long* const me_nr_tbl[2] = {&me_nr_g, &me_nr_g};
    struct me_nr_cin { long* const* t; };
    struct me_nr_cout { me_nr_cin* in; };
    __attribute__((noinline)) long me_nr_cread(const me_nr_cout* o, int k) {
      return *o->in->t[k & 1];
    }
    extern "C" long me_nr_const(const int* k) {
      me_nr_cin i{me_nr_tbl};
      me_nr_cout o{&i};
      return me_nr_cread(&o, *k);
    }
  )",
      {"me_nr_nested", "me_nr_const"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_NE(ME.Other, ModRef::None);
  }
}

// A callee returns a pointer that it loads through two levels.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ResultLoadedThroughTwoLevels) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_rt_g = 0;
    inline long* const me_rt_c = &me_rt_g;
    __attribute__((noinline)) long* me_rt_follow(long* const* const* box) {
      return **box;
    }
    extern "C" void me_rt_const() {
      long* const* b = &me_rt_c;
      ++*me_rt_follow(&b);
    }
    __attribute__((noinline)) long* me_rt_follow2(long** const* box) {
      return **box;
    }
    extern "C" void me_rt_local() {
      long* p = &me_rt_g;
      long** q = &p;
      ++*me_rt_follow2(&q);
    }
  )",
      {"me_rt_const", "me_rt_local"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(Writes(ME.Other));
  }
}

// A callee stores its argument through a local box that leads to a global.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CaptureThroughABox) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline void* me_cb_slot = nullptr;
    struct me_cb_box { void** where; };
    __attribute__((noinline)) void me_cb_put(const me_cb_box* b, void* p) {
      *b->where = p;
    }
    extern "C" void me_cb_leak(void* p) {
      me_cb_box b{&me_cb_slot};
      me_cb_put(&b, p);
    }
  )",
      {"me_cb_leak"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(MEs[0].Captured[0]);
}

// A callee stores the address of a global into *p; the caller writes **p.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_GlobalStoredIntoAParameter) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_gs_g = 0;
    __attribute__((noinline)) void me_gs_set(long** p) { *p = &me_gs_g; }
    extern "C" void me_gs_write(long** p) {
      me_gs_set(p);
      **p = 1;
    }
  )",
      {"me_gs_write"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].Other));
}

// Three levels of local objects, read and written by a callee.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ThreeLevels) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_t3_g = 0;
    struct me_t3_a { long* p; };
    struct me_t3_b { me_t3_a* a; };
    struct me_t3_c { me_t3_b* b; };
    __attribute__((noinline)) long me_t3_get(const me_t3_c* c) {
      return *c->b->a->p;
    }
    __attribute__((noinline)) void me_t3_put(const me_t3_c* c) {
      *c->b->a->p = 1;
    }
    extern "C" long me_t3_read() {
      me_t3_a a{&me_t3_g};
      me_t3_b b{&a};
      me_t3_c c{&b};
      return me_t3_get(&c);
    }
    extern "C" void me_t3_write() {
      me_t3_a a{&me_t3_g};
      me_t3_b b{&a};
      me_t3_c c{&b};
      me_t3_put(&c);
    }
    extern "C" long me_t3_local() {
      long v = 2;
      me_t3_a a{&v};
      me_t3_b b{&a};
      me_t3_c c{&b};
      return me_t3_get(&c);
    }
  )",
      {"me_t3_read", "me_t3_write", "me_t3_local"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 3u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_NE(MEs[0].Other, ModRef::None);
  EXPECT_TRUE(Writes(MEs[1].Other));
  // Only stack memory: nothing for the caller to see.
  EXPECT_EQ(MEs[2].Other, ModRef::None);
}

// An aggregate by value that holds a pointer to an object that holds a
// pointer to a global.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AggregateAndPointerLevels) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_ap_g = 0;
    struct me_ap_in { long* p; };
    struct me_ap_mid { me_ap_in* in; long n; };
    struct me_ap_out { me_ap_mid m; long k; };
    __attribute__((noinline)) long me_ap_byval(me_ap_mid m) {
      return *m.in->p + m.n;
    }
    __attribute__((noinline)) long me_ap_byref(const me_ap_out* o) {
      return *o->m.in->p;
    }
    extern "C" long me_ap_value() {
      me_ap_in i{&me_ap_g};
      return me_ap_byval({&i, 1});
    }
    extern "C" long me_ap_member() {
      me_ap_in i{&me_ap_g};
      me_ap_out o{{&i, 1}, 2};
      return me_ap_byref(&o);
    }
  )",
      {"me_ap_value", "me_ap_member"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_NE(ME.Other, ModRef::None);
  }
}

// A level that a callee returns as fresh memory may hold any pointer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_LevelFromAReturnedPointer) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_lr_g = 0;
    struct me_lr_in { long* p; };
    struct me_lr_out { me_lr_in* in; };
    __attribute__((noinline)) me_lr_in* me_lr_make() {
      return new me_lr_in{&me_lr_g};
    }
    __attribute__((noinline)) long me_lr_get(const me_lr_out* o) {
      return *o->in->p;
    }
    __attribute__((noinline)) long me_lr_get1(const me_lr_in* i) {
      return *i->p;
    }
    extern "C" long me_lr_nested() {
      me_lr_out o{me_lr_make()};
      long r = me_lr_get(&o);
      delete o.in;
      return r;
    }
    extern "C" long me_lr_direct() {
      me_lr_in* i = me_lr_make();
      long r = me_lr_get1(i);
      delete i;
      return r;
    }
    extern "C" void me_lr_param(me_lr_out* o) {
      o->in = me_lr_make();
      *o->in->p = 1;
    }
  )",
      {"me_lr_nested", "me_lr_direct", "me_lr_param"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 3u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_NE(MEs[0].Other, ModRef::None);
  EXPECT_NE(MEs[1].Other, ModRef::None);
  EXPECT_TRUE(Writes(MEs[2].Other));
}

// A callee stores a global at one level; the caller accesses another level.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_StoreAndAccessAtDifferentLevels) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_sl_g = 0;
    struct me_sl_in { long* p; };
    struct me_sl_box { me_sl_in* in; };
    __attribute__((noinline)) void me_sl_set(const me_sl_box* b) {
      b->in->p = &me_sl_g;
    }
    extern "C" void me_sl_param(me_sl_box* b) {
      me_sl_set(b);
      *b->in->p = 1;
    }
    extern "C" void me_sl_local(long* q) {
      me_sl_in i{q};
      me_sl_box b{&i};
      me_sl_set(&b);
      *i.p = 1;
    }
    __attribute__((noinline)) void me_sl_set_in(me_sl_in* i) {
      i->p = &me_sl_g;
    }
    extern "C" long me_sl_outer(me_sl_box* b) {
      me_sl_set_in(b->in);
      return *b->in->p;
    }
  )",
      {"me_sl_param", "me_sl_local", "me_sl_outer"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 3u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(Writes(MEs[0].Other));
  EXPECT_TRUE(Writes(MEs[1].Other));
  EXPECT_NE(MEs[2].Other, ModRef::None);
}

// A pointer that code loads from a global, stored into a parameter's memory
// or used as the target of a store.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_PointerLoadedFromAGlobal) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long* me_pg_slot = nullptr;
    inline long** me_pg_where = &me_pg_slot;
    __attribute__((noinline)) void me_pg_fetch(long** p) {
      *p = *me_pg_where;
    }
    extern "C" void me_pg_write(long** p) {
      me_pg_fetch(p);
      **p = 1;
    }
    struct me_pg_box { long** where; };
    __attribute__((noinline)) void me_pg_put(const me_pg_box* b, long* p) {
      *b->where = p;
    }
    extern "C" void me_pg_leak(long* p) {
      me_pg_box b{me_pg_where};
      me_pg_put(&b, p);
    }
  )",
      {"me_pg_write", "me_pg_leak"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(Writes(MEs[0].Other));
  EXPECT_TRUE(MEs[1].Captured[0]);
}

// Recursive callees that access or store two levels down.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_RecursionThroughTwoLevels) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_r2_g = 0;
    struct me_r2_in { long* p; };
    struct me_r2_box { me_r2_in* in; };
    __attribute__((noinline)) long me_r2_get(const me_r2_box* b, int d) {
      return d <= 0 ? *b->in->p : me_r2_get(b, d - 1) + 1;
    }
    extern "C" long me_r2_read(int d) {
      me_r2_in i{&me_r2_g};
      me_r2_box b{&i};
      return me_r2_get(&b, d);
    }
    __attribute__((noinline)) void me_r2_set(const me_r2_box* b, int d) {
      if (d <= 0) {
        b->in->p = &me_r2_g;
        return;
      }
      me_r2_set(b, d - 1);
    }
    extern "C" void me_r2_write(me_r2_box* b, int d) {
      me_r2_set(b, d);
      *b->in->p = 1;
    }
  )",
      {"me_r2_read", "me_r2_write"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_NE(MEs[0].Other, ModRef::None);
  EXPECT_TRUE(Writes(MEs[1].Other));
}

// Objects that point to themselves or to each other: the closure ends.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CyclicHoldings) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_cy_g = 0;
    struct me_cy_node { me_cy_node* next; long* v; };
    __attribute__((noinline)) long me_cy_walk(const me_cy_node* n) {
      return *n->next->next->next->v;
    }
    extern "C" long me_cy_self() {
      me_cy_node a{nullptr, &me_cy_g};
      a.next = &a;
      return me_cy_walk(&a);
    }
    extern "C" long me_cy_pair() {
      long x = 1;
      me_cy_node a{nullptr, &x};
      me_cy_node b{&a, &me_cy_g};
      a.next = &b;
      return me_cy_walk(&a);
    }
    extern "C" long me_cy_local() {
      long x = 1, y = 2;
      me_cy_node a{nullptr, &x};
      me_cy_node b{&a, &y};
      a.next = &b;
      return me_cy_walk(&a);
    }
    extern "C" long me_cy_param(me_cy_node* p) {
      me_cy_node a{p, &me_cy_g};
      p->next = &a;
      return me_cy_walk(p);
    }
  )",
      {"me_cy_self", "me_cy_pair", "me_cy_local", "me_cy_param"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 4u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_NE(MEs[0].Other, ModRef::None);
  EXPECT_NE(MEs[1].Other, ModRef::None);
  EXPECT_EQ(MEs[2].Other, ModRef::None);
  EXPECT_NE(MEs[3].Other, ModRef::None);
}

// A captured parameter's memory may receive a pointer to a global through
// the memory that holds the capture.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ForeignPointerThroughACapture) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_fc_g = 0;
    __attribute__((noinline)) void me_fc_redirect(long*** q) {
      **q = &me_fc_g;
    }
    extern "C" long me_fc_read(long*** q, long** p) {
      *q = p;
      me_fc_redirect(q);
      return **p;
    }
  )",
      {"me_fc_read"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(MEs[0].Captured[1]);
  EXPECT_NE(MEs[0].Other, ModRef::None) << Describe(MEs[0]);
}

// Integer data loaded from a global holds no address.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IntegerDataIsNoAddress) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_xc1_g = 0;
    struct me_xc1_s { long n; long* p; };
    extern "C" void me_xc1_f(me_xc1_s* s) {
      s->n = me_xc1_g;
      *s->p = 1;
    }
  )",
      {"me_xc1_f"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_NE(MEs[0].Other, ModRef::None);
  // Precision: only a read of the global.
  EXPECT_FALSE(Writes(MEs[0].Other)) << Describe(MEs[0]);
}

// A callee stores a global address as an integer into *p.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressStoredAsAnInteger) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_xia_g1 = 0, me_xia_g2 = 0;
    __attribute__((noinline)) void me_xia_c(unsigned long* p) {
      *p = (unsigned long)&me_xia_g1;
    }
    extern "C" void me_xia_const(long** p) {
      me_xia_c((unsigned long*)p);
      **p = 1;
    }
    __attribute__((noinline)) void me_xia_s(unsigned long* p, int c) {
      *p = (unsigned long)(c ? &me_xia_g1 : &me_xia_g2);
    }
    extern "C" void me_xia_select(long** p, int c) {
      me_xia_s((unsigned long*)p, c);
      **p = 1;
    }
    __attribute__((noinline)) void me_xia_l(unsigned long* p, int n) {
      long* q = &me_xia_g1;
      for (int i = 0; i < n; ++i)
        q = (i & 1) ? &me_xia_g2 : q;
      *p = (unsigned long)q;
    }
    extern "C" void me_xia_loop(long** p, int n) {
      me_xia_l((unsigned long*)p, n);
      **p = 1;
    }
    union me_xia_u { long* p; unsigned long i; };
    __attribute__((noinline)) void me_xia_pun(me_xia_u* u) {
      u->i = (unsigned long)&me_xia_g1;
    }
    extern "C" void me_xia_union(me_xia_u* u) {
      me_xia_pun(u);
      *u->p = 1;
    }
  )",
      {"me_xia_const", "me_xia_select", "me_xia_loop", "me_xia_union"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 4u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(Writes(ME.Other)) << Describe(ME);
  }
}

// Addresses that integers carry through selects, loads and arithmetic.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressArithmeticInIntegers) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_xim_g1 = 0, me_xim_g2 = 0;
    inline long* me_xim_gp = &me_xim_g1;
    __attribute__((noinline)) void me_xim_s(unsigned long* p, long* q, int c) {
      *p = (unsigned long)(c ? &me_xim_g1 : q);
    }
    extern "C" void me_xim_mixed(long** p, long* q, int c) {
      me_xim_s((unsigned long*)p, q, c);
      **p = 1;
    }
    __attribute__((noinline)) void me_xim_i(unsigned long* p, int c) {
      unsigned long a = (unsigned long)&me_xim_g1;
      unsigned long b = (unsigned long)&me_xim_g2;
      *p = c ? a : b;
    }
    extern "C" void me_xim_ints(long** p, int c) {
      me_xim_i((unsigned long*)p, c);
      **p = 1;
    }
    __attribute__((noinline)) void me_xim_l(unsigned long* p) {
      *p = (unsigned long)me_xim_gp;
    }
    extern "C" void me_xim_load(long** p) {
      me_xim_l((unsigned long*)p);
      **p = 1;
    }
    __attribute__((noinline)) void me_xim_o(unsigned long* p, int c) {
      *p = (unsigned long)(c ? &me_xim_g1 : &me_xim_g2) + 8;
    }
    extern "C" void me_xim_offset(long** p, int c) {
      me_xim_o((unsigned long*)p, c);
      *(*p - 1) = 1;
    }
    extern "C" void me_xim_inline(long** p, int c) {
      *(unsigned long*)p = (unsigned long)(c ? &me_xim_g1 : &me_xim_g2);
      **p = 1;
    }
  )",
      {"me_xim_mixed", "me_xim_ints", "me_xim_load", "me_xim_offset",
       "me_xim_inline"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 5u);
  for (size_t K = 0; K < MEs.size(); ++K) {
    ASSERT_TRUE(MEs[K].Valid);
    EXPECT_TRUE(Writes(MEs[K].Other)) << K << "\n" << Describe(MEs[K]);
  }
}

// An integer argument of unknown source, copied as bytes into a pointer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IntegerArgumentCopiedAsBytes) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    __attribute__((noinline)) void me_xc2_install(int** dst,
                                                  unsigned long bits) {
      __builtin_memcpy(dst, &bits, 8);
    }
    extern "C" void me_xc2_c(int** dst, unsigned long bits) {
      me_xc2_install(dst, bits);
      **dst = 1;
    }
  )",
      {"me_xc2_c"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
}

// A pointer loaded from a global, stored as an integer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IntegerFromAGlobalPointer) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_xc3_g = 0;
    inline long* me_xc3_gp = &me_xc3_g;
    __attribute__((noinline)) void me_xc3_set(unsigned long* p) {
      *p = (unsigned long)me_xc3_gp;
    }
    extern "C" void me_xc3_c() {
      long* slot;
      me_xc3_set((unsigned long*)&slot);
      *slot = 1;
    }
  )",
      {"me_xc3_c"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
}

// A copy of an object that holds a pointer, done as an integer copy.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_PointerCopiedAsAnInteger) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  ASSERT_EQ(Cpp::Declare("#include <vector>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_xpc_g = 0;
    struct me_xpc_in { long* p; };
    struct me_xpc_box { me_xpc_in* in; };
    __attribute__((noinline)) void me_xpc_use(const me_xpc_box* b) {
      *b->in->p = 1;
    }
    __attribute__((noinline)) void me_xpc_usev(const std::vector<me_xpc_box>* v) {
      *(*v)[0].in->p = 1;
    }
    extern "C" void me_xpc_copy() {
      me_xpc_in i{&me_xpc_g};
      me_xpc_box t{&i};
      me_xpc_box* b = new me_xpc_box(t);
      me_xpc_use(b);
      delete b;
    }
    extern "C" void me_xpc_param(long* q) {
      me_xpc_in i{q};
      me_xpc_box t{&i};
      me_xpc_box* b = new me_xpc_box(t);
      me_xpc_use(b);
      delete b;
    }
    extern "C" void me_xpc_vector() {
      me_xpc_in i{&me_xpc_g};
      std::vector<me_xpc_box> v;
      v.push_back(me_xpc_box{&i});
      me_xpc_usev(&v);
    }
    extern "C" void me_xpc_vparam(long* q) {
      me_xpc_in i{q};
      std::vector<me_xpc_box> v;
      v.push_back(me_xpc_box{&i});
      me_xpc_usev(&v);
    }
    extern "C" void me_xpc_direct(me_xpc_box* out, long* q) {
      me_xpc_in* i = new me_xpc_in{q};
      me_xpc_box t{i};
      *out = t;
      me_xpc_use(out);
    }
  )",
      {"me_xpc_copy", "me_xpc_param", "me_xpc_vector", "me_xpc_vparam"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 4u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
  EXPECT_TRUE(Writes(MEs[1].ParamsDirect[0]) || Writes(MEs[1].Other))
      << Describe(MEs[1]);
  EXPECT_TRUE(Writes(MEs[2].Other)) << Describe(MEs[2]);
  EXPECT_TRUE(Writes(MEs[3].ParamsDirect[0]) || Writes(MEs[3].Other))
      << Describe(MEs[3]);
}

// Levels behind a smart pointer and a heap box.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SmartPointerLevels) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  ASSERT_EQ(Cpp::Declare("#include <memory>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_xup_g = 0;
    struct me_xup_in { long* p; };
    struct me_xup_box { me_xup_in* in; };
    __attribute__((noinline)) void me_xup_use(const std::unique_ptr<me_xup_box>* u) {
      *(*u)->in->p = 1;
    }
    __attribute__((noinline)) void me_xup_raw(const me_xup_box* b) {
      *b->in->p = 1;
    }
    struct me_xup_w {
      me_xup_box* p;
      ~me_xup_w() { delete p; }
    };
    __attribute__((noinline)) void me_xup_wuse(const me_xup_w* w) {
      *w->p->in->p = 1;
    }
    extern "C" void me_xup_make() {
      me_xup_in i{&me_xup_g};
      auto u = std::make_unique<me_xup_box>(me_xup_box{&i});
      me_xup_use(&u);
    }
    extern "C" void me_xup_newu() {
      me_xup_in i{&me_xup_g};
      std::unique_ptr<me_xup_box> u(new me_xup_box{&i});
      me_xup_use(&u);
    }
    extern "C" void me_xup_get() {
      me_xup_in i{&me_xup_g};
      auto u = std::make_unique<me_xup_box>(me_xup_box{&i});
      me_xup_raw(u.get());
    }
    extern "C" void me_xup_wrap() {
      me_xup_in i{&me_xup_g};
      me_xup_w w{new me_xup_box{&i}};
      me_xup_wuse(&w);
    }
    extern "C" void me_xup_wrapnodtor() {
      me_xup_in i{&me_xup_g};
      me_xup_box* b = new me_xup_box{&i};
      me_xup_box* const* pb = &b;
      me_xup_wuse(reinterpret_cast<const me_xup_w*>(pb));
      delete b;
    }
  )",
      {"me_xup_make", "me_xup_newu", "me_xup_get", "me_xup_wrap",
       "me_xup_wrapnodtor"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 5u);
  for (size_t K = 0; K < MEs.size(); ++K) {
    ASSERT_TRUE(MEs[K].Valid);
    EXPECT_TRUE(Writes(MEs[K].Other)) << K << "\n" << Describe(MEs[K]);
  }
}

// Lambdas, references, arrays, smart pointers and constructors as levels.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_MixedLevelKinds) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  ASSERT_EQ(Cpp::Declare("#include <memory>\n#include <new>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_xmk_g = 0;
    struct me_xmk_in { long* p; };
    struct me_xmk_box { me_xmk_in* in; };
    template <class F> __attribute__((noinline)) void me_xmk_run(F& f) { f(); }
    extern "C" void me_xmk_lambda() {
      me_xmk_in i{&me_xmk_g};
      me_xmk_box b{&i};
      auto l = [&] { *b.in->p = 1; };
      me_xmk_run(l);
    }
    struct me_xmk_ref { long& r; };
    struct me_xmk_rbox { const me_xmk_ref* ref; };
    __attribute__((noinline)) void me_xmk_rset(const me_xmk_rbox* b) {
      b->ref->r = 1;
    }
    extern "C" void me_xmk_refm() {
      me_xmk_ref r{me_xmk_g};
      me_xmk_rbox b{&r};
      me_xmk_rset(&b);
    }
    struct me_xmk_arr { long* a[4]; };
    __attribute__((noinline)) void me_xmk_aset(me_xmk_arr* a, int k) {
      a->a[k & 3] = &me_xmk_g;
    }
    extern "C" void me_xmk_array(me_xmk_arr* a, int k) {
      me_xmk_aset(a, k);
      *a->a[0] = 1;
    }
    __attribute__((noinline)) void me_xmk_use(const std::unique_ptr<me_xmk_box>* u) {
      *(*u)->in->p = 1;
    }
    extern "C" void me_xmk_uptr() {
      me_xmk_in i{&me_xmk_g};
      auto u = std::make_unique<me_xmk_box>(me_xmk_box{&i});
      me_xmk_use(&u);
    }
    extern "C" void me_xmk_heap() {
      auto* i = new me_xmk_in{&me_xmk_g};
      auto u = std::make_unique<me_xmk_box>(me_xmk_box{i});
      me_xmk_use(&u);
      delete i;
    }
    struct me_xmk_s {
      long* p;
      __attribute__((noinline)) me_xmk_s() : p(&me_xmk_g) {}
    };
    extern "C" void me_xmk_ctor(void* o) {
      me_xmk_s* s = ::new (o) me_xmk_s();
      *s->p = 1;
    }
  )",
      {"me_xmk_lambda", "me_xmk_refm", "me_xmk_array", "me_xmk_uptr",
       "me_xmk_heap", "me_xmk_ctor"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 6u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(Writes(ME.Other)) << Describe(ME);
  }
}

// Integer data that holds an address, copied into a pointer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IntegerDataCarriesAnAddress) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  ASSERT_EQ(Cpp::Declare("#include <cstdint>\n#include <cstring>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_ydl_g = 0;
    struct me_ydl_f { uintptr_t a; };
    inline uintptr_t me_ydl_ga = (uintptr_t)&me_ydl_g;
    inline const uintptr_t me_ydl_tbl[2] = {(uintptr_t)&me_ydl_g,
                                            (uintptr_t)&me_ydl_g};
    __attribute__((noinline)) void me_ydl_fromg(me_ydl_f* f) {
      f->a = me_ydl_ga;
    }
    __attribute__((noinline)) void me_ydl_fromt(me_ydl_f* f, int k) {
      f->a = me_ydl_tbl[k & 1];
    }
    __attribute__((noinline)) uintptr_t me_ydl_rd(const me_ydl_f* s) {
      return s->a;
    }
    __attribute__((noinline)) void me_ydl_st(me_ydl_f* f, const me_ydl_f* s) {
      f->a = me_ydl_rd(s);
    }
    __attribute__((noinline)) void me_ydl_pokem(const me_ydl_f* f) {
      long* q;
      std::memcpy(&q, &f->a, sizeof q);
      *q = 1;
    }
    extern "C" void me_ydl_global(me_ydl_f* f) {
      me_ydl_fromg(f);
      long* q;
      std::memcpy(&q, &f->a, sizeof q);
      *q = 1;
    }
    extern "C" void me_ydl_table(me_ydl_f* f, int k) {
      me_ydl_fromt(f, k);
      long* q;
      std::memcpy(&q, &f->a, sizeof q);
      *q = 1;
    }
    extern "C" void me_ydl_ret(me_ydl_f* f) {
      me_ydl_f s{(uintptr_t)&me_ydl_g};
      f->a = me_ydl_rd(&s);
      long* q;
      std::memcpy(&q, &f->a, sizeof q);
      *q = 1;
    }
    extern "C" void me_ydl_retcallee(me_ydl_f* f) {
      me_ydl_f s{(uintptr_t)&me_ydl_g};
      me_ydl_st(f, &s);
      me_ydl_pokem(f);
    }
    extern "C" void me_ydl_globallocal() {
      me_ydl_f f{0};
      me_ydl_fromg(&f);
      me_ydl_pokem(&f);
    }
    extern "C" void me_ydl_inline(me_ydl_f* f) {
      f->a = me_ydl_ga;
      long* q;
      std::memcpy(&q, &f->a, sizeof q);
      *q = 1;
    }
  )",
      {"me_ydl_global", "me_ydl_table", "me_ydl_ret", "me_ydl_retcallee",
       "me_ydl_globallocal", "me_ydl_inline"},
      /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 6u);
  for (size_t K = 0; K < MEs.size(); ++K) {
    ASSERT_TRUE(MEs[K].Valid);
    EXPECT_TRUE(Writes(MEs[K].Other)) << K << "\n" << Describe(MEs[K]);
  }
}

// An address that an integer member holds, copied with the struct and then
// copied back into a pointer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressInACopiedIntegerMember) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  ASSERT_EQ(Cpp::Declare("#include <cstdint>\n#include <cstring>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_ym_g = 0;
    struct me_ym_s { uintptr_t a; long n; };
    __attribute__((noinline)) void me_ym_copy(me_ym_s* d, const me_ym_s* s) {
      *d = *s;
    }
    extern "C" void me_ym_c(me_ym_s* d) {
      me_ym_s s{(uintptr_t)&me_ym_g, 0};
      me_ym_copy(d, &s);
      long* q;
      std::memcpy(&q, &d->a, sizeof q);
      *q = 1;
    }
  )",
      {"me_ym_c"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
}

// Without optimization: an integer argument stored into an integer member,
// then copied back into a pointer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressInAnIntegerMemberAtO0) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::Declare("#include <cstdint>\n#include <cstring>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_yo_g = 0;
    inline uintptr_t me_yo_ga = (uintptr_t)&me_yo_g;
    struct me_yo_f { uintptr_t a; long* p; };
    void me_yo_set(me_yo_f* f, uintptr_t v) { f->a = v; }
    extern "C" void me_yo_c(me_yo_f* f) {
      me_yo_set(f, me_yo_ga);
      long* q;
      std::memcpy(&q, &f->a, sizeof q);
      *q = 1;
    }
    struct me_yo_s { uintptr_t a; long n; };
    void me_yo_copy(me_yo_s* d, const me_yo_s* s) { *d = *s; }
    extern "C" void me_yo_struct(me_yo_s* d) {
      me_yo_s s{(uintptr_t)&me_yo_g, 0};
      me_yo_copy(d, &s);
      long* q;
      std::memcpy(&q, &d->a, sizeof q);
      *q = 1;
    }
    // Precision: integer data next to a pointer member.
    extern "C" void me_yo_data(me_yo_f* f, uintptr_t v) {
      f->a = v;
      *f->p = 1;
    }
  )",
      {"me_yo_c", "me_yo_struct", "me_yo_data"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 3u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
  EXPECT_TRUE(Writes(MEs[1].Other)) << Describe(MEs[1]);
  EXPECT_EQ(MEs[2].Other, ModRef::None) << Describe(MEs[2]);
}

// A trusted opaque call stores no pointer to other memory into its
// arguments; an analysed callee is not trusted.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TrustCoversStoredPointers) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    inline long me_tc_g = 0;
    void me_tc_fill(long** p);
    extern "C" void me_tc_opaque(long** p) {
      me_tc_fill(p);
      **p = 1;
    }
    __attribute__((noinline)) void me_tc_set(long** p) { *p = &me_tc_g; }
    extern "C" void me_tc_body(long** p) {
      me_tc_set(p);
      **p = 1;
    }
    struct me_tc_v { long a, b, c, d; };
    void me_tc_fillv(long** p, me_tc_v v);
    extern "C" void me_tc_unbounded(long** p) {
      me_tc_fillv(p, {});
      **p = 1;
    }
  )",
      {"me_tc_opaque", "me_tc_body", "me_tc_unbounded"});
  ASSERT_EQ(MEs.size(), 3u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  // Untrusted: the opaque call may store any pointer into *p.
  EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
  // Trusted: the same fields as without the stored-pointer tracking.
  EXPECT_EQ(MEs[0].TrustedOther, ModRef::None) << Describe(MEs[0]);
  EXPECT_EQ(MEs[0].TrustedParamsDirect[0], ModRef::ReadWrite);
  EXPECT_EQ(MEs[0].TrustedParamsReachable[0], ModRef::Write);
  EXPECT_FALSE(MEs[0].TrustedCaptured[0]);
  // The analysed callee stores the global's address in both variants.
  EXPECT_TRUE(Writes(MEs[1].Other)) << Describe(MEs[1]);
  EXPECT_TRUE(Writes(MEs[1].TrustedOther)) << Describe(MEs[1]);
  // A class by value: the signature does not bound the call.
  EXPECT_FALSE(AllBounded(MEs[2])) << OpaqueNames(MEs[2]);
  EXPECT_TRUE(Writes(MEs[2].TrustedOther)) << Describe(MEs[2]);
}

// A local that a trusted call fills holds pointers into the call's argument
// memory or fresh memory, not into other memory.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TrustedCallFillsALocal) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    struct me_tf_l { long* p; };
    void me_tf_init(me_tf_l* l);
    void me_tf_from(me_tf_l* l, long* q);
    struct me_tf_v { long a, b, c, d; };
    void me_tf_initv(me_tf_l* l, me_tf_v v);
    __attribute__((noinline)) void me_tf_touch(long** o) { **o = 1; }
    extern "C" void me_tf_own(long** out) {
      me_tf_l l;
      me_tf_init(&l);
      *out = l.p;
      me_tf_touch(out);
    }
    extern "C" void me_tf_arg(long* q) {
      me_tf_l l;
      me_tf_from(&l, q);
      *l.p = 1;
    }
    extern "C" void me_tf_unbounded(long** out) {
      me_tf_l l;
      me_tf_initv(&l, {});
      *out = l.p;
      me_tf_touch(out);
    }
  )",
      {"me_tf_own", "me_tf_arg", "me_tf_unbounded"});
  ASSERT_EQ(MEs.size(), 3u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  // As without the stored-pointer tracking.
  EXPECT_EQ(MEs[0].TrustedOther, ModRef::None) << Describe(MEs[0]);
  EXPECT_FALSE(MEs[0].TrustedCaptured[0]) << Describe(MEs[0]);
  EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
  // l.p may point into q's memory.
  EXPECT_TRUE(Writes(MEs[1].TrustedParamsDirect[0]) ||
              Writes(MEs[1].TrustedParamsReachable[0]) ||
              Writes(MEs[1].TrustedOther))
      << Describe(MEs[1]);
  EXPECT_TRUE(Writes(MEs[1].Other)) << Describe(MEs[1]);
  // The signature does not bound the call.
  EXPECT_TRUE(Writes(MEs[2].TrustedOther)) << Describe(MEs[2]);
}

// A trusted call may copy pointers from one argument's memory into another
// argument's memory, but it keeps no copy of an argument itself.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TrustedCallFillsAParameter) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    void me_fp_mv(long** r, long* const* qq);
    struct me_fp_in { long* p; };
    struct me_fp_out { me_fp_in* in; };
    void me_fp_deep(me_fp_out* r, const me_fp_out* s);
    // *r = *qq: the write lands in the memory that qq reaches.
    extern "C" void me_fp_pp(long** r, long* const* qq) {
      me_fp_mv(r, qq);
      **r = 1;
    }
    // r->in = s->in: the write lands in the memory that s reaches.
    extern "C" void me_fp_member(me_fp_out* r, const me_fp_out* s) {
      me_fp_deep(r, s);
      *r->in->p = 1;
    }
  )",
      {"me_fp_pp", "me_fp_member"});
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(AllBounded(ME)) << OpaqueNames(ME);
    EXPECT_TRUE(Writes(ME.TrustedParamsReachable[1]) || Writes(ME.TrustedOther))
        << Describe(ME);
    // The pointers of qq's memory now also live in r's memory.
    EXPECT_TRUE(ME.TrustedCaptured[1]) << Describe(ME);
  }
}

// The trusted fill of a fresh allocation, without a store into the table.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_TrustedFillOfFreshMemory) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  TestFixture::CreateInterpreter();
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    struct me_ff_table { int** slots; int own; };
    void me_ff_fill(int** slots);
    extern "C" void me_ff_c(me_ff_table* t) {
      int** fresh = new int*[2];
      fresh[0] = &t->own;
      me_ff_fill(fresh);
      *fresh[1] = 1;
      delete[] fresh;
    }
  )",
      {"me_ff_c"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
  // fresh[1] points into the allocation or into *t, not into other memory.
  // Through &t->own the callee may also reach what *t holds.
  EXPECT_EQ(MEs[0].TrustedOther, ModRef::None) << Describe(MEs[0]);
  EXPECT_TRUE(Writes(MEs[0].TrustedParamsDirect[0])) << Describe(MEs[0]);
  EXPECT_TRUE(Writes(MEs[0].TrustedParamsReachable[0])) << Describe(MEs[0]);
}

// A getter that returns a string and may throw: the exception runtime and a
// trusted string allocation add no effect on the receiver.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ThrowingGetterOfAString) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI(
      "the runtime model knows the Itanium new, delete and throw entry points");
  SKIP_ON_LIBCXX("expects the string layout of libstdc++");
  TestFixture::CreateInterpreter({"-O2", "-std=c++20"});
  ASSERT_EQ(Cpp::Declare("#include <new>\n#include <optional>\n"
                         "#include <stdexcept>\n#include <string>\n"
                         "#include <vector>"),
            0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    struct me_tg_sym {
      std::vector<std::string> m;
      std::optional<std::string> p1(int s, long u) const {
        if (u < 0 || u >= (long)m.size()) throw std::runtime_error("no p1");
        if (s < 0) return std::nullopt;
        return m[u];
      }
      std::string name(long k) const {
        if (k < 0) throw std::runtime_error("neg");
        return m[k];
      }
    };
    struct me_tg_api {
      const me_tg_sym& s;
      std::optional<std::string> get(int a, long u) const { return s.p1(a, u); }
    };
    extern "C" void me_tg_discard(const me_tg_api* r, int* a, long* u) {
      (void)(*r).get(*a, *u);
    }
    extern "C" void me_tg_keep(const me_tg_api* r, int* a, long* u, void* out) {
      ::new (out) std::optional<std::string>((*r).get(*a, *u));
    }
    extern "C" void me_tg_name(const me_tg_sym* c, long* k, void* out) {
      ::new (out) std::string(c->name(*k));
    }
  )",
      {"me_tg_discard", "me_tg_keep", "me_tg_name"});
  ASSERT_EQ(MEs.size(), 3u);
  for (const Cpp::MemoryEffects& ME : MEs)
    ASSERT_TRUE(ME.Valid);
  EXPECT_FALSE(MEs[0].TrustedCaptured[0]) << Describe(MEs[0]);
  EXPECT_EQ(MEs[1].TrustedOther, ModRef::None) << Describe(MEs[1]);
  // A trusted allocation of the result may return unknown memory, which the
  // copy writes; nothing reads other memory.
  EXPECT_FALSE(static_cast<unsigned>(MEs[2].TrustedOther) &
               static_cast<unsigned>(ModRef::Read))
      << Describe(MEs[2]);
  EXPECT_EQ(MEs[2].TrustedParamsReachable[0], ModRef::Read) << Describe(MEs[2]);
}

// A parameter captured into a local holder; a callee stores a global through
// the holder. The capture post-pass must not need the holder's object.
TYPED_TEST(CPPINTEROP_TEST_MODE,
           MemoryEffects_ForeignStoreThroughALocalHolder) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  // Without optimization clang ignores noinline.
  TestFixture::CreateInterpreter({"-O2"});
  std::vector<Cpp::MemoryEffects> MEs = Cpp::GetFunctionsMemoryEffects(
      R"(
    inline long me_lh_g = 0;
    struct me_lh_h { long** p; };
    __attribute__((noinline)) void me_lh_put(const me_lh_h* h) {
      *h->p = &me_lh_g;
    }
    extern "C" void me_lh_c(long** p) {
      me_lh_h h{p};
      me_lh_put(&h);
      **p = 1;
    }
  )",
      {"me_lh_c"}, /*rewind=*/true);
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
  EXPECT_TRUE(Writes(MEs[0].TrustedOther)) << Describe(MEs[0]);
}

// A table entry for a member that writes only an integer of its object. An
// entry without the stored-pointer flags is read conservatively; an entry
// from the current summarizer carries them.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SummaryCarriesForeignStores) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  const std::string Fields =
      llvm::utohexstr(llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef)
                          .toIntValue()) +
      "\t-c-.\tmn\tnn\t..\tn\t...";
  const char* Code = R"(
    struct me_sf_pol {
      float f;
      mutable unsigned long next;
      unsigned long need(unsigned long n) const;
    };
    struct me_sf_tab { long* p; me_sf_pol pol; };
    extern "C" void me_sf_c(me_sf_tab* t, const unsigned long* n) {
      (void)t->pol.need(*n);
      *t->p = 1;
    }
  )";
  for (const char* Flags : {"", "000"}) {
    TestFixture::CreateInterpreter({"-O2"});
    std::string Table =
        SummaryHeader() + "_ZNK9me_sf_pol4needEm\t" + Fields + Flags + "\t.\n";
    ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1) << Flags;
    std::vector<Cpp::MemoryEffects> MEs =
        Cpp::AnalyzeFunctionsMemoryEffects(Code, {"me_sf_c"});
    ASSERT_EQ(MEs.size(), 1u);
    ASSERT_TRUE(MEs[0].Valid);
    EXPECT_TRUE(MEs[0].Opaque.empty()) << OpaqueNames(MEs[0]);
    EXPECT_TRUE(Writes(MEs[0].ParamsReachable[0])) << Describe(MEs[0]);
    if (*Flags)
      EXPECT_EQ(MEs[0].Other, ModRef::None) << Describe(MEs[0]);
    else
      EXPECT_TRUE(Writes(MEs[0].Other)) << Describe(MEs[0]);
  }
}

// The summarizer writes the stored-pointer flags. A model declaration that
// writes its argument stores no foreign pointer only when it says so.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SummaryWritesForeignStores) {
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    declare i64 @me_sw_model(ptr captures(none), i64) memory(argmem: readwrite)
    declare i64 @me_sw_bound(ptr captures(none), i64) memory(argmem: readwrite) "cppinterop-trusted-stores"
    define i64 @me_sw_any(ptr %this, i64 %n) {
      %r = call i64 @me_sw_model(ptr %this, i64 %n)
      ret i64 %r
    }
    define i64 @me_sw_own(ptr %this, i64 %n) {
      %r = call i64 @me_sw_bound(ptr %this, i64 %n)
      ret i64 %r
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  // The result field: kind, two origins, two flag digits, other memory.
  auto Flags = [&](llvm::StringRef Name) {
    std::vector<std::string> Row = RowOf(Table, Name);
    return Row.size() == 9u && Row[7].size() == 6u ? Row[7].substr(3) : Table;
  };
  const std::string Any = Flags("me_sw_any");
  const std::string Own = Flags("me_sw_own");
  ASSERT_EQ(Any.size(), 3u) << Table;
  ASSERT_EQ(Own.size(), 3u) << Table;
  // A base-32 digit; bit 0 says that the parameter may receive one.
  auto Bits = [](char C) {
    return llvm::StringRef("0123456789abcdefghijklmnopqrstuv").find(C);
  };
  EXPECT_TRUE(Bits(Any[0]) & 1) << Table;
  EXPECT_FALSE(Bits(Own[0]) & 1) << Table;
  EXPECT_EQ(Own[2], '0') << Table;
}

// A callee stores its integer argument into heap memory of its object. At
// -O0 no type proves it data; the caller knows the integer it passes.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_IntegerArgumentIntoAWindow) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  for (const char* Opt : {"-O0", "-O2"}) {
    TestFixture::CreateInterpreter({Opt, "-std=c++20"});
    ASSERT_EQ(Cpp::Declare("#include <algorithm>\n#include <cmath>\n"
                           "#include <cstdint>\n#include <limits>\n"
                           "#include <stdexcept>\n#include <vector>"),
              0);
    std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
        R"(
      namespace me_iw_ns {
      template <class T, std::size_t C> struct Ring {
        Ring() { m_buffer.resize(C); }
        std::size_t size() const { return m_write - m_read; }
        bool empty() const { return m_write == m_read; }
        T& operator[](std::size_t i) { return m_buffer[(m_read + i) & (C - 1)]; }
        void push_back(const T& v) {
          m_buffer[m_write & (C - 1)] = v;
          if (++m_write - m_read > C)
            ++m_read;
        }
        T pop_front() {
          if (empty())
            throw std::runtime_error("pop_front on an empty buffer");
          return m_buffer[m_read++ & (C - 1)];
        }
        std::vector<T> m_buffer;
        std::size_t m_write = 0, m_read = 0;
      };
      struct Window {
        struct Rec { std::int64_t t{}; double v{}; };
        void update(std::int64_t now, double value, double ms) {
          if (now > 0 && value != 0) m_buffer.push_back({now, value});
          while (m_buffer.size() > 1 && now - m_buffer[1].t >= ms * 1e6)
            m_buffer.pop_front();
          m_value = m_buffer[0].v;
          m_age = std::clamp(double(now - m_buffer[0].t), 0.0, ms * 1e6);
        }
        Ring<Rec, 1024> m_buffer;
        double m_value{}, m_age{};
      };
      struct State { bool off; double value, age; Window w; };
      }
      extern "C" void me_iw_c(me_iw_ns::State* s, const std::int64_t* now,
                              const double* v, const double* ms) {
        if (s->off) return;
        s->w.update(*now, *v, *ms);
        s->value = s->w.m_value;
        s->age = s->w.m_age;
      }
    )",
        {"me_iw_c"});
    ASSERT_EQ(MEs.size(), 1u);
    ASSERT_TRUE(MEs[0].Valid) << Opt;
    EXPECT_EQ(MEs[0].TrustedOther, ModRef::None) << Opt << Describe(MEs[0]);
    EXPECT_FALSE(MEs[0].TrustedCaptured[0]) << Opt << Describe(MEs[0]);
    EXPECT_TRUE(Writes(MEs[0].TrustedParamsReachable[0])) << Opt;
  }
}

// The out-of-line rehash policy of a hash set, from a table entry with the
// stored-pointer flags: inserting into a set of integers is no foreign write.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_HashSetWithPolicyEntries) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  SKIP_ON_LIBCXX("expects the out-of-line rehash policy of libstdc++");
  const std::string E = llvm::utohexstr(
      llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::ModRef).toIntValue());
  for (const char* Opt : {"-O0", "-O2"}) {
    TestFixture::CreateInterpreter({Opt, "-std=c++20"});
    const std::string Table =
        SummaryHeader() +
        "_ZNKSt8__detail20_Prime_rehash_policy14_M_need_rehashEmmm\t" + E +
        "\t-c-.-.-.\tmnnn\tnnnn\t....\tn\t.....00000\t.\n" +
        "_ZNKSt8__detail20_Prime_rehash_policy11_M_next_bktEm\t" + E +
        "\t-c-.\tmn\tnn\t..\tn\t...000\t.\n";
    ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 2);
    ASSERT_EQ(Cpp::Declare("#include <cstdint>\n#include <unordered_set>"), 0);
    std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
        R"(
      struct me_hs_state { bool off; std::unordered_set<std::uint64_t> ids; };
      extern "C" void me_hs_c(me_hs_state* s, const bool* clear,
                              const std::uint64_t* id) {
        if (s->off) return;
        if (*clear) s->ids.clear();
        s->ids.insert(*id);
      }
    )",
        {"me_hs_c"});
    ASSERT_EQ(MEs.size(), 1u);
    ASSERT_TRUE(MEs[0].Valid) << Opt;
    EXPECT_TRUE(MEs[0].Opaque.empty()) << OpaqueNames(MEs[0]);
    EXPECT_EQ(MEs[0].Other, ModRef::None) << Opt << Describe(MEs[0]);
    EXPECT_FALSE(MEs[0].Captured[0]) << Opt << Describe(MEs[0]);
    EXPECT_TRUE(Writes(MEs[0].ParamsReachable[0])) << Opt;
  }
}

// Two handles: integer data of one handle's memory added into the memory of
// the other is no copy of a pointer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_SumOfTwoHandles) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  for (const char* Opt : {"-O0", "-O2"}) {
    TestFixture::CreateInterpreter({Opt});
    std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
        R"(
      struct me_sh_impl { long v = 0; };
      inline me_sh_impl me_sh_g;
      struct me_sh_handle { me_sh_impl* impl = &me_sh_g; };
      inline long me_sh_poke(const me_sh_handle& a, const me_sh_handle& b) {
        a.impl->v += b.impl->v;
        return a.impl->v;
      }
      extern "C" void me_sh_c(const me_sh_handle* a, const me_sh_handle* b) {
        (void)me_sh_poke(*a, *b);
      }
    )",
        {"me_sh_c"});
    ASSERT_EQ(MEs.size(), 1u);
    ASSERT_TRUE(MEs[0].Valid) << Opt;
    EXPECT_FALSE(MEs[0].Captured[1]) << Opt << Describe(MEs[0]);
    EXPECT_EQ(MEs[0].ParamsReachable[1], ModRef::Read) << Opt;
    EXPECT_TRUE(Writes(MEs[0].ParamsReachable[0])) << Opt;
  }
}

// A table entry of a function that copies integer data of a global into
// the object it returns. The caller frees the buffer of the result.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ResultFilledFromAGlobal) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    %S = type { ptr, i64, i64 }
    @me_rg_g = global %S zeroinitializer
    declare noalias ptr @_Znwm(i64)
    define void @_Z10me_rg_copyv(ptr sret(%S) %r) {
      %np = getelementptr i8, ptr @me_rg_g, i64 8
      %n = load i64, ptr %np, !tbaa !1, !cppinterop.integer !5
      %b = call ptr @_Znwm(i64 %n)
      store ptr %b, ptr %r, !tbaa !4
      %rn = getelementptr i8, ptr %r, i64 8
      store i64 %n, ptr %rn, !tbaa !1, !cppinterop.integer !5
      %rc = getelementptr i8, ptr %r, i64 16
      store i64 %n, ptr %rc, !tbaa !1, !cppinterop.integer !5
      ret void
    }
    !0 = !{!"me root"}
    !2 = !{!"long", !0, i64 0}
    !1 = !{!2, !2, i64 0}
    !3 = !{!"any pointer", !0, i64 0}
    !4 = !{!3, !3, i64 0}
    !5 = !{}
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  // The integer marks are what MarkIntegerLoads gives the -O0 code.
  const std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1) << Table;
  ASSERT_EQ(Cpp::Declare("#include <new>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    struct me_rg_s { char* p; long n; long c; };
    me_rg_s me_rg_copy();
    extern "C" void me_rg_c() {
      me_rg_s s = me_rg_copy();
      ::operator delete(s.p, s.c);
    }
  )",
      {"me_rg_c"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(MEs[0].Opaque.empty()) << OpaqueNames(MEs[0]);
  // The read of the global; the buffer is fresh memory.
  EXPECT_EQ(MEs[0].Other, ModRef::Read) << Table << Describe(MEs[0]);
}

// An old-format entry of a function that writes nothing stores and copies
// nothing.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_OldReadOnlyEntryCopiesNothing) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  TestFixture::CreateInterpreter({"-O2"});
  const std::string E = llvm::utohexstr(
      (llvm::MemoryEffects::argMemOnly(llvm::ModRefInfo::Ref) |
       llvm::MemoryEffects::inaccessibleMemOnly(llvm::ModRefInfo::ModRef))
          .toIntValue());
  const std::string Table = SummaryHeader() + "_Z9me_or_logPKcRKl\t" + E +
                            "\trcrc\trr\trr\t..\tn\t...\t.\n";
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    void me_or_log(const char* fmt, const long& v);
    struct me_or_box { long* p; long n; };
    extern "C" void me_or_c(me_or_box* b, const long* v) {
      b->n = *v;
      me_or_log("{}", b->n);
      *b->p = 1;
    }
  )",
      {"me_or_c"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_EQ(MEs[0].Other, ModRef::None) << Describe(MEs[0]);
  EXPECT_FALSE(MEs[0].Captured[0]) << Describe(MEs[0]);
}

// A string from a table entry, copied by characters into memory that holds
// integer data of a global: the characters are no pointers.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_CharacterCopyIntoDataMemory) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  SKIP_ON_LIBCXX("expects the string layout of libstdc++");
  TestFixture::CreateInterpreter({"-O0", "-std=c++20"});
  const std::string Table =
      SummaryHeader() +
      "_Z10me_cd_makeB5cxx11i\tFF\t-.-.\twn\tnn\too\tn\t...220\t.\n";
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1);
  ASSERT_EQ(Cpp::Declare("#include <string>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    inline long me_cd_g = 0;
    struct me_cd_box { long n; std::string s; };
    std::string me_cd_make(int k);
    me_cd_box me_cd_get(int k) {
      const std::string s = me_cd_make(k);
      return me_cd_box{me_cd_g, s};
    }
    extern "C" void me_cd_c(void* out, const int* k) {
      ::new (out) me_cd_box(me_cd_get(*k));
    }
  )",
      {"me_cd_c"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_FALSE(Writes(MEs[0].TrustedOther)) << Describe(MEs[0]);
}

// A name from a constant table of {pointer, length} views, copied into the
// result: the length that the view holds is no foreign pointer.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_LengthFromAConstantView) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    %S = type { ptr, i64, i64 }
    @me_lv_s0 = constant [2 x i8] c"a\00"
    @me_lv_s1 = constant [3 x i8] c"bc\00"
    @me_lv_names = constant [2 x { i64, ptr }]
        [{ i64, ptr } { i64 1, ptr @me_lv_s0 }, { i64, ptr } { i64 2, ptr @me_lv_s1 }]
    declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
    define internal { i64, ptr } @me_lv_view(i32 %k) noinline {
      %i = zext i32 %k to i64
      %e = getelementptr [2 x { i64, ptr }], ptr @me_lv_names, i64 0, i64 %i
      %v = load { i64, ptr }, ptr %e
      ret { i64, ptr } %v
    }
    define void @_Z10me_lv_namei(ptr sret(%S) %r, i32 %k) {
      %sv = call { i64, ptr } @me_lv_view(i32 %k)
      %n = extractvalue { i64, ptr } %sv, 0
      %p = extractvalue { i64, ptr } %sv, 1
      %buf = getelementptr i8, ptr %r, i64 16
      store ptr %buf, ptr %r
      %len = getelementptr i8, ptr %r, i64 8
      store i64 %n, ptr %len
      call void @llvm.memcpy.p0.p0.i64(ptr %buf, ptr %p, i64 1, i1 false)
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  const std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  std::vector<std::string> Row = RowOf(Table, "_Z10me_lv_namei");
  ASSERT_EQ(Row.size(), 9u) << Table;
  // Kind, two origins, then the flags of the result and of the integer.
  ASSERT_EQ(Row[7].size(), 6u) << Table;
  const size_t Bits =
      llvm::StringRef("0123456789abcdefghijklmnopqrstuv").find(Row[7][3]);
  EXPECT_FALSE(Bits & 1) << Table;
  TestFixture::CreateInterpreter();
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1) << Table;
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    struct me_lv_s { char* p; long n; long c; };
    me_lv_s me_lv_name(int k);
    extern "C" void me_lv_c(const int* k, char* out) {
      me_lv_s s = me_lv_name(*k);
      *out = *s.p;
    }
  )",
      {"me_lv_c"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_FALSE(Writes(MEs[0].TrustedOther)) << Table << Describe(MEs[0]);
}

// A table entry that constructs an optional string in its result and only
// reads other memory, called through inline layers. The result object holds
// nothing of what its memory held before the call.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ResultThroughInlineLayers) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  SKIP_ON_MSVC_ABI("the summary table keys Itanium mangled names");
  SKIP_ON_LIBCXX("expects the string layout of libstdc++");
  TestFixture::CreateInterpreter({"-O0", "-std=c++20"});
  // Sret, receiver, two integers; the result may receive integer data and
  // read it back, and holds only its own pointers. Untrusted, the throw path
  // is opaque.
  const std::string Table =
      SummaryHeader() + "_ZNK9me_rl_api3getB5cxx11Eil\tFFF\t-.rc-.-.\twrnn\t" +
      "mrnn\t..c.\tm\t.....e2521\ti\tFFF\t-.rc-.-.\twrnn\tmrnn\ts.c.\tr\t" +
      ".....e2620\t_ZNSt13runtime_errorC1EPKc\n";
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1);
  ASSERT_EQ(
      Cpp::Declare("#include <new>\n#include <optional>\n#include <string>"),
      0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    struct me_rl_api {
      long* data;
      std::optional<std::string> get(int kind, long id) const;
    };
    struct me_rl_client {
      me_rl_api api;
      std::optional<std::string> get(int kind, long id) const {
        return api.get(kind, id);
      }
    };
    struct me_rl_getter {
      static std::optional<std::string> get(const me_rl_client& c, int k,
                                            long id) {
        return c.get(k, id);
      }
    };
    extern "C" void me_rl_d(const me_rl_client* c, const int* k,
                            const long* id) {
      (void)me_rl_getter::get(*c, *k, *id);
    }
    extern "C" void me_rl_k(const me_rl_client* c, const int* k,
                            const long* id, void* out) {
      ::new (out) std::optional<std::string>(me_rl_getter::get(*c, *k, *id));
    }
  )",
      {"me_rl_d", "me_rl_k"});
  ASSERT_EQ(MEs.size(), 2u);
  for (const Cpp::MemoryEffects& ME : MEs) {
    ASSERT_TRUE(ME.Valid);
    EXPECT_TRUE(Mentions(ME, "runtime_error")) << OpaqueNames(ME);
    EXPECT_EQ(ME.TrustedOther, ModRef::Read) << Describe(ME);
    EXPECT_FALSE(ME.TrustedCaptured[0]) << Describe(ME);
  }
}

// At -O0, a result string copies a local string or a string that the
// receiver points to. Each copy loads the length twice, and optimization
// merges the loads. A merged load still reads an integer object: it holds no
// pointer, and the result holds only its own pointers.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_MergedIntegerLoadStaysData) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  ASSERT_EQ(CppInternal::LoadMemoryEffectsSummaries(
                SummaryHeader() + "me_ml_make\tFFF\t-.-.\twn\tnn\toc\tn\t" +
                "...220\t.\n"),
            1);
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    %S = type { ptr, i64, [16 x i8] }
    declare noalias ptr @_Znwm(i64)
    declare void @_ZdlPvm(ptr, i64)
    declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
    declare void @me_ml_make(ptr sret(%S), i8)
    define linkonce_odr ptr @_ZNSt11char_traitsIcE4copyEPcPKcm(ptr %d, ptr %s,
        i64 %n) optnone noinline {
      call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
      ret ptr %d
    }
    define i64 @me_ml_size(ptr %s) optnone noinline {
      %l = getelementptr inbounds %S, ptr %s, i32 0, i32 1
      %n = load i64, ptr %l
      ret i64 %n
    }
    define void @me_ml_copy(ptr %r, ptr %s) optnone noinline {
      %rb = getelementptr inbounds %S, ptr %r, i32 0, i32 2
      store ptr %rb, ptr %r
      %p = load ptr, ptr %s
      %n1 = call i64 @me_ml_size(ptr %s)
      %n2 = call i64 @me_ml_size(ptr %s)
      %big = icmp ugt i64 %n1, 15
      br i1 %big, label %heap, label %copy
    heap:
      %size = add i64 %n1, 1
      %h = call ptr @_Znwm(i64 %size)
      store ptr %h, ptr %r
      store i64 %n2, ptr %rb
      br label %copy
    copy:
      %d = load ptr, ptr %r
      call ptr @_ZNSt11char_traitsIcE4copyEPcPKcm(ptr %d, ptr %p, i64 %n1)
      %rl = getelementptr inbounds %S, ptr %r, i32 0, i32 1
      store i64 %n2, ptr %rl
      ret void
    }
    define void @me_ml_get(ptr noalias sret(%S) %r, ptr %this, i8 %k)
        optnone noinline {
      %s = alloca %S
      %own = icmp eq i8 %k, 0
      br i1 %own, label %mine, label %local
    mine:
      %t = load ptr, ptr %this
      call void @me_ml_copy(ptr %r, ptr %t)
      ret void
    local:
      call void @me_ml_make(ptr sret(%S) %s, i8 %k)
      call void @me_ml_copy(ptr %r, ptr %s)
      %p = load ptr, ptr %s
      %sb = getelementptr inbounds %S, ptr %s, i32 0, i32 2
      %short = icmp eq ptr %p, %sb
      br i1 %short, label %done, label %free
    free:
      call void @_ZdlPvm(ptr %p, i64 16)
      br label %done
    done:
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  const std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  std::vector<std::string> Row = RowOf(Table, "me_ml_get");
  ASSERT_EQ(Row.size(), 9u) << Table;
  EXPECT_EQ(Row[5][0], 's') << Table;
}

// A local holds an address as an integer. A callee stores a sum or a
// tagged form of it into the argument, and the caller reads the argument
// back as a pointer and writes through it.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ArithmeticOnAnAddressInData) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  for (const char* Opt : {"-O0", "-O2"}) {
    TestFixture::CreateInterpreter({Opt});
    ASSERT_EQ(Cpp::Declare("#include <cstdint>\n#include <cstring>"), 0);
    std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
        R"(
      inline long me_ad_g[8] = {};
      struct me_ad_f { uintptr_t a; };
      __attribute__((noinline)) void me_ad_addk(me_ad_f* d, const me_ad_f* s) {
        d->a = s->a + 8;
      }
      __attribute__((noinline)) void me_ad_tag(me_ad_f* d, const me_ad_f* s) {
        d->a = (s->a & ~uintptr_t(7)) | 1;
      }
      __attribute__((noinline)) void me_ad_poke(const void* d) {
        long* q;
        std::memcpy(&q, d, sizeof q);
        *q = 1;
      }
      __attribute__((noinline)) void me_ad_untag(const me_ad_f* d) {
        *reinterpret_cast<long*>(d->a & ~uintptr_t(7)) = 1;
      }
      extern "C" void me_ad_sum(me_ad_f* d) {
        me_ad_f s{(uintptr_t)&me_ad_g[0]};
        me_ad_addk(d, &s);
        me_ad_poke(d);
      }
      extern "C" void me_ad_tagged(me_ad_f* d) {
        me_ad_f s{(uintptr_t)&me_ad_g[0]};
        me_ad_tag(d, &s);
        me_ad_untag(d);
      }
    )",
        {"me_ad_sum", "me_ad_tagged"});
    ASSERT_EQ(MEs.size(), 2u);
    for (const Cpp::MemoryEffects& ME : MEs) {
      ASSERT_TRUE(ME.Valid) << Opt;
      EXPECT_TRUE(Writes(ME.Other)) << Opt << Describe(ME);
      EXPECT_TRUE(Writes(ME.TrustedOther)) << Opt << Describe(ME);
    }
  }
}

// As above, with the callee as a table entry that the summarizer writes
// from its -O0 body.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_ArithmeticOnAnAddressInATable) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    %F = type { i64 }
    define void @me_at_addk(ptr %d, ptr %s) optnone noinline {
      %dp = alloca ptr
      %sp = alloca ptr
      store ptr %d, ptr %dp
      store ptr %s, ptr %sp
      %s1 = load ptr, ptr %sp
      %sa = getelementptr inbounds %F, ptr %s1, i32 0, i32 0
      %v = load i64, ptr %sa
      %w = add i64 %v, 8
      %d1 = load ptr, ptr %dp
      %da = getelementptr inbounds %F, ptr %d1, i32 0, i32 0
      store i64 %w, ptr %da
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  const std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  TestFixture::CreateInterpreter({"-O0"});
  ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 1) << Table;
  ASSERT_EQ(Cpp::Declare("#include <cstdint>\n#include <cstring>"), 0);
  std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
      R"(
    inline long me_at_g[8] = {};
    struct me_at_f { uintptr_t a; };
    extern "C" void me_at_addk(me_at_f* d, const me_at_f* s);
    extern "C" void me_at_c(me_at_f* d) {
      me_at_f s{(uintptr_t)&me_at_g[0]};
      me_at_addk(d, &s);
      long* q;
      std::memcpy(&q, d, sizeof q);
      *q = 1;
    }
  )",
      {"me_at_c"});
  ASSERT_EQ(MEs.size(), 1u);
  ASSERT_TRUE(MEs[0].Valid);
  EXPECT_TRUE(Writes(MEs[0].Other)) << Table << Describe(MEs[0]);
  EXPECT_TRUE(Writes(MEs[0].TrustedOther)) << Table << Describe(MEs[0]);
}

// A callee puts an address into an integer object, or passes it on as an
// integer result or argument. Another callee adds to it. The caller reads
// the result back as a pointer and writes through it.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressDataFromCallees) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  for (const char* Opt : {"-O0", "-O2"}) {
    TestFixture::CreateInterpreter({Opt});
    ASSERT_EQ(Cpp::Declare("#include <cstdint>\n#include <cstring>"), 0);
    std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
        R"(
      inline long me_cd_g[8] = {};
      struct me_cd_f { uintptr_t a; };
      struct me_cd_big { uintptr_t a; long pad[8]; };
      __attribute__((noinline)) void me_cd_seta(me_cd_f* f, uintptr_t a) {
        f->a = a;
      }
      __attribute__((noinline)) void me_cd_seta2(me_cd_f* f, uintptr_t a) {
        me_cd_seta(f, a);
      }
      __attribute__((noinline)) void me_cd_addk(me_cd_f* d,
                                                const me_cd_f* s) {
        d->a = s->a + 8;
      }
      __attribute__((noinline)) void me_cd_addb(me_cd_f* d,
                                                const me_cd_big* s) {
        d->a = s->a + 8;
      }
      __attribute__((noinline)) void me_cd_fill(me_cd_big* d) {
        me_cd_big t{};
        t.a = (uintptr_t)&me_cd_g[0];
        std::memcpy(d, &t, sizeof t);
      }
      __attribute__((noinline)) uintptr_t me_cd_rd(const me_cd_f* s) {
        return s->a;
      }
      __attribute__((noinline)) void me_cd_add8(me_cd_f* d, uintptr_t v) {
        d->a = v + 8;
      }
      __attribute__((noinline)) void me_cd_poke(const void* d) {
        long* q;
        std::memcpy(&q, d, sizeof q);
        *q = 1;
      }
      extern "C" void me_cd_set(me_cd_f* d) {
        me_cd_f s{0};
        me_cd_seta(&s, (uintptr_t)&me_cd_g[0]);
        me_cd_addk(d, &s);
        me_cd_poke(d);
      }
      extern "C" void me_cd_set2(me_cd_f* d) {
        me_cd_f s{0};
        me_cd_seta2(&s, (uintptr_t)&me_cd_g[0]);
        me_cd_addk(d, &s);
        me_cd_poke(d);
      }
      extern "C" void me_cd_copy(me_cd_f* d) {
        me_cd_big s;
        me_cd_fill(&s);
        me_cd_addb(d, &s);
        me_cd_poke(d);
      }
      extern "C" void me_cd_split(me_cd_f* d) {
        me_cd_f s{(uintptr_t)&me_cd_g[0]};
        me_cd_add8(d, me_cd_rd(&s));
        me_cd_poke(d);
      }
    )",
        {"me_cd_set", "me_cd_set2", "me_cd_copy", "me_cd_split"});
    ASSERT_EQ(MEs.size(), 4u);
    for (size_t K = 0; K < MEs.size(); ++K) {
      ASSERT_TRUE(MEs[K].Valid) << Opt;
      EXPECT_TRUE(Writes(MEs[K].Other)) << Opt << " " << K << Describe(MEs[K]);
      EXPECT_TRUE(Writes(MEs[K].TrustedOther))
          << Opt << " " << K << Describe(MEs[K]);
    }
  }
}

// The callees above as table entries that the summarizer writes from their
// -O0 bodies.
TYPED_TEST(CPPINTEROP_TEST_MODE, MemoryEffects_AddressDataThroughTables) {
  SKIP_WITHOUT_MEMORY_EFFECTS();
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(R"(
    %struct.me_ct_f = type { i64 }
    %struct.me_ct_big = type { i64, [8 x i64] }
    @me_ct_g = global [8 x i64] zeroinitializer
    define void @me_ct_seta(ptr %f, i64 %a) optnone noinline {
    entry:
      %f.addr = alloca ptr
      %a.addr = alloca i64
      store ptr %f, ptr %f.addr
      store i64 %a, ptr %a.addr
      %0 = load i64, ptr %a.addr
      %1 = load ptr, ptr %f.addr
      %a1 = getelementptr inbounds nuw %struct.me_ct_f, ptr %1, i32 0, i32 0
      store i64 %0, ptr %a1
      ret void
    }
    define void @me_ct_seta2(ptr %f, i64 %a) optnone noinline {
    entry:
      %f.addr = alloca ptr
      %a.addr = alloca i64
      store ptr %f, ptr %f.addr
      store i64 %a, ptr %a.addr
      %0 = load ptr, ptr %f.addr
      %1 = load i64, ptr %a.addr
      call void @me_ct_seta(ptr %0, i64 %1)
      ret void
    }
    define void @me_ct_addk(ptr %d, ptr %s) optnone noinline {
    entry:
      %d.addr = alloca ptr
      %s.addr = alloca ptr
      store ptr %d, ptr %d.addr
      store ptr %s, ptr %s.addr
      %0 = load ptr, ptr %s.addr
      %a = getelementptr inbounds nuw %struct.me_ct_f, ptr %0, i32 0, i32 0
      %1 = load i64, ptr %a
      %add = add i64 %1, 8
      %2 = load ptr, ptr %d.addr
      %a1 = getelementptr inbounds nuw %struct.me_ct_f, ptr %2, i32 0, i32 0
      store i64 %add, ptr %a1
      ret void
    }
    define void @me_ct_addb(ptr %d, ptr %s) optnone noinline {
    entry:
      %d.addr = alloca ptr
      %s.addr = alloca ptr
      store ptr %d, ptr %d.addr
      store ptr %s, ptr %s.addr
      %0 = load ptr, ptr %s.addr
      %a = getelementptr inbounds nuw %struct.me_ct_big, ptr %0, i32 0, i32 0
      %1 = load i64, ptr %a
      %add = add i64 %1, 8
      %2 = load ptr, ptr %d.addr
      %a1 = getelementptr inbounds nuw %struct.me_ct_f, ptr %2, i32 0, i32 0
      store i64 %add, ptr %a1
      ret void
    }
    define void @me_ct_fill(ptr %d) optnone noinline {
    entry:
      %d.addr = alloca ptr
      %t = alloca %struct.me_ct_big
      store ptr %d, ptr %d.addr
      call void @llvm.memset.p0.i64(ptr %t, i8 0, i64 72, i1 false)
      %a = getelementptr inbounds nuw %struct.me_ct_big, ptr %t, i32 0, i32 0
      store i64 ptrtoint (ptr @me_ct_g to i64), ptr %a
      %0 = load ptr, ptr %d.addr
      call void @llvm.memcpy.p0.p0.i64(ptr %0, ptr %t, i64 72, i1 false)
      ret void
    }
    declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)
    declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
    define i64 @me_ct_rd(ptr %s) optnone noinline {
    entry:
      %s.addr = alloca ptr
      store ptr %s, ptr %s.addr
      %0 = load ptr, ptr %s.addr
      %a = getelementptr inbounds nuw %struct.me_ct_f, ptr %0, i32 0, i32 0
      %1 = load i64, ptr %a
      ret i64 %1
    }
    define void @me_ct_add8(ptr %d, i64 %v) optnone noinline {
    entry:
      %d.addr = alloca ptr
      %v.addr = alloca i64
      store ptr %d, ptr %d.addr
      store i64 %v, ptr %v.addr
      %0 = load i64, ptr %v.addr
      %add = add i64 %0, 8
      %1 = load ptr, ptr %d.addr
      %a = getelementptr inbounds nuw %struct.me_ct_f, ptr %1, i32 0, i32 0
      store i64 %add, ptr %a
      ret void
    }
  )",
                                                              Err, Ctx);
  ASSERT_TRUE(M) << Err.getMessage().str();
  const std::string Table = CppInternal::SummarizeMemoryEffects(*M);
  for (const char* Opt : {"-O0", "-O2"}) {
    TestFixture::CreateInterpreter({Opt});
    ASSERT_EQ(Cpp::LoadMemoryEffectsSummaries(Table.c_str()), 7) << Table;
    ASSERT_EQ(Cpp::Declare("#include <cstdint>\n#include <cstring>"), 0);
    std::vector<Cpp::MemoryEffects> MEs = Cpp::AnalyzeFunctionsMemoryEffects(
        R"(
      inline long me_ct_h[8] = {};
      extern "C" {
      struct me_ct_f { uintptr_t a; };
      struct me_ct_big { uintptr_t a; long pad[8]; };
      void me_ct_seta(me_ct_f* f, uintptr_t a);
      void me_ct_seta2(me_ct_f* f, uintptr_t a);
      void me_ct_addk(me_ct_f* d, const me_ct_f* s);
      void me_ct_addb(me_ct_f* d, const me_ct_big* s);
      void me_ct_fill(me_ct_big* d);
      uintptr_t me_ct_rd(const me_ct_f* s);
      void me_ct_add8(me_ct_f* d, uintptr_t v);
      void me_ct_poke(const me_ct_f* d) {
        long* q;
        std::memcpy(&q, d, sizeof q);
        *q = 1;
      }
      void me_ct_set(me_ct_f* d) {
        me_ct_f s{0};
        me_ct_seta(&s, (uintptr_t)&me_ct_h[0]);
        me_ct_addk(d, &s);
        me_ct_poke(d);
      }
      void me_ct_set2(me_ct_f* d) {
        me_ct_f s{0};
        me_ct_seta2(&s, (uintptr_t)&me_ct_h[0]);
        me_ct_addk(d, &s);
        me_ct_poke(d);
      }
      void me_ct_copy(me_ct_f* d) {
        me_ct_big s;
        me_ct_fill(&s);
        me_ct_addb(d, &s);
        me_ct_poke(d);
      }
      void me_ct_split(me_ct_f* d) {
        me_ct_f s{(uintptr_t)&me_ct_h[0]};
        me_ct_add8(d, me_ct_rd(&s));
        me_ct_poke(d);
      }
      }
    )",
        {"me_ct_set", "me_ct_set2", "me_ct_copy", "me_ct_split"});
    ASSERT_EQ(MEs.size(), 4u);
    for (size_t K = 0; K < MEs.size(); ++K) {
      ASSERT_TRUE(MEs[K].Valid) << Opt;
      EXPECT_TRUE(Writes(MEs[K].Other))
          << Opt << " " << K << Table << Describe(MEs[K]);
      EXPECT_TRUE(Writes(MEs[K].TrustedOther))
          << Opt << " " << K << Describe(MEs[K]);
    }
  }
}
