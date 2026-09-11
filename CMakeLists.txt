cmake_minimum_required(VERSION 3.20)

project(AYApplication
    VERSION 0.1.0
    DESCRIPTION "AYApplication — host-side SubSystem wiring and product assembly"
)

set(SOURCES
    src/AYApplicationImpl.cpp
    src/AYAppEventHost.cpp
    src/AYEngineModuleContext.cpp
    src/AYEngineModuleRuntime.cpp
    src/AYEngineHost.cpp
    src/AYEngineHostScope.cpp
    src/AYEngineRuntimeScope.cpp
    src/AYGameProject.cpp
    src/AYRegisterDefaultModules.cpp
    src/AYRuntimeSceneLoader.cpp
    src/AYRuntimeSceneLoaderModule.cpp
    src/AYSceneLifecycleEventBridge.cpp
)

set(HEADERS
    interface/AYApplication/IApplication.h
    interface/AYApplication/AppEventHost.h
    interface/AYApplication/EngineModuleContext.h
    interface/AYApplication/EngineModuleRuntime.h
    interface/AYApplication/IEngineHost.h
    interface/AYApplication/EngineRuntimeScope.h
    interface/AYApplication/GameProject.h
    interface/AYApplication/ProjectContentValidator.h
    interface/AYApplication/IHostPluginHooks.h
    interface/AYApplication/RegisterDefaultModules.h
    interface/AYApplication/RuntimeSceneLoader.h

    include/AYApplication/SceneLifecycleEventBridge.h
    include/AYApplication/RuntimeSceneLoaderModule.h
)

add_library(${PROJECT_NAME} STATIC ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/interface
)

set(AY_APPLICATION_HAS_AUDIO 0)
set(AY_APPLICATION_HAS_DEVICE 0)
set(AY_APPLICATION_HAS_PRESENTATION 0)
set(AY_APPLICATION_HAS_2D 0)
set(AY_APPLICATION_HAS_PHYSICS 0)
set(AY_APPLICATION_HAS_SCRIPT 0)
set(AY_APPLICATION_HAS_NETWORK 0)

set(_ay_application_private_libraries
    AYEntityCore
    AYResource
    AYTask
    AYScene
)

if(TARGET AYAudio)
    set(AY_APPLICATION_HAS_AUDIO 1)
    list(APPEND _ay_application_private_libraries AYAudio)
endif()
if(TARGET AYDeviceSubSystem)
    set(AY_APPLICATION_HAS_DEVICE 1)
    list(APPEND _ay_application_private_libraries AYDeviceSubSystem)
endif()
if(TARGET AYRenderer AND
   TARGET AYEntityAnimationIntegration AND
   TARGET AYEntityRenderIntegration)
    set(AY_APPLICATION_HAS_PRESENTATION 1)
    list(APPEND _ay_application_private_libraries
        AYRenderer
        AYEntityAnimationIntegration
        AYEntityRenderIntegration)
endif()
if(TARGET AYEntity2DIntegration)
    set(AY_APPLICATION_HAS_2D 1)
    list(APPEND _ay_application_private_libraries AYEntity2DIntegration)
endif()
if(TARGET AYPhysics AND TARGET AYEntityPhysicsIntegration)
    set(AY_APPLICATION_HAS_PHYSICS 1)
    list(APPEND _ay_application_private_libraries
        AYPhysics
        AYEntityPhysicsIntegration)
endif()
if(TARGET AYScript AND TARGET AYEntityScriptIntegration)
    set(AY_APPLICATION_HAS_SCRIPT 1)
    list(APPEND _ay_application_private_libraries
        AYScript
        AYEntityScriptIntegration)
endif()
if(TARGET AYNetwork)
    set(AY_APPLICATION_HAS_NETWORK 1)
endif()

target_link_libraries(${PROJECT_NAME}
    PUBLIC
        AYCore
        AYEventSystem
        AYGameLoop
        AY::Module
    PRIVATE
        ${_ay_application_private_libraries}
)

