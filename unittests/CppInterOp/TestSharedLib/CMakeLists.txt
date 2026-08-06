add_llvm_library(TestSharedLib
  SHARED
  DISABLE_LLVM_LINK_LLVM_DYLIB
  BUILDTREE_ONLY
  TestSharedLib.cpp)
# Put TestSharedLib next to the unit test executable.
set_output_directory(TestSharedLib
  BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/../bin/$<CONFIG>/
  LIBRARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/../bin/$<CONFIG>/
  )


if (EMSCRIPTEN)
  target_compile_options(TestSharedLib
    PUBLIC "SHELL: ${CPPINTEROP_EXTRA_WASM_FLAGS}"
  )

  target_link_options(TestSharedLib
    PRIVATE "SHELL: -s WASM_BIGINT"
    PRIVATE "SHELL: -s SIDE_MODULE=1"
    PUBLIC "SHELL: ${CPPINTEROP_EXTRA_WASM_FLAGS}"
  )
endif()

if (APPLE)
  # LLVM's -fvisibility-inlines-hidden hides Meyers-singleton backing
  # variables on Darwin; keep them exported -- the fixture models a library
  # that exports its header-defined singletons.
  target_compile_options(TestSharedLib PRIVATE
    -fno-visibility-inlines-hidden-static-local-var)
endif()

set_target_properties(TestSharedLib PROPERTIES FOLDER "Tests")
