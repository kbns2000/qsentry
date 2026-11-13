# Compile-time probe: which of the OS interfaces that a network library such as
# open62541 looks for exist in QNX SDP 8.0?  Nothing is run on the host or on
# the target; every answer comes from a cross try_compile (compile + link).
#
#   cmake -G Ninja -S tools/apiprobe -B <dir> -DCMAKE_TOOLCHAIN_FILE=cmake/qnx800.toolchain.cmake -DQNX_ARCH=x86_64
#
# Result: <dir>/qnx_api_matrix.txt (lines "O6Q_API <kind> <name> <yes|no>") and
# the target program o6q_envprobe, which prints run-time facts on the target.
#
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
project(o6q_apiprobe C)

include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckStructHasMember)

set(_out "${CMAKE_BINARY_DIR}/qnx_api_matrix.txt")
file(WRITE "${_out}" "# QNX_ARCH=${QNX_ARCH} CMAKE_SYSTEM=${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION} compiler=${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}\n")

function(o6q_record kind name ok)
  if(ok)
    set(v yes)
  else()
    set(v no)
  endif()
  file(APPEND "${_out}" "O6Q_API ${kind} ${name} ${v}\n")
  message(STATUS "O6Q_API ${kind} ${name} ${v}")
endfunction()

function(o6q_header h)
  string(MAKE_C_IDENTIFIER "O6Q_HAVE_H_${h}" var)
  check_include_file("${h}" ${var})
  o6q_record(header "${h}" "${${var}}")
endfunction()

function(o6q_symbol sym headers)
  string(MAKE_C_IDENTIFIER "O6Q_HAVE_${sym}" var)
  check_symbol_exists(${sym} "${headers}" ${var})
  o6q_record(symbol "${sym}" "${${var}}")
endfunction()

function(o6q_member struct member headers)
  string(MAKE_C_IDENTIFIER "O6Q_HAVE_${struct}_${member}" var)
  check_struct_has_member("${struct}" ${member} "${headers}" ${var} LANGUAGE C)
  o6q_record(member "${struct}.${member}" "${${var}}")
endfunction()

# The BSD socket API lives in libsocket on QNX.
find_library(O6Q_LIBSOCKET socket)
find_library(O6Q_LIBSLOG2 slog2)
o6q_record(library socket "${O6Q_LIBSOCKET}")
o6q_record(library slog2 "${O6Q_LIBSLOG2}")
set(_libs "")
if(O6Q_LIBSOCKET)
  list(APPEND _libs socket)
endif()
if(O6Q_LIBSLOG2)
  list(APPEND _libs slog2)
endif()
set(CMAKE_REQUIRED_LIBRARIES ${_libs})
# Ask for everything the OS offers (not only strict ISO C / POSIX).
if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  set(CMAKE_REQUIRED_DEFINITIONS -D_QNX_SOURCE)
  add_compile_definitions(_QNX_SOURCE)
else()
  set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
  add_compile_definitions(_GNU_SOURCE)
endif()

find_package(OpenSSL QUIET)
o6q_record(package "OpenSSL-${OPENSSL_VERSION}" "${OPENSSL_FOUND}")
message(STATUS "OpenSSL: found=${OPENSSL_FOUND} version=${OPENSSL_VERSION} include=${OPENSSL_INCLUDE_DIR} ssl=${OPENSSL_SSL_LIBRARY} crypto=${OPENSSL_CRYPTO_LIBRARY}")

foreach(h
    stdatomic.h sys/epoll.h sys/event.h sys/eventfd.h sys/timerfd.h sys/signalfd.h
    sys/select.h poll.h ifaddrs.h net/if.h net/if_dl.h net/bpf.h net/ethernet.h
    netinet/if_ether.h netpacket/packet.h linux/if_packet.h sys/sockio.h sys/ioctl.h
    sys/un.h netinet/tcp.h sys/random.h execinfo.h syslog.h sys/slog2.h
    endian.h sys/endian.h byteswap.h gulliver.h sys/neutrino.h sys/syspage.h
    sys/procfs.h sys/dcmd_proc.h devctl.h
    openssl/evp.h mbedtls/version.h check.h libxml/parser.h)
  o6q_header(${h})
endforeach()

set(_sock "sys/types.h;sys/socket.h;netinet/in.h")
foreach(s SO_REUSEADDR SO_REUSEPORT SO_BINDTODEVICE SO_PRIORITY SO_TIMESTAMP SO_NOSIGPIPE
          SO_KEEPALIVE SO_RCVBUF SO_SNDBUF SO_ERROR SO_BROADCAST
          MSG_NOSIGNAL MSG_DONTWAIT SOCK_NONBLOCK SOCK_CLOEXEC
          accept4 recvmmsg sendmmsg socketpair
          IP_ADD_MEMBERSHIP IP_DROP_MEMBERSHIP IP_MULTICAST_IF IP_MULTICAST_LOOP IP_MULTICAST_TTL
          IP_PKTINFO IP_RECVDSTADDR IP_TOS
          IPV6_V6ONLY IPV6_JOIN_GROUP IPV6_LEAVE_GROUP IPV6_MULTICAST_IF IPV6_MULTICAST_LOOP
          IPV6_MULTICAST_HOPS IPV6_PKTINFO IPV6_RECVPKTINFO
          MCAST_JOIN_GROUP INADDR_LOOPBACK)
  o6q_symbol(${s} "${_sock}")
