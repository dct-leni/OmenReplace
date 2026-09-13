cmake_minimum_required(VERSION 3.20)

# --- Version Management (reads root VERSION file) ---
if(EXISTS "${CMAKE_SOURCE_DIR}/VERSION")
    file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" AMDOMEN_VERSION_RAW)
    string(STRIP "${AMDOMEN_VERSION_RAW}" AMDOMEN_VERSION)
else()
    set(AMDOMEN_VERSION "1.0.0")
endif()

# Parse Major.Minor.Patch
if(AMDOMEN_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(AMDOMEN_VER_MAJOR ${CMAKE_MATCH_1})
    set(AMDOMEN_VER_MINOR ${CMAKE_MATCH_2})
    set(AMDOMEN_VER_PATCH ${CMAKE_MATCH_3})
    set(AMDOMEN_VER_RC "${AMDOMEN_VER_MAJOR},${AMDOMEN_VER_MINOR},${AMDOMEN_VER_PATCH},0")
else()
    set(AMDOMEN_VER_RC "1,0,0,0")
endif()

project(AMDOMENTool VERSION ${AMDOMEN_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(UNICODE _UNICODE NOMINMAX)
add_compile_definitions(AMDOMEN_VERSION_STR="${AMDOMEN_VERSION}")

# Generate resource.rc with current version stamped
configure_file(
    "${CMAKE_SOURCE_DIR}/resource.rc.in"
    "${CMAKE_SOURCE_DIR}/resource.rc"
    @ONLY
)

option(OMEN_TELEMETRY "Enable CSV telemetry logging for debugging" OFF)
if(OMEN_TELEMETRY)
    add_compile_definitions(OMEN_TELEMETRY)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/output)

# --- Sources (HAL backend + native Win32 GUI) ---
file(GLOB_RECURSE SRC "src/*.cpp")

add_executable(AMDOMEN WIN32 ${SRC} "${CMAKE_SOURCE_DIR}/resource.rc")

target_include_directories(AMDOMEN PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/vendor"
)

# --- Windows libraries (EC/WMI/COM/GDI+) ---
target_link_libraries(AMDOMEN PRIVATE
    wbemuuid ole32 oleaut32 powrprof pdh psapi winmm comctl32 user32 gdi32 gdiplus advapi32 shell32 ws2_32 taskschd
)
