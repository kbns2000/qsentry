# open62541-qnx: open62541 (OPC UA) ported to QNX 8.0
#
#   python scripts/port.py                       (fetch the release, apply the port)
#   cmake -G Ninja -S . -B build/x86_64 \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/qnx800.toolchain.cmake -DQNX_ARCH=x86_64
#   cmake --build build/x86_64
#
# QNX_ARCH is x86_64 or aarch64le. The file also configures for the host
# (Linux), but all results of this project come from the QNX builds.
#
# Targets
#   open62541            static library of the port          (libopen62541.a)
#   open62541_upstream   the release with the same build configuration and no
#                        other change, only for comparison   (O6Q_BUILD_UPSTREAM)
#   o6q_check            the on-target check, linked with the port
#   o6q_check_upstream   the same check, linked with the unchanged release
#   o6q_envprobe         what the operating system itself does (no open62541)
#   o6q_server           demo server: the live QNX system as OPC UA nodes
#
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(open62541_qnx VERSION 1.4.10 LANGUAGES C)

option(O6Q_BUILD_UPSTREAM "Also build the unchanged release and o6q_check_upstream" ON)
set(O6Q_EVENTLOOP "auto" CACHE STRING
    "Event loop of the port: auto (ppoll on QNX, epoll on Linux), ppoll, select")
set_property(CACHE O6Q_EVENTLOOP PROPERTY STRINGS auto ppoll select)

