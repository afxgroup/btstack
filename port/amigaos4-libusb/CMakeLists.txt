cmake_minimum_required (VERSION 3.12)

SET(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(BTstack-amigaos4-libusb)

SET(BTSTACK_ROOT ${CMAKE_SOURCE_DIR}/../..)

# AmigaOS 4 libusb-1.library uses inline macros
add_compile_definitions(__USE_INLINE__)

# extra compiler warnings
if ("${CMAKE_C_COMPILER_ID}" MATCHES ".*Clang.*")
    SET(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -Wunused-variable -Wswitch-default -Wall -Wextra -Werror -Werror=uninitialized")
elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "GNU")
    SET(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -Wunused-but-set-variable -Wunused-variable -Wswitch-default -Wall -Wextra")
elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "Intel")
    # using Intel C++
elseif ("${CMAKE_C_COMPILER_ID}" STREQUAL "MSVC")
    # using Visual Studio C++
endif()

# to generate .h from .gatt files
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# local dir for btstack_config.h after build dir to avoid using .h from Makefile
include_directories(.)

# libusb wrapper to expose <libusb.h> on AmigaOS 4 (SDK provides <libusb-1.h>)
include_directories(include)

include_directories(${BTSTACK_ROOT}/libusb-1/SDK/local/common/include)

include_directories(${BTSTACK_ROOT}/3rd-party/micro-ecc)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/include)
include_directories(${BTSTACK_ROOT}/3rd-party/lc3-google/include)
include_directories(${BTSTACK_ROOT}/3rd-party/md5)
include_directories(${BTSTACK_ROOT}/3rd-party/hxcmod-player)
include_directories(${BTSTACK_ROOT}/3rd-party/hxcmod-player/mod)
include_directories(${BTSTACK_ROOT}/3rd-party/lwip/core/src/include)
include_directories(${BTSTACK_ROOT}/3rd-party/lwip/dhcp-server)
include_directories(${BTSTACK_ROOT}/3rd-party/qr-code-generator)
include_directories(${BTSTACK_ROOT}/3rd-party/rijndael)
include_directories(${BTSTACK_ROOT}/3rd-party/yxml)
include_directories(${BTSTACK_ROOT}/3rd-party/tinydir)
include_directories(${BTSTACK_ROOT}/src)
include_directories(${BTSTACK_ROOT}/chipset/realtek)
include_directories(${BTSTACK_ROOT}/chipset/zephyr)
include_directories(${BTSTACK_ROOT}/platform/posix)
include_directories(${BTSTACK_ROOT}/platform/embedded)
include_directories(${BTSTACK_ROOT}/platform/lwip)
include_directories(${BTSTACK_ROOT}/platform/lwip/port)

file(GLOB SOURCES_SRC       "${BTSTACK_ROOT}/src/*.c" "${BTSTACK_ROOT}/example/*_demo_util*.c")
file(GLOB SOURCES_BLE       "${BTSTACK_ROOT}/src/ble/*.c")
file(GLOB SOURCES_GATT      "${BTSTACK_ROOT}/src/ble/gatt-service/*.c")
file(GLOB SOURCES_CLASSIC   "${BTSTACK_ROOT}/src/classic/*.c")
file(GLOB SOURCES_MESH      "${BTSTACK_ROOT}/src/mesh/*.c" "${BTSTACK_ROOT}/src/mesh/gatt-service/*.c")
file(GLOB SOURCES_BLUEDROID "${BTSTACK_ROOT}/3rd-party/bluedroid/encoder/srce/*.c" "${BTSTACK_ROOT}/3rd-party/bluedroid/decoder/srce/*.c")
file(GLOB SOURCES_MD5       "${BTSTACK_ROOT}/3rd-party/md5/md5.c")
file(GLOB SOURCES_UECC      "${BTSTACK_ROOT}/3rd-party/micro-ecc/uECC.c")
file(GLOB SOURCES_YXML      "${BTSTACK_ROOT}/3rd-party/yxml/yxml.c")
file(GLOB SOURCES_HXCMOD    "${BTSTACK_ROOT}/3rd-party/hxcmod-player/*.c"  "${BTSTACK_ROOT}/3rd-party/hxcmod-player/mods/*.c")
file(GLOB SOURCES_QRCODEGEN "${BTSTACK_ROOT}/3rd-party/qr-code-generator/qrcodegen.c")
file(GLOB SOURCES_RIJNDAEL  "${BTSTACK_ROOT}/3rd-party/rijndael/rijndael.c")
file(GLOB SOURCES_POSIX     "${BTSTACK_ROOT}/platform/posix/*.c")
# AmigaOS 4 port sources: HCI transport, run loop, console input, input.device.
# Applications living in this folder (they define btstack_main) must not end up
# in the library, or they would clash with every example.
file(GLOB SOURCES_LIBUSB    "${CMAKE_SOURCE_DIR}/*.c")
list(REMOVE_ITEM SOURCES_LIBUSB ${CMAKE_SOURCE_DIR}/bthid.c)
file(GLOB SOURCES_ZEPHYR    "${BTSTACK_ROOT}/chipset/zephyr/*.c")
file(GLOB SOURCES_REALTEK   "${BTSTACK_ROOT}/chipset/realtek/*.c")
file(GLOB SOURCES_LC3_GOOGLE "${BTSTACK_ROOT}/3rd-party/lc3-google/src/*.c")

