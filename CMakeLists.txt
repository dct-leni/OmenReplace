cmake_minimum_required(VERSION 3.20)
project(OmenControlTool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE NOMINMAX)

option(OMEN_TELEMETRY "Enable CSV telemetry logging for debugging" OFF)
if(OMEN_TELEMETRY)
    add_compile_definitions(OMEN_TELEMETRY)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/output)

# --- Sources (HAL backend + native Win32 GUI) ---
file(GLOB_RECURSE SRC "src/*.cpp")

add_executable(OmenControl WIN32 ${SRC} "${CMAKE_SOURCE_DIR}/resource.rc")

target_include_directories(OmenControl PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/vendor"
)

# --- Windows libraries (EC/WMI/COM) ---
target_link_libraries(OmenControl PRIVATE
    wbemuuid ole32 oleaut32 powrprof pdh psapi winmm comctl32 user32 advapi32 shell32 ws2_32
)
