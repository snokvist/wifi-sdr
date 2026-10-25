# SPDX-License-Identifier: GPL-2.0-only
# wifi-sdr host tools. Builds devourer (OpenIPC/devourer, GPL-2.0) as a subproject for chip bring-up,
# and assembles the on-chip MIPS16e2 programs with GNU binutils (apt: binutils-mipsel-linux-gnu).
cmake_minimum_required(VERSION 3.16)
project(wsdr C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(DEVOURER_DIR "$ENV{HOME}/dev/snokvist/devourer" CACHE PATH "devourer source tree")
# Only the dies this project targets: 8822B (Jaguar2), 8822C / 8822E (Jaguar3).
foreach(opt JAGUAR1 8814 JAGUAR2_8821C 8733B KESTREL_8852B KESTREL_8852C)
  set(DEVOURER_${opt} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(opt JAGUAR2_8822B JAGUAR3_8822C JAGUAR3_8822E)
  set(DEVOURER_${opt} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${DEVOURER_DIR} devourer EXCLUDE_FROM_ALL)

# ---- on-chip programs: foo.S -> foo.bin (linked at 0x80000230) -> fw/foo.h (C array) ----
find_program(MIPS_AS mipsel-linux-gnu-as REQUIRED)
find_program(MIPS_LD mipsel-linux-gnu-ld REQUIRED)
find_program(MIPS_OBJCOPY mipsel-linux-gnu-objcopy REQUIRED)
find_program(XXD xxd REQUIRED)
set(FW_OUT ${CMAKE_BINARY_DIR}/fw)
file(MAKE_DIRECTORY ${FW_OUT})
set(FW_HEADERS)
function(add_fw name)
  set(src ${CMAKE_SOURCE_DIR}/src/fw/${name}.S)
  add_custom_command(
    OUTPUT ${FW_OUT}/${name}.h
    COMMAND ${MIPS_AS} -EL -march=mips32r2 -mips16 -mmips16e2 ${src} -o ${FW_OUT}/${name}.o
    COMMAND ${MIPS_LD} -EL -Ttext=0x80000230 -e _start -o ${FW_OUT}/${name}.elf ${FW_OUT}/${name}.o
    COMMAND ${MIPS_OBJCOPY} -O binary -j .text ${FW_OUT}/${name}.elf ${FW_OUT}/${name}.bin
    COMMAND ${CMAKE_COMMAND} -E chdir ${FW_OUT} ${XXD} -i ${name}.bin ${name}.h
    DEPENDS ${src}
    COMMENT "MIPS16e2 ${name}")
  list(APPEND FW_HEADERS ${FW_OUT}/${name}.h)
  set(FW_HEADERS ${FW_HEADERS} PARENT_SCOPE)
endfunction()
add_fw(marker8822b)
add_fw(c2hbench8822b)
add_custom_target(fw ALL DEPENDS ${FW_HEADERS})

add_library(wsdrcore STATIC src/rtlreg.cpp src/session.cpp src/la88xx.cpp src/cpu88xx.cpp src/c2h88xx.cpp)
target_include_directories(wsdrcore PUBLIC src ${DEVOURER_DIR}/examples/common ${FW_OUT})
target_link_libraries(wsdrcore PUBLIC devourer)
add_dependencies(wsdrcore fw)
target_compile_options(wsdrcore PRIVATE -Wall -Wextra)

add_executable(wsdr_probe src/wsdr_probe.cpp)
target_link_libraries(wsdr_probe wsdrcore)
