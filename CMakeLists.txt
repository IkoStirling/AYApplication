cmake_minimum_required(VERSION 3.20)

project(AYApplication)

set(SOURCES
    src/AYApplicationImpl.cpp
)

set(HEADERS
    interface/IAYApplication.h
    include/AYApplication.h
)

add_library(${PROJECT_NAME} STATIC ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/interface
)

target_link_libraries(${PROJECT_NAME} PUBLIC
    AYCore
    AYGameLoop
    # AYAudio and AYDeviceSubSystem are linked PRIVATE so their libs (and their
    # own PRIVATE win32 deps) do not leak onto the link line of consumers
    # (AYEditor, AYEditorShell_Demo, AYEditorShutdownRepro). ApplicationImpl is
    # the only TU that references AudioSubSystem / DeviceSubSystem.
    PRIVATE AYAudio
    PRIVATE AYDeviceSubSystem
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
)

enable_testing()
add_subdirectory(unittest)