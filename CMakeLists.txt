cmake_minimum_required(VERSION 3.14)
project(OmenControlTool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE)

# Set output directory to ./output
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/output)

# --- Size Optimizations (Lightweight Binaries) ---
if(MINGW)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Os -ffunction-sections -fdata-sections -Wall -Wunused")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -s -Wl,--gc-sections -municode -static-libgcc -static-libstdc++ -specs=${CMAKE_SOURCE_DIR}/static_pthread.specs")
endif()

# --- Sources ---
file(GLOB_RECURSE SRC "src/*.cpp")
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/vendor/imgui")
set(IMGUI_SRC 
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
)

# --- Executable (Restored to OmenControl) ---
add_executable(OmenControl WIN32 ${SRC} ${IMGUI_SRC} "${CMAKE_SOURCE_DIR}/resource.rc")

# --- Include Directories ---
target_include_directories(OmenControl PRIVATE 
    "${CMAKE_SOURCE_DIR}/src"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

# --- Linking ---
target_link_libraries(OmenControl PRIVATE
    d3d11 dxgi d3dcompiler wbemuuid dwmapi imm32 ole32 oleaut32 powrprof pdh psapi winmm
)

# --- Install target ---
install(TARGETS OmenControl DESTINATION .)