set(O6Q_TP "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# ------------------------------------------------------------------ the port
# third_party/ is not in the repository: scripts/port.py downloads the release,
# checks its SHA-256 and applies patches/. Run it here if that was not done,
# and again at build time whenever a file of patches/ changes.
find_package(Python3 COMPONENTS Interpreter)
file(GLOB O6Q_PATCH_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/patches/*")
set(O6Q_PORT_OUTPUTS
    "${O6Q_TP}/open62541.c" "${O6Q_TP}/open62541.h"
    "${O6Q_TP}/upstream/open62541.c" "${O6Q_TP}/upstream/open62541.h")
if(NOT EXISTS "${O6Q_TP}/open62541.c" OR NOT EXISTS "${O6Q_TP}/upstream/open62541.c")
  if(NOT Python3_Interpreter_FOUND)
    message(FATAL_ERROR "third_party/open62541.c is missing and no Python 3 was found. "
                        "Run: python scripts/port.py")
  endif()
  message(STATUS "Running scripts/port.py (downloads open62541 v${PROJECT_VERSION})")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/port.py"
                  RESULT_VARIABLE o6q_port_result)
  if(NOT o6q_port_result EQUAL 0)
    message(FATAL_ERROR "scripts/port.py failed")
  endif()
endif()
if(Python3_Interpreter_FOUND)
  add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/port.stamp"
    BYPRODUCTS ${O6Q_PORT_OUTPUTS}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/port.py"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/port.stamp"
    DEPENDS ${O6Q_PATCH_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/scripts/port.py"
    COMMENT "Applying the QNX port to the open62541 release"
    VERBATIM)
  add_custom_target(o6q_port_sources DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/port.stamp")
else()
  add_custom_target(o6q_port_sources)
endif()

# ------------------------------------------------------------ system libraries
find_package(OpenSSL REQUIRED)
set(O6Q_SYSTEM_LIBS OpenSSL::Crypto m)
if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  # The socket API of QNX is in libsocket; threads, clocks and timers are in libc.
  list(APPEND O6Q_SYSTEM_LIBS socket)
else()
  find_package(Threads REQUIRED)
  list(APPEND O6Q_SYSTEM_LIBS Threads::Threads)
endif()

# CMake's "Release" is -O3; the release packages of open62541 are built with -O2.
string(REPLACE "-O3" "-O2" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")

# open62541 is always compiled with -fno-strict-aliasing by its own build
# system (for gcc and clang; qcc is gcc). A compiler that does not know a
# declaration assumes "int f()", which on a 64-bit CPU cuts off pointers: on a
# new operating system that must stop the build, not become a warning.
set(O6Q_LIB_FLAGS -fno-strict-aliasing -Wall)
set(O6Q_STRICT_FLAGS -Werror=implicit-function-declaration -Werror=int-conversion
                     -Werror=incompatible-pointer-types)

add_library(open62541 STATIC "${O6Q_TP}/open62541.c")
add_dependencies(open62541 o6q_port_sources)
target_include_directories(open62541 PUBLIC
  "$<BUILD_INTERFACE:${O6Q_TP}>" "$<INSTALL_INTERFACE:include>")
target_compile_options(open62541 PRIVATE ${O6Q_LIB_FLAGS} ${O6Q_STRICT_FLAGS})
target_link_libraries(open62541 PUBLIC ${O6Q_SYSTEM_LIBS})
set_target_properties(open62541 PROPERTIES C_STANDARD 99 C_EXTENSIONS ON
                                           POSITION_INDEPENDENT_CODE ON)
if(O6Q_EVENTLOOP STREQUAL "ppoll")
  target_compile_definitions(open62541 PUBLIC O6Q_EVENTLOOP_POLL=1)
elseif(O6Q_EVENTLOOP STREQUAL "select")
  target_compile_definitions(open62541 PUBLIC O6Q_EVENTLOOP_SELECT=1)
elseif(NOT O6Q_EVENTLOOP STREQUAL "auto")
  message(FATAL_ERROR "O6Q_EVENTLOOP must be auto, ppoll or select")
endif()

if(O6Q_BUILD_UPSTREAM)
  # No -Werror here: what the compiler says about the unchanged release on QNX
  # is a result (see results/), and the comparison needs the binary.
  add_library(open62541_upstream STATIC EXCLUDE_FROM_ALL "${O6Q_TP}/upstream/open62541.c")
  add_dependencies(open62541_upstream o6q_port_sources)
  target_include_directories(open62541_upstream PUBLIC "${O6Q_TP}/upstream")
  target_compile_options(open62541_upstream PRIVATE ${O6Q_LIB_FLAGS})
  target_link_libraries(open62541_upstream PUBLIC ${O6Q_SYSTEM_LIBS})
  set_target_properties(open62541_upstream PROPERTIES C_STANDARD 99 C_EXTENSIONS ON
                                                      POSITION_INDEPENDENT_CODE ON)
endif()

# ------------------------------------------------------------------- programs
set(O6Q_WARN -Wall -Wextra -Wshadow -Wformat=2 ${O6Q_STRICT_FLAGS})

set(O6Q_CHECK_SOURCES
    src/o6q_check.c src/o6q_common.c src/o6q_model.c src/qnx/o6q_sysinfo.c
    src/suite_core.c src/suite_security.c src/suite_pubsub.c src/suite_highfd.c
    src/suite_robust.c)

# The header of the release is included as a system header in the programs:
# -Wextra in our code must not report what is in open62541.h (the library
# itself is compiled with its header as a normal include).
add_executable(o6q_check ${O6Q_CHECK_SOURCES})
target_include_directories(o6q_check PRIVATE src src/qnx)
target_include_directories(o6q_check SYSTEM PRIVATE "${O6Q_TP}")
target_compile_options(o6q_check PRIVATE ${O6Q_WARN})
target_link_libraries(o6q_check PRIVATE open62541)

if(O6Q_BUILD_UPSTREAM)
  add_executable(o6q_check_upstream ${O6Q_CHECK_SOURCES})
  target_include_directories(o6q_check_upstream PRIVATE src src/qnx)
  target_include_directories(o6q_check_upstream SYSTEM PRIVATE "${O6Q_TP}/upstream")
  target_compile_definitions(o6q_check_upstream PRIVATE O6Q_VARIANT="upstream")
  target_compile_options(o6q_check_upstream PRIVATE ${O6Q_WARN})
  target_link_libraries(o6q_check_upstream PRIVATE open62541_upstream)
endif()

add_executable(o6q_envprobe src/o6q_envprobe.c)
target_compile_definitions(o6q_envprobe PRIVATE O6Q_HAVE_OPENSSL=1)
target_compile_options(o6q_envprobe PRIVATE ${O6Q_WARN})
target_link_libraries(o6q_envprobe PRIVATE ${O6Q_SYSTEM_LIBS})

add_executable(o6q_server src/o6q_server.c src/o6q_common.c src/o6q_model.c
                          src/qnx/o6q_sysinfo.c src/qnx/o6q_slog.c)
target_include_directories(o6q_server PRIVATE src src/qnx)
target_include_directories(o6q_server SYSTEM PRIVATE "${O6Q_TP}")
target_compile_options(o6q_server PRIVATE ${O6Q_WARN})
target_link_libraries(o6q_server PRIVATE open62541)
if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  find_library(O6Q_SLOG2_LIBRARY slog2)
  if(O6Q_SLOG2_LIBRARY)
    target_compile_definitions(o6q_server PRIVATE O6Q_HAVE_SLOG2=1)
    target_link_libraries(o6q_server PRIVATE "${O6Q_SLOG2_LIBRARY}")
  endif()
endif()

# -------------------------------------------------------------------- install
include(GNUInstallDirs)
install(TARGETS open62541 ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${O6Q_TP}/open62541.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS o6q_check o6q_envprobe o6q_server RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES LICENSE patches/LICENSE-MPL-2.0 "${O6Q_TP}/PORT_MANIFEST.txt"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/open62541-qnx")

message(STATUS "open62541-qnx: system=${CMAKE_SYSTEM_NAME} processor=${CMAKE_SYSTEM_PROCESSOR} "
               "compiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} "
               "OpenSSL=${OPENSSL_VERSION} eventloop=${O6Q_EVENTLOOP} "
               "upstream_comparison=${O6Q_BUILD_UPSTREAM}")