target_compile_definitions(${PROJECT_NAME} PUBLIC
    AY_APPLICATION_HAS_AUDIO=${AY_APPLICATION_HAS_AUDIO}
    AY_APPLICATION_HAS_DEVICE=${AY_APPLICATION_HAS_DEVICE}
    AY_APPLICATION_HAS_PRESENTATION=${AY_APPLICATION_HAS_PRESENTATION}
    AY_APPLICATION_HAS_2D=${AY_APPLICATION_HAS_2D}
    AY_APPLICATION_HAS_PHYSICS=${AY_APPLICATION_HAS_PHYSICS}
    AY_APPLICATION_HAS_SCRIPT=${AY_APPLICATION_HAS_SCRIPT}
    AY_APPLICATION_HAS_NETWORK=${AY_APPLICATION_HAS_NETWORK}
)

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
)

# Client-only application UI orchestration. Keeping this in a companion target
# preserves AYApplication's server/headless graph while making the persistent
# runtime an Application-owned service rather than an AYUI singleton.
if(TARGET AYUI)
    add_library(AYApplicationUI STATIC
        ui/src/AYUIFlowAssetValidation.cpp
        ui/src/AYUIFlowSceneBridge.cpp
        ui/src/AYUIFlowSceneBridgeModule.cpp
        ui/src/AYUIFlowSceneComponents.cpp
        ui/src/AYUIFlowRuntime.cpp
        ui/src/AYUIFlowRuntimeModule.cpp
        ui/src/AYUIManagerFlowScreenHost.cpp
        ui/include/AYApplication/UIFlowAssetValidation.h
        ui/include/AYApplication/UIFlowSceneBridge.h
        ui/include/AYApplication/UIFlowSceneBridgeModule.h
        ui/include/AYApplication/UIFlowSceneComponents.h
        ui/include/AYApplication/UIFlowRuntime.h
        ui/include/AYApplication/UIFlowRuntimeModule.h
        ui/include/AYApplication/UIManagerFlowScreenHost.h
        AYApplicationUI.h
    )
    target_include_directories(AYApplicationUI PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/include
    )
    target_link_libraries(AYApplicationUI PUBLIC
        AYApplication
        AYEntityCore
        AYUI
    )
    target_link_libraries(AYApplicationUI PRIVATE AYScene)
    target_compile_options(AYApplicationUI PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>: /W4>
        $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
    )
endif()

# Keep content inspection out of the runtime application library.  The
# full-client implementation may use AYUI loaders, while headless builds
# deliberately compile the same API without bringing UI into AYApplication.
find_package(nlohmann_json CONFIG REQUIRED)
add_library(AYProjectContentValidationCore STATIC
    src/AYProjectContentValidator.cpp
    interface/AYApplication/ProjectContentValidator.h)
