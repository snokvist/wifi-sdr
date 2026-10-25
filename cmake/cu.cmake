# SPDX-License-Identifier: GPL-2.0-only
# 8822C (RTL8812CU) custom-code tool wsdr_cu. Its on-chip programs (linked at 0x8003003c, after the vendor
# 8822C IMEM entry stub) and the 8822C streamer are part of wsdrcore (CMakeLists.txt).
add_executable(wsdr_cu ${CMAKE_CURRENT_SOURCE_DIR}/src/wsdr_cu.cpp)
target_link_libraries(wsdr_cu wsdrcore)
target_compile_options(wsdr_cu PRIVATE -Wall -Wextra)
