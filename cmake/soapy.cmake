# SPDX-License-Identifier: GPL-2.0-only
# Optional SoapySDR driver module "wsdr" (src/soapy/SoapyWsdr.cpp). Opt-in, because a MODULE needs
# position-independent code in wsdrcore, iqdsp and devourer, which rebuilds them:
#   cmake -S . -B build-soapy -DWSDR_SOAPY=ON && cmake --build build-soapy -j --target wsdrSupport
#   SOAPY_SDR_PLUGIN_PATH=$PWD/build-soapy SoapySDRUtil --find=driver=wsdr
option(WSDR_SOAPY "Build the SoapySDR module (libwsdrSupport.so)" OFF)
if(NOT WSDR_SOAPY)
  return()
endif()

find_package(SoapySDR CONFIG)
if(NOT SoapySDR_FOUND)
  message(WARNING "WSDR_SOAPY=ON but SoapySDR was not found (apt: libsoapysdr-dev); skipping the module")
  return()
endif()
find_package(Threads REQUIRED)

set_target_properties(devourer wsdrcore iqdsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(COMMAND SOAPY_SDR_MODULE_UTIL)
  SOAPY_SDR_MODULE_UTIL(
    TARGET wsdrSupport
    SOURCES ${CMAKE_SOURCE_DIR}/src/soapy/SoapyWsdr.cpp
    LIBRARIES wsdrcore iqdsp Threads::Threads)
else()
  add_library(wsdrSupport MODULE ${CMAKE_SOURCE_DIR}/src/soapy/SoapyWsdr.cpp)
  target_link_libraries(wsdrSupport PRIVATE wsdrcore iqdsp Threads::Threads SoapySDR)
  set_target_properties(wsdrSupport PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endif()
target_compile_options(wsdrSupport PRIVATE -Wall -Wextra -Wno-cpp) # -Wno-cpp: SoapySDR headers include <ciso646> (deprecated in C++20)
# Keep devourer/wsdrcore symbols private to the module (other Soapy modules share the process).
target_link_options(wsdrSupport PRIVATE -Wl,--exclude-libs,ALL)
