cmake_minimum_required(VERSION 3.20)

project(AYApplication
    VERSION 0.1.0
    DESCRIPTION "AYApplication — host-side SubSystem wiring + EventBus host cleanup"
)

set(SOURCES
    src/AYApplicationImpl.cpp
    src/AYAppEventHost.cpp
    src/AYEngineHost.cpp
    src/AYEngineHostScope.cpp
    src/AYRegisterDefaultModules.cpp
    src/AYSceneLifecycleEventBridge.cpp
)

set(HEADERS
    interface/IAYApplication.h
    interface/AYAppEventHost.h
    interface/IEngineHost.h
    interface/IHostPluginHooks.h
    interface/AYRegisterDefaultModules.h
    include/AYApplication.h
    include/AYSceneLifecycleEventBridge.h
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
    # INT-01 (2026-07-15): AYScript is linked PRIVATE because ApplicationImpl
    # is the only TU that references ScriptSubSystem. AYScript transitively
    # pulls AYEntity/AYReflect, so this effectively re-uses those libs without
    # leaking the Logia DSL / ScriptComponent API onto consumer link lines.
    PRIVATE AYAudio
    PRIVATE AYDeviceSubSystem
    PRIVATE AYScript
    PRIVATE AYResource
    PRIVATE AYTask
    # PR-6 (v0.1.3, design §10 Q-F 收口): 关卡生命周期管家 facade.
    # DEFAULT-ENGINE-HOST-INSIDE-ONLY: bindBuiltinHostServices 注册 SceneManager，
    # 但 AYScene 不暴露给 AYApplication 下游（AYEditor / AYEditorShell_Demo /
    # AYEditorShutdownRepro）的 link line —— 与 PRIVATE AYResource 模式一致。
    PRIVATE AYScene
    # P0 Client presentation option: RendererSubSystem registration.
    PRIVATE AYRenderer
    # Server/Client physics assembly (PhysicsSubSystem : ISubSystem).
    PRIVATE AYPhysics
    # Entity comes via AYScript (bootstrapEntityCore). EventBus via GameLoop.
    # AYResource: DefaultEngineHost::resources() / bindBuiltinHostServices.
    # AYScene: DefaultEngineHost::scenes() / bindBuiltinHostServices (PR-6 v0.1.3).
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
)

enable_testing()
add_subdirectory(unittest)

message(STATUS "AYApplication v0.1.0 (EngineHost services + EventBusHostScope) — see ${CMAKE_CURRENT_SOURCE_DIR}/docs/engine-host.md")
