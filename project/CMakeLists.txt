cmake_minimum_required(VERSION 3.20)
project(DSERecomp CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(RECOMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
file(GLOB PPC_SOURCES ${RECOMP_ROOT}/ppc/ppc_recomp.*.cpp ${RECOMP_ROOT}/ppc/ppc_func_mapping.cpp)

# Recompiled game code (very large): its own library with its own flags.
# DSE_PPC_OPT: /O1 builds faster, /O2 runs faster.
add_library(DSERecompLib STATIC ${PPC_SOURCES})
target_include_directories(DSERecompLib PUBLIC ${RECOMP_ROOT}/ppc ${RECOMP_ROOT}/XenonRecomp/thirdparty/simde)
set(DSE_PPC_OPT "/O1" CACHE STRING "Optimization of the recompiled PPC code")
target_compile_options(DSERecompLib PRIVATE ${DSE_PPC_OPT} -fno-strict-aliasing -msse4.1 -Wno-everything -gline-tables-only /Z7
    "/FI${RECOMP_ROOT}/tools/ppc_indirect.h")

add_executable(DSE
    ${RECOMP_ROOT}/runtime/main.cpp
    ${RECOMP_ROOT}/runtime/updater.cpp
    ${RECOMP_ROOT}/runtime/boot.cpp
    ${RECOMP_ROOT}/runtime/hooks.cpp
    ${RECOMP_ROOT}/runtime/ios.cpp
    ${RECOMP_ROOT}/runtime/ios_devices.cpp
    ${RECOMP_ROOT}/runtime/threads.cpp
    ${RECOMP_ROOT}/runtime/gx.cpp
    ${RECOMP_ROOT}/runtime/video.cpp
    ${RECOMP_ROOT}/runtime/disc.cpp
    ${RECOMP_ROOT}/runtime/gx_vertex.cpp
    ${RECOMP_ROOT}/runtime/gx_texture.cpp
    ${RECOMP_ROOT}/runtime/gx_d3d12.cpp
    ${RECOMP_ROOT}/runtime/input.cpp
    ${RECOMP_ROOT}/runtime/audio.cpp
    ${RECOMP_ROOT}/runtime/ax.cpp
    ${RECOMP_ROOT}/runtime/config.cpp
    ${RECOMP_ROOT}/runtime/bindings.cpp
    ${RECOMP_ROOT}/runtime/launcher.cpp
    ${RECOMP_ROOT}/runtime/netplay.cpp
    ${RECOMP_ROOT}/runtime/dse.rc)
target_include_directories(DSE PRIVATE ${RECOMP_ROOT}/runtime)
target_compile_options(DSE PRIVATE /O2 -fno-strict-aliasing -msse4.1 -Wno-unused-parameter /Z7 "/FI${RECOMP_ROOT}/tools/ppc_indirect.h")
target_compile_definitions(DSE PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(DSE PRIVATE DSERecompLib ws2_32 winhttp user32 gdi32 dbghelp d3d12 dxgi d3dcompiler xinput ole32 dwmapi shell32 winmm)
# Windowed app: no console window (the log goes to logs/juego.log, see main.cpp).
target_link_options(DSE PRIVATE /DEBUG /STACK:0x4000000 /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