target_include_directories(AYProjectContentValidationCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/interface)
target_link_libraries(AYProjectContentValidationCore
    PUBLIC AYScene AYResource
    PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(AYProjectContentValidationCore PRIVATE
    AY_APPLICATION_HAS_DEVICE=${AY_APPLICATION_HAS_DEVICE})
if(AY_APPLICATION_HAS_DEVICE AND TARGET AYUI)
    target_link_libraries(AYProjectContentValidationCore PRIVATE AYUI)
endif()
target_compile_options(AYProjectContentValidationCore PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>: /W4>
    $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>)

option(AY_BUILD_CONTENT_VALIDATOR
    "Build the independent project content validator" ${BUILD_TESTING})
if(AY_BUILD_CONTENT_VALIDATOR)
    add_executable(AYProjectContentValidator
        tools/project_content_validator/main.cpp)
    target_link_libraries(AYProjectContentValidator PRIVATE
        AYProjectContentValidationCore AYEntity)
    target_compile_options(AYProjectContentValidator PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>: /W4>
        $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>)
endif()

# A final-link and startup proof for the minimal build graph. Unlike building
# the AYApplication static library alone, this catches accidental references
# to a disabled runtime library. The target only exists when every optional
# application capability is absent.
if(NOT AY_APPLICATION_HAS_AUDIO AND
   NOT AY_APPLICATION_HAS_DEVICE AND
   NOT AY_APPLICATION_HAS_PRESENTATION AND
   NOT AY_APPLICATION_HAS_2D AND
   NOT AY_APPLICATION_HAS_PHYSICS AND
   NOT AY_APPLICATION_HAS_SCRIPT AND
   NOT AY_APPLICATION_HAS_NETWORK)
    add_executable(AYApplication_HeadlessSmoke
        tools/headless_smoke/main.cpp
    )
    target_link_libraries(AYApplication_HeadlessSmoke PRIVATE
        AYApplication
    )
    target_compile_options(AYApplication_HeadlessSmoke PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>: /W4>
        $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
    )
    if(BUILD_TESTING)
        add_test(NAME AYApplication_HeadlessSmoke
            COMMAND AYApplication_HeadlessSmoke)
    endif()
endif()

# Shared content identity -> installed scene mapping. This deliberately stays
# independent of the full AYApplication target so server-side code need not
# depend on the client application bridge.
if(TARGET AYNetwork)
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
    online/src/AYOnlineApplicationRuntimeModule.cpp
    online/interface/AYOnlineApplication/OnlineApplication.h
    online/interface/AYOnlineApplication/OnlineApplicationRuntimeModule.h
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
# bridge. It runs headlessly and links only Entity core plus the explicit
# Network integration layer.
add_library(AYDedicatedApplication STATIC
    online/src/AYDedicatedApplication.cpp
    online/interface/AYOnlineApplication/DedicatedApplication.h
)
target_include_directories(AYDedicatedApplication PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/online/interface
)
target_link_libraries(AYDedicatedApplication PUBLIC
    AYNetwork
    AYEntityNetworkIntegration
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
endif()

if(BUILD_TESTING AND
   (AY_APPLICATION_HAS_AUDIO OR AY_APPLICATION_HAS_DEVICE OR
    AY_APPLICATION_HAS_PRESENTATION OR AY_APPLICATION_HAS_2D OR
    AY_APPLICATION_HAS_PHYSICS OR AY_APPLICATION_HAS_SCRIPT OR
    AY_APPLICATION_HAS_NETWORK))
    enable_testing()
    add_subdirectory(unittest)

    # Kept here instead of appending to unittest/CMakeLists.txt so the
    # optional client-only test follows the same target gate as
    # AYApplicationUI without coupling the existing headless test graph.
    if(TARGET AYApplicationUI)
        add_executable(UIFlowRuntimeTest
            unittest/TestMain.cpp
            unittest/Test_UIFlowRuntime.cpp
            unittest/Test_UIFlowSceneBridge.cpp
        )
        target_link_libraries(UIFlowRuntimeTest PRIVATE
            AYApplicationUI
            AYEntityCore
            AYScene
            AYTest
        )
        target_compile_options(UIFlowRuntimeTest PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>: /W4>
            $<$<CXX_COMPILER_ID:GNU,Clang>: -Wall -Wextra -Wpedantic>
        )
        target_compile_definitions(UIFlowRuntimeTest PRIVATE
            AY_APPLICATION_UI_TEST_ASSET_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/unittest/assets")
        if(MSVC)
            target_link_options(UIFlowRuntimeTest PRIVATE
                "/FORCE:MULTIPLE"
                "/IGNORE:4005"
                "/IGNORE:4006")
        endif()
        ay_add_test(UIFlowRuntimeTest LABELS "ui-flow;ayapplication")
    endif()
endif()

message(STATUS "AYApplication v0.1.0 (EngineHost services + module assembly) — see ${CMAKE_CURRENT_SOURCE_DIR}/docs/engine-host.md")