set(LWIP_CORE_SRC
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/def.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/inet_chksum.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/init.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/ip.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/mem.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/memp.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/netif.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/pbuf.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/tcp.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/tcp_in.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/tcp_out.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/timeouts.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/udp.c
)
set (LWIP_IPV4_SRC
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/ipv4/acd.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/ipv4/dhcp.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/ipv4/etharp.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/ipv4/icmp.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/ipv4/ip4.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/ipv4/ip4_addr.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/core/ipv4/ip4_frag.c
)
set (LWIP_NETIF_SRC
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/netif/ethernet.c
)
set (LWIP_HTTPD
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/apps/http/altcp_proxyconnect.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/apps/http/fs.c
 ${BTSTACK_ROOT}/3rd-party/lwip/core/src/apps/http/httpd.c
)
set (LWIP_DHCPD
 ${BTSTACK_ROOT}/3rd-party/lwip/dhcp-server/dhserver.c
)
set (LWIP_PORT
 ${BTSTACK_ROOT}/platform/lwip/port/sys_arch.c
 ${BTSTACK_ROOT}/platform/lwip/bnep_lwip.c
)

set (SOURCES_LWIP ${LWIP_CORE_SRC} ${LWIP_IPV4_SRC} ${LWIP_NETIF_SRC} ${LWIP_HTTPD} ${LWIP_DHCPD} ${LWIP_PORT})

file(GLOB SOURCES_SRC_OFF "${BTSTACK_ROOT}/src/hci_transport_*.c")
list(REMOVE_ITEM SOURCES_SRC   ${SOURCES_SRC_OFF})

file(GLOB SOURCES_BLE_OFF "${BTSTACK_ROOT}/src/ble/le_device_db_memory.c")
list(REMOVE_ITEM SOURCES_BLE   ${SOURCES_BLE_OFF})

file(GLOB SOURCES_POSIX_OFF "${BTSTACK_ROOT}/platform/posix/le_device_db_fs.c")
list(REMOVE_ITEM SOURCES_POSIX ${SOURCES_POSIX_OFF})

# AmigaOS 4: exclude POSIX files that depend on host-specific features.
# btstack_run_loop_posix.c and btstack_stdin_posix.c are replaced by the native
# implementations in this folder: their select()/pipe() based waiting does not
# work on AmigaOS 4. btstack_signal.c needs the posix run loop's pipes.
list(REMOVE_ITEM SOURCES_POSIX
    ${BTSTACK_ROOT}/platform/posix/btstack_network_posix.c
    ${BTSTACK_ROOT}/platform/posix/btstack_audio_portaudio.c
    ${BTSTACK_ROOT}/platform/posix/btstack_audio_sdl2.c
    ${BTSTACK_ROOT}/platform/posix/btstack_sco_transport_posix_i2s_test_bridge.c
    ${BTSTACK_ROOT}/platform/posix/btstack_run_loop_posix.c
    ${BTSTACK_ROOT}/platform/posix/btstack_stdin_posix.c
    ${BTSTACK_ROOT}/platform/posix/btstack_signal.c
)

