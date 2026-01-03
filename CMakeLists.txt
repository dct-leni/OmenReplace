cmake_minimum_required(VERSION 3.14)
project(OmenControlTool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE)

# --- Size Optimizations (Lightweight Binaries) ---
if(MINGW)
    # -Os: Optimize for size
    # -s: Strip all symbol table and relocation information (saves ~5MB)
    # -ffunction-sections -fdata-sections: Help linker remove unused code
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Os -ffunction-sections -fdata-sections")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s -Wl,--gc-sections -municode")
endif()

# --- Sources ---
file(GLOB_RECURSE SRC "src/*.cpp")
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/vendor/imgui")
set(IMGUI_SRC 
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
)

# --- Executable ---
add_executable(OmenReplace WIN32 ${SRC} ${IMGUI_SRC} "${CMAKE_SOURCE_DIR}/resource.rc")

# --- Include Directories ---
target_include_directories(OmenReplace PRIVATE 
    "${CMAKE_SOURCE_DIR}/src"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

# --- Linking ---
target_link_libraries(OmenReplace PRIVATE
    d3d11 dxgi d3dcompiler wbemuuid dwmapi imm32 ole32 oleaut32 powrprof pdh
)

# --- Custom Commands ---
# Clean log files before build
add_custom_command(TARGET OmenReplace PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E remove -f 
        "${CMAKE_SOURCE_DIR}/sensor_debug.log"
        "${CMAKE_SOURCE_DIR}/pawnio_debug.log"
    COMMENT "Cleaning debug log files"
)
