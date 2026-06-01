# Wrapper for the vendored mldsa-native submodule (external/mldsa-native).
#
# mldsa-native (https://github.com/pq-code-package/mldsa-native) ships with
# a Makefile-based build and does not expose a CMake target of its own. This
# file replicates the "monolithic" build path in CMake: a single auto-
# generated mldsa/mldsa_native.c source file bundles all of the library's
# C code, and an optional mldsa/mldsa_native_asm.S bundles the native-
# backend assembly. We compile those two files into a STATIC library.
#
# Parameter set is fixed at ML-DSA-44 (Dilithium-2) per the engagement
# scope. Build is C-only by default; the AVX2 native backend is opt-in via
# the mldsa_avx2 CMake option, intended for benchmark runs.

option(mldsa_avx2
  "Build mldsa-native with the AVX2 native backend (x86_64 only)" OFF)

set(MLDSA_NATIVE_ROOT ${CMAKE_SOURCE_DIR}/external/mldsa-native)

set(MLDSA_SRC ${MLDSA_NATIVE_ROOT}/mldsa/mldsa_native.c)
if(mldsa_avx2)
  enable_language(ASM)
  list(APPEND MLDSA_SRC ${MLDSA_NATIVE_ROOT}/mldsa/mldsa_native_asm.S)
endif()

add_library(mldsa_native STATIC ${MLDSA_SRC})
add_library(Mldsa::native ALIAS mldsa_native)

target_include_directories(mldsa_native PUBLIC
  $<BUILD_INTERFACE:${MLDSA_NATIVE_ROOT}/mldsa>)

target_compile_definitions(mldsa_native PUBLIC
  MLD_CONFIG_PARAMETER_SET=44)

if(mldsa_avx2)
  target_compile_definitions(mldsa_native PRIVATE
    MLD_CONFIG_USE_NATIVE_BACKEND_ARITH
    MLD_CONFIG_USE_NATIVE_BACKEND_FIPS202)
  target_compile_options(mldsa_native PRIVATE -mavx2)
endif()

# Vendored third-party code: suppress strict warnings so rippled's -Werror
# policy (under the werr build option) does not flag the library.
target_compile_options(mldsa_native PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w>
  $<$<C_COMPILER_ID:MSVC>:/w>)

target_compile_features(mldsa_native PRIVATE c_std_99)
