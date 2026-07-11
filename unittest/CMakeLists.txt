cmake_minimum_required(VERSION 3.20)

project(AYApplicationTest)

set(SOURCES
    TestMain.cpp
    ApplicationTest.cpp
    Test_AudioSubsystemRegistration.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYCore/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    AYApplication
    AYTest
    # The integration test uses AudioSubSystem + MiniaudioBackend. Linking
    # AYAudio PRIVATE here is fine because the test executable is a leaf
    # target (no consumers downstream).
    AYAudio
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>)

# Register with ctest so `ctest` picks up the binary automatically.
enable_testing()
add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})