set(SOURCES
 ${SOURCES_MD5}
 ${SOURCES_YXML}
 ${SOURCES_BLUEDROID}
 ${SOURCES_POSIX}
 ${SOURCES_RIJNDAEL}
 ${SOURCES_LIBUSB}
 ${SOURCES_LC3_GOOGLE}
 ${SOURCES_SRC}
 ${SOURCES_BLE}
 ${SOURCES_GATT}
 ${SOURCES_MESH}
 ${SOURCES_CLASSIC}
 ${SOURCES_UECC}
 ${SOURCES_HXCMOD}
 ${SOURCES_QRCODEGEN}
 ${SOURCES_REALTEK}
 ${SOURCES_ZEPHYR}
)
list(SORT SOURCES)

# create static lib
add_library(btstack STATIC ${SOURCES})

# AmigaOS 4 libusb-1.library
# Note: no link stubs (libusb-1.a) needed - with __USE_INLINE__ (see above) all
# libusb_* calls go through the inline4 macros on ILibusb1, and main.c opens the
# library / gets the interface explicitly and defines the ILibusb1 global itself.

# pthread
find_package(Threads)
target_link_libraries(btstack ${CMAKE_THREAD_LIBS_INIT})

# libm
target_link_libraries(btstack m)

# get list of examples
include(${BTSTACK_ROOT}/example/CMakeLists.txt)
set (EXAMPLES ${EXAMPLES_GENERAL} ${EXAMPLES_CLASSIC_ONLY} ${EXAMPLES_LE_ONLY} ${EXAMPLES_DUAL_MODE})
list(REMOVE_DUPLICATES EXAMPLES)
list(REMOVE_ITEM EXAMPLES "avrcp_browsing_client")

# AmigaOS 4: exclude examples that need host audio, networking, or GATT generation issues
list(REMOVE_ITEM EXAMPLES
    audio_duplex
    audio_player
    mod_player
    sine_player
    pan_lwip_http_server
    a2dp_sink_demo
    a2dp_source_demo
    avrcp_browsing_client
    hfp_ag_demo
    hfp_hf_demo
    hsp_ag_demo
    hsp_hs_demo
    le_audio_broadcast_sink_lite
    le_audio_broadcast_source_lite
    le_audio_unicast_gateway_lite
    le_audio_unicast_headset_lite
    mesh_node_demo
)

# .gatt files in src
file(GLOB GATT_FILES "${BTSTACK_ROOT}/src/*/gatt-service/*.gatt")

# create targets
foreach(EXAMPLE ${EXAMPLES})
    # get c file
    set (SOURCES_EXAMPLE ${BTSTACK_ROOT}/example/${EXAMPLE}.c)

    # add GATT DB creation
    if ( "${EXAMPLES_GATT_FILES}" MATCHES ${EXAMPLE} )
        message("example ${EXAMPLE} -- with GATT DB")
        add_custom_command(
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${EXAMPLE}.h
   DEPENDS ${BTSTACK_ROOT}/example/${EXAMPLE}.gatt ${GATT_FILES}
   COMMAND ${Python3_EXECUTABLE} ${BTSTACK_ROOT}/tool/compile_gatt.py ${BTSTACK_ROOT}/example/${EXAMPLE}.gatt ${CMAKE_CURRENT_BINARY_DIR}/${EXAMPLE}.h
   )
        list(APPEND SOURCES_EXAMPLE ${CMAKE_CURRENT_BINARY_DIR}/${EXAMPLE}.h)
    endif()

    # add build target
    add_executable(${EXAMPLE} ${SOURCES_EXAMPLE})
    target_link_libraries(${EXAMPLE} btstack)
endforeach(EXAMPLE)

# AmigaOS 4 applications from this folder (not BTstack examples).
# Note: no GATT DB - bthid is a HID host and runs no ATT server, see bthid.c
add_executable(bthid ${CMAKE_SOURCE_DIR}/bthid.c)
target_link_libraries(bthid btstack)