endforeach()
o6q_member("struct ip_mreq"  imr_interface "${_sock}")
o6q_member("struct ip_mreqn" imr_ifindex   "${_sock}")
o6q_member("struct ipv6_mreq" ipv6mr_interface "${_sock}")
o6q_member("struct sockaddr_in" sin_len "${_sock}")
o6q_member("struct sockaddr_storage" ss_len "${_sock}")

foreach(s TCP_NODELAY TCP_KEEPIDLE TCP_KEEPINTVL TCP_KEEPCNT TCP_KEEPALIVE TCP_MAXSEG)
  o6q_symbol(${s} "${_sock};netinet/tcp.h")
endforeach()

foreach(s if_nametoindex if_indextoname IFF_MULTICAST IFF_LOOPBACK IFF_RUNNING IFNAMSIZ)
  o6q_symbol(${s} "${_sock};net/if.h")
endforeach()
foreach(s getifaddrs freeifaddrs)
  o6q_symbol(${s} "${_sock};ifaddrs.h")
endforeach()
foreach(s getaddrinfo getnameinfo freeaddrinfo gai_strerror AI_PASSIVE AI_ADDRCONFIG AI_NUMERICHOST NI_NUMERICHOST)
  o6q_symbol(${s} "${_sock};netdb.h")
endforeach()

foreach(s select pselect FD_SETSIZE)
  o6q_symbol(${s} "sys/types.h;sys/time.h;sys/select.h")
endforeach()
foreach(s poll ppoll)
  o6q_symbol(${s} "poll.h")
endforeach()
foreach(s pipe pipe2 gethostname fcntl getpid usleep sysconf _SC_OPEN_MAX _SC_NPROCESSORS_ONLN getentropy)
  o6q_symbol(${s} "unistd.h;fcntl.h")
endforeach()
foreach(s arc4random arc4random_buf mkdtemp realpath)
  o6q_symbol(${s} "stdlib.h")
endforeach()
foreach(s strtok_r strerror_r strnlen strlcpy)
  o6q_symbol(${s} "string.h")
endforeach()
foreach(s vasprintf fmemopen open_memstream)
  o6q_symbol(${s} "stdio.h")
endforeach()

foreach(s clock_gettime clock_getres clock_nanosleep nanosleep gmtime_r localtime_r timegm
          CLOCK_REALTIME CLOCK_MONOTONIC CLOCK_MONOTONIC_RAW CLOCK_BOOTTIME CLOCK_TAI
          CLOCK_PROCESS_CPUTIME_ID CLOCK_THREAD_CPUTIME_ID timer_create)
  o6q_symbol(${s} "time.h")
endforeach()
foreach(s gettimeofday)
  o6q_symbol(${s} "sys/time.h")
endforeach()

foreach(s pthread_mutexattr_settype PTHREAD_MUTEX_RECURSIVE pthread_condattr_setclock
          pthread_cond_timedwait pthread_setname_np pthread_spin_lock pthread_rwlock_rdlock
          pthread_attr_setstacksize pthread_getattr_np pthread_once)
  o6q_symbol(${s} "pthread.h")
endforeach()
foreach(s sigaction sigwait SIGPIPE SIGINT SIGTERM signal)
  o6q_symbol(${s} "signal.h")
endforeach()

if(O6Q_HAVE_H_sys_neutrino_h)
  foreach(s ClockCycles ClockPeriod ClockTime MsgSend ChannelCreate ThreadCtl SchedGet)
    o6q_symbol(${s} "sys/neutrino.h")
  endforeach()
endif()
if(O6Q_HAVE_H_sys_slog2_h)
  foreach(s slog2_register slog2f slog2c slog2_set_default_buffer SLOG2_INFO SLOG2_ERROR SLOG2_DEBUG1)
    o6q_symbol(${s} "sys/slog2.h")
  endforeach()
endif()
if(O6Q_HAVE_H_syslog_h)
  foreach(s syslog openlog)
    o6q_symbol(${s} "syslog.h")
  endforeach()
endif()
if(O6Q_HAVE_H_sys_random_h)
  o6q_symbol(getrandom "sys/random.h")
endif()
if(O6Q_HAVE_H_execinfo_h)
  o6q_symbol(backtrace "execinfo.h")
endif()

# Run-time facts, printed on the target by vm/run.sh.
add_executable(o6q_envprobe envprobe.c)
target_link_libraries(o6q_envprobe PRIVATE ${_libs})
if(OPENSSL_FOUND)
  target_compile_definitions(o6q_envprobe PRIVATE O6Q_HAVE_OPENSSL=1)
  target_link_libraries(o6q_envprobe PRIVATE OpenSSL::Crypto)
endif()
