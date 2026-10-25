# SPDX-License-Identifier: GPL-2.0-only
# 8822C (RTL8812CU) custom-code tools: wsdr_cu + its on-chip programs. Programs link at 0x8003003c,
# directly after the vendor 8822C IMEM entry stub (src/cpu8822c.h); add_fw() in CMakeLists.txt is the
# 8822B rule (0x80000230).
function(add_fw_8822c name)
  set(src ${CMAKE_SOURCE_DIR}/src/fw/${name}.S)
  add_custom_command(
    OUTPUT ${FW_OUT}/${name}.h
    COMMAND ${MIPS_AS} -EL -march=mips32r2 -mips16 -mmips16e2 ${src} -o ${FW_OUT}/${name}.o
    COMMAND ${MIPS_LD} -EL -Ttext=0x8003003c -e _start -o ${FW_OUT}/${name}.elf ${FW_OUT}/${name}.o
    COMMAND ${MIPS_OBJCOPY} -O binary -j .text ${FW_OUT}/${name}.elf ${FW_OUT}/${name}.bin
    COMMAND ${CMAKE_COMMAND} -E chdir ${FW_OUT} ${XXD} -i ${name}.bin ${name}.h
    DEPENDS ${src}
    COMMENT "MIPS16e2 ${name} (8822C)")
  set(FW_CU_HEADERS ${FW_CU_HEADERS} ${FW_OUT}/${name}.h PARENT_SCOPE)
endfunction()

set(FW_CU_HEADERS)
add_fw_8822c(marker8822c)
add_fw_8822c(c2hbench8822c)
add_custom_target(fw_cu DEPENDS ${FW_CU_HEADERS})

add_executable(wsdr_cu src/wsdr_cu.cpp src/cpu8822c.cpp)
target_link_libraries(wsdr_cu wsdrcore)
add_dependencies(wsdr_cu fw_cu)
target_compile_options(wsdr_cu PRIVATE -Wall -Wextra)
