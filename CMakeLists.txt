cmake_minimum_required(VERSION 3.16)
project(wallp VERSION 1.0.0 LANGUAGES C CXX)

if(NOT MSVC)
  message(FATAL_ERROR "wallp targets Windows and is built with MSVC.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT (/MT, /MTd for Debug).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- Built-in shaders (fullscreen-triangle VS + blit PS), compiled offline with fxc ----
set(_fxc_hints "")
if(DEFINED ENV{WindowsSdkVerBinPath})
  list(APPEND _fxc_hints "$ENV{WindowsSdkVerBinPath}/x64")
endif()
file(GLOB _sdk_bin_dirs LIST_DIRECTORIES true "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
list(SORT _sdk_bin_dirs)
list(REVERSE _sdk_bin_dirs)
foreach(_d IN LISTS _sdk_bin_dirs)
  list(APPEND _fxc_hints "${_d}/x64")
endforeach()
find_program(FXC_EXECUTABLE fxc HINTS ${_fxc_hints})
if(NOT FXC_EXECUTABLE)
  message(FATAL_ERROR "fxc.exe (Windows SDK) not found. Run from a VS developer prompt or set FXC_EXECUTABLE.")
endif()

set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")
set(BUILTIN_HLSL "${CMAKE_SOURCE_DIR}/src/builtin.hlsl")
add_custom_command(
  OUTPUT "${GEN_DIR}/builtin_vs.h"
  COMMAND "${FXC_EXECUTABLE}" /nologo /T vs_4_0 /E VSMain /O3 /Vn g_builtinVS /Fh "${GEN_DIR}/builtin_vs.h" "${BUILTIN_HLSL}"
  DEPENDS "${BUILTIN_HLSL}"
  COMMENT "fxc VSMain"
  VERBATIM)
add_custom_command(
  OUTPUT "${GEN_DIR}/builtin_blit.h"
  COMMAND "${FXC_EXECUTABLE}" /nologo /T ps_4_0 /E PSBlit /O3 /Vn g_builtinBlitPS /Fh "${GEN_DIR}/builtin_blit.h" "${BUILTIN_HLSL}"
  DEPENDS "${BUILTIN_HLSL}"
  COMMENT "fxc PSBlit"
  VERBATIM)

# ---- Executable ----
add_executable(wallp WIN32
  src/main.cpp
  src/util.cpp
  src/config.cpp
  src/desktop.cpp
  src/detect.cpp
  src/policy.cpp
  src/renderer.cpp
  src/shader.cpp
  src/tray.cpp
  third_party/yyjson/yyjson.c
  resources/wallp.rc
  resources/wallp.manifest
  "${GEN_DIR}/builtin_vs.h"
  "${GEN_DIR}/builtin_blit.h")

target_include_directories(wallp PRIVATE src resources third_party/yyjson "${GEN_DIR}")

target_compile_definitions(wallp PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000006
  YYJSON_DISABLE_INCR_READER=1 YYJSON_DISABLE_UTILS=1 YYJSON_DISABLE_FILE=1)

target_compile_options(wallp PRIVATE
  /utf-8
  $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /Zc:__cplusplus>
  $<$<CONFIG:Release>:/O2 /GL /Gy /Gw>)

target_link_options(wallp PRIVATE
  /DELAYLOAD:pdh.dll
  "$<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>")

target_link_libraries(wallp PRIVATE
  d3d11 dxgi dwmapi pdh wtsapi32 shell32 user32 gdi32 advapi32 ole32 delayimp)

# Portable layout: wallp.exe next to a shaders/ folder. A separate target so that edited
# shaders are copied even when nothing needs relinking (copy_directory overwrites, so
# do not edit build/shaders and the source shaders/ at the same time).
add_custom_target(copy_shaders ALL
  COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:wallp>/shaders"
  COMMENT "Copying shaders"
  VERBATIM)
add_dependencies(copy_shaders wallp)

install(TARGETS wallp RUNTIME DESTINATION .)
install(DIRECTORY shaders/ DESTINATION shaders)
