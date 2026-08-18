#include "TestSharedLib.h"

int ret_zero() { return 0; }

OverlayBase::OverlayBase() {}
OverlayBase::~OverlayBase() {}
int OverlayBase::frob(int x) { return x; }

int OverlayDispatchOnce(OverlayBase* b, int x) { return b->frob(x); }

void* singleton_fixture_meyers_addr() { return &SingletonFixture::get(); }
int singleton_fixture_meyers_value() { return SingletonFixture::get().value; }
void* singleton_fixture_member_addr() {
  return &SingletonFixture::s_inline_member;
}
int singleton_fixture_member_value() {
  return SingletonFixture::s_inline_member;
}

#if !defined(_WIN32) && !defined(EMSCRIPTEN)
__thread int native_tls_slot = 0;

void* native_tls_slot_addr() { return &native_tls_slot; }
int native_tls_slot_value() { return native_tls_slot; }
#endif
