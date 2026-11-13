# CMake toolchain file for QNX SDP 8.0 (Windows, Linux or macOS host).
#
# Run the SDP environment script first (qnxsdp-env.bat / qnxsdp-env.sh) so that
# QNX_HOST and QNX_TARGET are set, then:
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/qnx800.toolchain.cmake \
#         -DQNX_ARCH=x86_64      (or aarch64le)
#
# SPDX-License-Identifier: Apache-2.0

if(NOT DEFINED ENV{QNX_HOST} OR NOT DEFINED ENV{QNX_TARGET})
  message(FATAL_ERROR
    "QNX_HOST / QNX_TARGET are not set. Run the QNX SDP 8.0 environment script first "
    "(qnxsdp-env.bat on Windows, 'source qnxsdp-env.sh' elsewhere).")
endif()

file(TO_CMAKE_PATH "$ENV{QNX_HOST}"   QNX_HOST)
file(TO_CMAKE_PATH "$ENV{QNX_TARGET}" QNX_TARGET)

if(NOT QNX_ARCH)
  if(DEFINED ENV{QNX_ARCH})
    set(QNX_ARCH "$ENV{QNX_ARCH}")
  else()
    set(QNX_ARCH x86_64)
  endif()
endif()
# try_compile() runs a nested configure; hand our selection down to it.
set(ENV{QNX_ARCH} "${QNX_ARCH}")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES QNX_ARCH)

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 8.0.0)

if(QNX_ARCH STREQUAL "x86_64")
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
  set(_qnx_qcc_target gcc_ntox86_64)
  set(_qnx_binutils_prefix ntox86_64)
elseif(QNX_ARCH STREQUAL "aarch64le")
  set(CMAKE_SYSTEM_PROCESSOR aarch64)
  set(_qnx_qcc_target gcc_ntoaarch64le)
  set(_qnx_binutils_prefix ntoaarch64)
else()
  message(FATAL_ERROR "QNX_ARCH must be x86_64 or aarch64le (got '${QNX_ARCH}')")
endif()

if(CMAKE_HOST_WIN32)
  set(_qnx_exe .exe)
else()
  set(_qnx_exe "")
endif()

set(CMAKE_C_COMPILER          "${QNX_HOST}/usr/bin/qcc${_qnx_exe}")
set(CMAKE_C_COMPILER_TARGET   ${_qnx_qcc_target})
set(CMAKE_CXX_COMPILER        "${QNX_HOST}/usr/bin/q++${_qnx_exe}")
set(CMAKE_CXX_COMPILER_TARGET ${_qnx_qcc_target})
set(CMAKE_ASM_COMPILER        "${QNX_HOST}/usr/bin/qcc${_qnx_exe}")
set(CMAKE_ASM_COMPILER_TARGET ${_qnx_qcc_target})

set(CMAKE_AR     "${QNX_HOST}/usr/bin/${_qnx_binutils_prefix}-ar${_qnx_exe}"     CACHE FILEPATH "QNX ar")
set(CMAKE_RANLIB "${QNX_HOST}/usr/bin/${_qnx_binutils_prefix}-ranlib${_qnx_exe}" CACHE FILEPATH "QNX ranlib")
set(CMAKE_STRIP  "${QNX_HOST}/usr/bin/${_qnx_binutils_prefix}-strip${_qnx_exe}"  CACHE FILEPATH "QNX strip")
set(CMAKE_NM     "${QNX_HOST}/usr/bin/${_qnx_binutils_prefix}-nm${_qnx_exe}"     CACHE FILEPATH "QNX nm")
set(CMAKE_OBJCOPY "${QNX_HOST}/usr/bin/${_qnx_binutils_prefix}-objcopy${_qnx_exe}" CACHE FILEPATH "QNX objcopy")
set(CMAKE_OBJDUMP "${QNX_HOST}/usr/bin/${_qnx_binutils_prefix}-objdump${_qnx_exe}" CACHE FILEPATH "QNX objdump")

# Headers are shared by all CPUs (QNX_TARGET/usr/include); libraries are per CPU.
set(CMAKE_SYSROOT "${QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH "${QNX_TARGET}/${QNX_ARCH}" "${QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
