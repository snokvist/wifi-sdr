# SPDX-License-Identifier: GPL-2.0-only
# wifi-sdr host tools. Builds devourer (OpenIPC/devourer, GPL-2.0) as a subproject for chip bring-up,
# and assembles the on-chip MIPS16e2 programs with GNU binutils (apt: binutils-mipsel-linux-gnu).
cmake_minimum_required(VERSION 3.21)
project(wsdr C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# devourer: the pinned submodule (external/devourer) unless DEVOURER_DIR points elsewhere.
set(_wsdr_devourer ${CMAKE_CURRENT_SOURCE_DIR}/external/devourer)
if(NOT EXISTS ${_wsdr_devourer}/CMakeLists.txt)
  set(_wsdr_devourer "$ENV{HOME}/dev/snokvist/devourer")
endif()
set(DEVOURER_DIR "${_wsdr_devourer}" CACHE PATH "devourer source tree")
message(STATUS "wifi-sdr: devourer from ${DEVOURER_DIR}")
# Only the dies this project targets: 8822B (Jaguar2), 8822C / 8822E (Jaguar3).
foreach(opt JAGUAR1 8814 JAGUAR2_8821C 8733B KESTREL_8852B KESTREL_8852C)
  set(DEVOURER_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt JAGUAR2_8822B JAGUAR3_8822C JAGUAR3_8822E)
  set(DEVOURER_${opt} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${DEVOURER_DIR} devourer EXCLUDE_FROM_ALL)

# ---- on-chip programs: foo.S -> foo.bin (linked at the chip's load address) -> fw/foo.h (C array) ----
# 8822B programs run at IMEM 0x80000230 (after the vendor prologue); 8822C programs at 0x8003003c (after
# the vendor IMEM entry stub, src/cpu8822c.h).
find_program(MIPS_AS mipsel-linux-gnu-as REQUIRED)
find_program(MIPS_LD mipsel-linux-gnu-ld REQUIRED)
find_program(MIPS_OBJCOPY mipsel-linux-gnu-objcopy REQUIRED)
find_program(XXD xxd REQUIRED)
set(WSDR_FW_OUT ${CMAKE_CURRENT_BINARY_DIR}/fw)
file(MAKE_DIRECTORY ${WSDR_FW_OUT})
set(WSDR_FW_HEADERS)
function(wsdr_add_fw name text_addr)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/src/fw/${name}.S)
  add_custom_command(
    OUTPUT ${WSDR_FW_OUT}/${name}.h
    COMMAND ${MIPS_AS} -EL -march=mips32r2 -mips16 -mmips16e2 ${src} -o ${WSDR_FW_OUT}/${name}.o
    COMMAND ${MIPS_LD} -EL -Ttext=${text_addr} -e _start -o ${WSDR_FW_OUT}/${name}.elf ${WSDR_FW_OUT}/${name}.o
    COMMAND ${MIPS_OBJCOPY} -O binary -j .text ${WSDR_FW_OUT}/${name}.elf ${WSDR_FW_OUT}/${name}.bin
    COMMAND ${CMAKE_COMMAND} -E chdir ${WSDR_FW_OUT} ${XXD} -i ${name}.bin ${name}.h
    DEPENDS ${src}
    COMMENT "MIPS16e2 ${name} @ ${text_addr}")
  set(WSDR_FW_HEADERS ${WSDR_FW_HEADERS} ${WSDR_FW_OUT}/${name}.h PARENT_SCOPE)
endfunction()
foreach(fw marker8822b c2hbench8822b latime8822b stream8822b)
  wsdr_add_fw(${fw} 0x80000230)
endforeach()
foreach(fw marker8822c c2hbench8822c latime8822c stream8822c)
  wsdr_add_fw(${fw} 0x8003003c)
endforeach()
if(PROJECT_IS_TOP_LEVEL)
  add_custom_target(wsdr_fw ALL DEPENDS ${WSDR_FW_HEADERS})
else()
  add_custom_target(wsdr_fw DEPENDS ${WSDR_FW_HEADERS})
endif()

# ---- the receive core: both chips (8822B and 8822C streamers), consumed by the tools and by apps ----
add_library(wsdrcore STATIC src/rtlreg.cpp src/session.cpp src/la88xx.cpp src/cpu88xx.cpp src/c2h88xx.cpp
            src/stream88xx.cpp src/cpu8822c.cpp src/stream_cu.cpp src/receiver.cpp src/usbmode.cpp)
target_include_directories(wsdrcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${DEVOURER_DIR}/examples/common
                           ${WSDR_FW_OUT})
target_link_libraries(wsdrcore PUBLIC devourer)
add_dependencies(wsdrcore wsdr_fw)
target_compile_options(wsdrcore PRIVATE -Wall -Wextra)

find_package(Threads REQUIRED)
add_library(iqdsp STATIC src/iqdsp.cpp)
target_include_directories(iqdsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(iqdsp PRIVATE -Wall -Wextra)

# Tools, tests and the Soapy module only when wifi-sdr is the top-level project.
if(NOT PROJECT_IS_TOP_LEVEL)
  return()
endif()

add_executable(wsdr_probe src/wsdr_probe.cpp)
target_link_libraries(wsdr_probe wsdrcore)

add_executable(wsdr_rxtest src/wsdr_rxtest.cpp)
target_link_libraries(wsdr_rxtest wsdrcore iqdsp)

add_executable(iqdsp_test src/iqdsp_test.cpp)
target_link_libraries(iqdsp_test iqdsp)

add_executable(wsdr_tcp src/wsdr_tcp.cpp)
target_link_libraries(wsdr_tcp wsdrcore iqdsp Threads::Threads)

# 8822C (8812CU) custom-code tools (cmake/cu.cmake).
include(cmake/cu.cmake OPTIONAL)

# Optional SoapySDR driver module (cmake/soapy.cmake builds it when SoapySDR is found).
include(cmake/soapy.cmake OPTIONAL)
