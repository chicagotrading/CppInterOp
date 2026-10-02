// dlopens TestDownstreamLib without linking libclangCppInterOp; with
// argv[1] = libclangCppInterOp path, also drives the probe's
// LoadDispatchAPI check to pin the X-macro slot-population contract.

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
using HandleTy = HMODULE;
static HandleTy openLib(const char* p) { return LoadLibraryA(p); }
static void* findSym(HandleTy h, const char* n) {
  return reinterpret_cast<void*>(GetProcAddress(h, n));
}
static void closeLib(HandleTy h) { FreeLibrary(h); }
static const char* lastErr() { return "LoadLibrary failed"; }
#else
#include <dlfcn.h>
using HandleTy = void*;
static HandleTy openLib(const char* p) {
  return dlopen(p, RTLD_NOW | RTLD_LOCAL);
}
static void* findSym(HandleTy h, const char* n) { return dlsym(h, n); }
static void closeLib(HandleTy h) { dlclose(h); }
static const char* lastErr() { return dlerror(); }
#endif

#ifndef TEST_DOWNSTREAM_LIB_PATH
#error "TEST_DOWNSTREAM_LIB_PATH must be defined"
#endif

int main(int argc, char** argv) {
  HandleTy h = openLib(TEST_DOWNSTREAM_LIB_PATH);
  if (!h) {
    std::fprintf(stderr, "open(%s) failed: %s\n", TEST_DOWNSTREAM_LIB_PATH,
                 lastErr());
    return 1;
  }
#ifndef _WIN32
  // The Dispatch.h inline Cpp::* wrappers must stay hidden. An exported copy
  // interposes libclangCppInterOp's own dispatch table under RTLD_GLOBAL, and
  // every Cpp:: call then recurses into itself. downstream_wrapper_address()
  // forces an out-of-line copy, so the check holds at any -O level.
  if (!findSym(h, "downstream_wrapper_address")) {
    std::fprintf(stderr, "missing downstream_wrapper_address: %s\n", lastErr());
    closeLib(h);
    return 5;
  }
  if (findSym(h, "_ZN3Cpp14GetInterpreterEv")) {
    std::fprintf(stderr, "Cpp::GetInterpreter is exported from %s\n",
                 TEST_DOWNSTREAM_LIB_PATH);
    closeLib(h);
    return 6;
  }
#endif
  // argv[1] is the libclangCppInterOp path; verify slot population.
  int rc = 0;
  if (argc > 1) {
    auto* verify = reinterpret_cast<int (*)(const char*)>(
        findSym(h, "downstream_verify_trace_slots"));
    if (!verify) {
      std::fprintf(stderr, "missing downstream_verify_trace_slots: %s\n",
                   lastErr());
      rc = 2;
    } else {
      rc = verify(argv[1]);
      if (rc != 0)
        std::fprintf(stderr, "downstream_verify_trace_slots -> %d\n", rc);
    }
  }
  closeLib(h);
  return rc;
}
