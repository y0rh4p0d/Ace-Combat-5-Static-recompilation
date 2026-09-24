# CMake toolchain for a Windows/AArch64 (arm64) build of ac5.
#
# Windows on ARM has no MSVC here and the LunarG Vulkan SDK only ships x64/ARM64
# MSVC import libraries, so this uses the MSYS2 clang toolchain: an x86_64 clang
# driver that targets aarch64-w64-mingw32, plus the matching mingw-w64 sysroot.
#
# Usage (from the repository root):
#
#   cmake -S . -B build/arm64 -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-arm64.cmake \
#     -DPS2_GENERATED_DIR=generated-cnjp
#   cmake --build build/arm64
#
# Override any of these on the command line if your MSYS2 lives elsewhere:
#   -DPS2_HOST_TOOLS=C:/msys64/clang64   (x86_64 clang driver)
#   -DPS2_SYSROOT=C:/msys64/clangarm64   (aarch64 target sysroot)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# CMake re-includes this file for try_compile/check_* probes, which do not carry
# the -D options from the command line.  A CACHE default would therefore replace
# the caller's value on the second pass (and warn about the wrong directory), so
# keep these as plain variables: an existing definition, including one from -D,
# always wins.
if(NOT DEFINED PS2_HOST_TOOLS)
  set(PS2_HOST_TOOLS "C:/msys64/clang64")
endif()
if(NOT DEFINED PS2_SYSROOT)
  set(PS2_SYSROOT "C:/msys64/clangarm64")
endif()

set(_ps2_target aarch64-w64-mingw32)

# A native x86_64 clang is required: ${PS2_SYSROOT}/bin/clang.exe is itself an
# AArch64 binary and cannot run on an x64 host.  This toolchain therefore expects
# an x86_64 clang (from the MSYS2 clang64 environment, or any other install) and
# points it at the aarch64 sysroot.
find_program(PS2_CLANG NAMES clang
             HINTS "${PS2_HOST_TOOLS}/bin" ENV PATH
             REQUIRED)
find_program(PS2_CLANGXX NAMES clang++ clang
             HINTS "${PS2_HOST_TOOLS}/bin" ENV PATH
             REQUIRED)
find_program(PS2_PKGCONFIG NAMES pkgconf pkg-config
             HINTS "${PS2_HOST_TOOLS}/bin" ENV PATH
             REQUIRED)
# An archiver has to be a host binary too, and the AArch64 sysroot ships no `ar`.
# llvm-tools provides llvm-ar/llvm-ranlib, which read and write AArch64 objects
# without caring what they run on.
find_program(PS2_AR NAMES llvm-ar ar
             HINTS "${PS2_HOST_TOOLS}/bin" ENV PATH)
find_program(PS2_RANLIB NAMES llvm-ranlib ranlib
             HINTS "${PS2_HOST_TOOLS}/bin" ENV PATH)
find_program(PS2_NM NAMES llvm-nm nm
             HINTS "${PS2_HOST_TOOLS}/bin" ENV PATH)
if(NOT PS2_AR)
  message(FATAL_ERROR
    "No archiver found.  Install mingw-w64-clang-x86_64-llvm-tools into "
    "${PS2_HOST_TOOLS} (tools/setup_arm64_toolchain.py does this).")
endif()

set(CMAKE_C_COMPILER "${PS2_CLANG}")
set(CMAKE_CXX_COMPILER "${PS2_CLANGXX}")

set(CMAKE_C_COMPILER_TARGET "${_ps2_target}")
set(CMAKE_CXX_COMPILER_TARGET "${_ps2_target}")

set(CMAKE_AR "${PS2_AR}" CACHE FILEPATH "" FORCE)
if(PS2_RANLIB)
  set(CMAKE_RANLIB "${PS2_RANLIB}" CACHE FILEPATH "" FORCE)
endif()
if(PS2_NM)
  set(CMAKE_NM "${PS2_NM}" CACHE FILEPATH "" FORCE)
endif()

# Use LLVM's own linker.  The mingw-w64 sysroot ships no `ld`, and a GNU ld built
# for aarch64 cannot run on an x64 host anyway, so a native ld.lld is the only
# option; `ld.lld` next to clang is picked up through the driver search path.
set(_ps2_ld "${PS2_HOST_TOOLS}/bin/ld.lld.exe")
if(EXISTS "${_ps2_ld}")
  set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
  set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
  set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
else()
  message(WARNING "ld.lld not found in ${PS2_HOST_TOOLS}/bin; linking may fail")
endif()

# Compiler runtime and intrinsic headers must come from the *target* clang
# installation, because the resource directory of an x86_64 clang install has no
# libclang_rt.builtins-aarch64.a.  The clangarm64 tree carries a matching one.
set(_ps2_resdir "${PS2_SYSROOT}/lib/clang")
file(GLOB _ps2_versions RELATIVE "${_ps2_resdir}" "${_ps2_resdir}/*")
list(SORT _ps2_versions)
list(REVERSE _ps2_versions)
foreach(_v IN LISTS _ps2_versions)
  if(EXISTS "${_ps2_resdir}/${_v}/lib/windows/libclang_rt.builtins-aarch64.a")
    set(PS2_CLANG_RESOURCE_DIR "${_ps2_resdir}/${_v}")
    break()
  endif()
endforeach()

if(PS2_CLANG_RESOURCE_DIR)
  message(STATUS "clang resource dir: ${PS2_CLANG_RESOURCE_DIR}")
  string(APPEND CMAKE_C_FLAGS_INIT " -resource-dir \"${PS2_CLANG_RESOURCE_DIR}\"")
  string(APPEND CMAKE_CXX_FLAGS_INIT " -resource-dir \"${PS2_CLANG_RESOURCE_DIR}\"")
else()
  message(WARNING
    "No aarch64 clang resource directory under ${_ps2_resdir}; the compiler "
    "runtime (libclang_rt.builtins-aarch64.a) will not be found and linking "
    "will fail. Install mingw-w64-clang-aarch64-compiler-rt.")
endif()

# Search the sysroot for headers and libraries, never the host.
set(CMAKE_SYSROOT "${PS2_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "${PS2_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# pkg-config must read the target .pc files, not the host's.
set(ENV{PKG_CONFIG_LIBDIR} "${PS2_SYSROOT}/lib/pkgconfig;${PS2_SYSROOT}/share/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${PS2_SYSROOT}")
set(PKG_CONFIG_EXECUTABLE "${PS2_PKGCONFIG}" CACHE FILEPATH "" FORCE)

# The clang driver needs its own DLLs (libLLVM, libc++, libwinpthread) next to it
# or on PATH; put the host tools directory first so it is self-contained.
set(ENV{PATH} "${PS2_HOST_TOOLS}/bin;$ENV{PATH}")
