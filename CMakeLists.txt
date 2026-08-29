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
    src/AYRuntimeSceneLoader.cpp
    src/AYSceneLifecycleEventBridge.cpp
)

set(HEADERS
    interface/AYApplication/IApplication.h
    interface/AYApplication/AppEventHost.h
    interface/AYApplication/IEngineHost.h
    interface/AYApplication/IHostPluginHooks.h
    interface/AYApplication/RegisterDefaultModules.h
    interface/AYApplication/RuntimeSceneLoader.h
    
    include/AYApplication/SceneLifecycleEventBridge.h
)

add_library(${PROJECT_NAME} STATIC ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
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

# Shared content identity -> installed scene mapping. This deliberately stays
# independent of the full AYApplication target so server-side code need not
# depend on the client application bridge.
add_library(AYOnlineContent STATIC
    online/src/AYOnlineContent.cpp
    online/interface/AYOnlineApplication/OnlineContent.h
)
target_include_directories(AYOnlineContent PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/online/interface
)
target_link_libraries(AYOnlineContent PUBLIC
    AYNetwork
)
target_compile_options(AYOnlineContent PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
)

# Optional client application/network bridge. Keeping this as a separate
# target prevents ordinary AYApplication consumers from linking GNS and
# online backend dependencies.
add_library(AYOnlineApplication STATIC
    online/src/AYOnlineApplication.cpp
    online/interface/AYOnlineApplication/OnlineApplication.h
)
target_include_directories(AYOnlineApplication PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/online/interface
)
target_link_libraries(AYOnlineApplication PUBLIC
    AYApplication
    AYNetwork
    AYOnlineContent
    AYScene
)
target_compile_options(AYOnlineApplication PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
)

# Server-side scene/world integration is separate from the client application
# bridge. It runs headlessly; AYScene's current monolithic AYEntity dependency
# can still place presentation static libraries on the final link line.
add_library(AYDedicatedApplication STATIC
    online/src/AYDedicatedApplication.cpp
    online/interface/AYOnlineApplication/DedicatedApplication.h
)
target_include_directories(AYDedicatedApplication PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/online/interface
)
target_link_libraries(AYDedicatedApplication PUBLIC
    AYNetwork
    AYOnlineContent
    AYScene
)
target_compile_options(AYDedicatedApplication PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
)

# Scene-backed headless authority executable. AYNetwork keeps its generic
# runtime shell free of AYScene; this application-layer target performs the
# trusted content-ID -> local scene mapping and owns real Scene/World objects.
add_executable(AYApplication_DedicatedServer
    online/tools/dedicated_server/main.cpp
)
target_link_libraries(AYApplication_DedicatedServer PRIVATE
    AYDedicatedApplication
)
target_compile_options(AYApplication_DedicatedServer PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
)

enable_testing()
add_subdirectory(unittest)

message(STATUS "AYApplication v0.1.0 (EngineHost services + EventBusHostScope) — see ${CMAKE_CURRENT_SOURCE_DIR}/docs/engine-host.md")
