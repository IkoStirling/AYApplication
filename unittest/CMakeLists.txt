cmake_minimum_required(VERSION 3.20)

project(AYApplicationTest)

set(SOURCES
    TestMain.cpp
    ApplicationTest.cpp
    Test_EventBusHostScope.cpp
    Test_AudioSubsystemRegistration.cpp
    Test_ServerAndPhysicsModules.cpp
    Test_HostExtensionsAndEvents.cpp
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
    # Server/Physics assembly tests need PhysicsSubSystem symbols.
    AYPhysics
    # Host extension / Scene lifecycle event tests.
    AYScene
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>)

# -----------------------------------------------------------------------------
# bimg_decode / basisu_encoder tinyexr LNK2005 fix (mirror of AYEditor).
# -----------------------------------------------------------------------------
# Background: AYRenderer (PRIVATE) → bgfx::bimg_decode pulls tinyexr into
# AYApplicationTest's link line. AYResource transitively pulls
# basisu_encoder.lib (via assimp-vc145-mtd.lib), which also contains tinyexr.
# The linker errors on the duplicate tinyexr symbols even though both copies
# are bit-identical (bgfx vendors tinyexr; basisu ships a snapshot of the
# same code).
#
# The demos (AYSuzanneSkinned_Demo.exe, AYRenderer_Demo.exe) link the same
# pair without LNK2005 because they predate the AYApplication module.
# /FORCE:MULTIPLE + /IGNORE:4005,4006 is the narrowest workaround that
# touches only this test target and doesn't perturb AYRenderer's semantics
# for the working demos. See AYEditor/unittest/CMakeLists.txt for the
# original ship (2026-08-07 ish).
# -----------------------------------------------------------------------------
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        "/FORCE:MULTIPLE"
        "/IGNORE:4005"   # duplicate definition treated as warning
        "/IGNORE:4006"   # duplicate definition treated as warning
    )
endif()

# Register with ctest so `ctest` picks up the binary automatically.
enable_testing()
ay_configure_test_tmpdir(${PROJECT_NAME})
add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})

# -----------------------------------------------------------------------------
# EventBusScopeTest - standalone EventBusHostScope tests
# -----------------------------------------------------------------------------
# The main AYApplicationTest exe pulls in the full AYApplication library, which
# transitively drags AYAudio ? bgfx/bimg ? tinyexr symbols colliding with
# basisu_encoder.lib. That collision is pre-existing on baseline main and is
# unrelated to this commit. To verify EventBusHostScope independently, this
# target links ONLY the bits it needs (no AYApplication ? no AYAudio ? no bimg).
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

ay_configure_test_tmpdir(EventBusScopeTest)
add_test(NAME EventBusScopeTest COMMAND EventBusScopeTest)

# -----------------------------------------------------------------------------
# EngineHostTest - service registry contract (FakeHost; no DefaultEngineHost /
# AYResource/AYAudio � avoids tinyexr collision on this leaf exe).
# PR-6 (v0.1.3): Test_EngineHost.cpp 的 scenes() facade cases 调 SceneManager::instance()，
# 所以 EngineHostTest 必须 link AYScene（与 AYGameLoop/AYEventSystem 同形态 PRIVATE）。
# -----------------------------------------------------------------------------
add_executable(EngineHostTest
    TestMain.cpp
    Test_EngineHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/AYEngineHostScope.cpp
)

target_include_directories(EngineHostTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../interface
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYCore/include
)

target_link_libraries(EngineHostTest PRIVATE
    AYGameLoop
    AYEventSystem
    AYTest
    # PR-6 (v0.1.3): Test_EngineHost 调 SceneManager::instance() → AYScene 必需。
    AYScene
)

target_compile_options(EngineHostTest PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>)

ay_configure_test_tmpdir(EngineHostTest)
add_test(NAME EngineHostTest COMMAND EngineHostTest)
