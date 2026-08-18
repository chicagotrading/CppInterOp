#ifndef UNITTESTS_CPPINTEROP_TESTSHAREDLIB_TESTSHAREDLIB_H
#define UNITTESTS_CPPINTEROP_TESTSHAREDLIB_TESTSHAREDLIB_H

#ifdef _WIN32
#define TESTSHAREDLIB_API __declspec(dllexport)
#else
#define TESTSHAREDLIB_API __attribute__((visibility("default")))
#endif

// Avoid having to mangle/demangle the symbol name in tests
extern "C" TESTSHAREDLIB_API int ret_zero();

// A polymorphic type whose vtable is anchored in this shared library,
// plus a dispatch helper compiled here. Calling OverlayDispatchOnce
// from another translation unit is a genuine cross-DSO virtual call
// the caller's compiler cannot devirtualize or inline -- the honest
// setting for measuring VTableOverlay dispatch cost.
struct TESTSHAREDLIB_API OverlayBase {
  OverlayBase();
  virtual ~OverlayBase();
  virtual int frob(int x);
};

extern "C" TESTSHAREDLIB_API int OverlayDispatchOnce(OverlayBase* b, int x);

// Header-*defined* singleton state, in the two shapes that compile to weak
// globals: a function-local static in an inline function and a C++17 inline
// static data member. TestSharedLib.cpp compiles an AOT copy of both (the
// accessors below); jitted code that repeats these definitions must bind the
// library's copies rather than materialize duplicates.
struct TESTSHAREDLIB_API SingletonFixture {
  static SingletonFixture& get() {
    static SingletonFixture instance;
    return instance;
  }

  static inline int s_inline_member = 0;

  int value = 0;
};

extern "C" TESTSHAREDLIB_API void* singleton_fixture_meyers_addr();
extern "C" TESTSHAREDLIB_API int singleton_fixture_meyers_value();
extern "C" TESTSHAREDLIB_API void* singleton_fixture_member_addr();
extern "C" TESTSHAREDLIB_API int singleton_fixture_member_value();

// A native thread-local plus accessors compiled here: jitted redeclarations
// must share the calling thread's storage with them
// (compat::redirectNativeTLSDeclarations).
#if !defined(_WIN32) && !defined(EMSCRIPTEN)
extern "C" {
TESTSHAREDLIB_API extern __thread int native_tls_slot;
TESTSHAREDLIB_API void* native_tls_slot_addr();
TESTSHAREDLIB_API int native_tls_slot_value();
}
#endif

#endif // UNITTESTS_CPPINTEROP_TESTSHAREDLIB_TESTSHAREDLIB_H
