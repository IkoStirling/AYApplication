cmake_minimum_required(VERSION 3.20)

project(AYApplicationTest)

set(SOURCES
    TestMain.cpp
    ApplicationTest.cpp
    Test_EventBusHostScope.cpp
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
ay_configure_test_tmpdir(${PROJECT_NAME})
add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})

# -----------------------------------------------------------------------------
# EventBusScopeTest - standalone EventBusHostScope tests
# -----------------------------------------------------------------------------
# The main AYApplicationTest exe pulls in the full AYApplication library, which
# transitively drags AYAudio â†?bgfx/bimg â†?tinyexr symbols colliding with
# basisu_encoder.lib. That collision is pre-existing on baseline main and is
# unrelated to this commit. To verify EventBusHostScope independently, this
# target links ONLY the bits it needs (no AYApplication â†?no AYAudio â†?no bimg).
# It compiles AYAppEventHost.cpp directly from the lib source so we don't have
# to expose it as a separate library.
# -----------------------------------------------------------------------------
add_executable(EventBusScopeTest
    TestMain.cpp
    Test_EventBusHostScope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/AYAppEventHost.cpp
)

target_include_directories(EventBusScopeTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYCore/include
)

target_link_libraries(EventBusScopeTest PRIVATE
    AYEventSystem
    AYTest
)

target_compile_options(EventBusScopeTest PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>)

ay_configure_test_tmpdir(${PROJECT_NAME})
add_test(NAME EventBusScopeTest COMMAND EventBusScopeTest)