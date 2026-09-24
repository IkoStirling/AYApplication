#pragma once

#include <AYApplication/GameProject.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/SaveGameService.h>

namespace ayt::app
{

/// Resolve the effective project path with Application command-line
/// precedence.
[[nodiscard]] inline std::string resolveGameUserDataPath(
    const GameProject& project, const AppCommandLine& commandLine = {})
{
    return resolveGameUserDataPath(
        project.id, project.userDataPath, commandLine.userDataPath);
}

/// Construct a standalone service for tools or tests. Games launched through
/// runGameProject receive an Application-owned instance from saveGameService.
[[nodiscard]] inline SaveGameService makeProjectSaveGameService(
    const GameProject& project, const AppCommandLine& commandLine = {})
{
    return SaveGameService({
        .userDataPath = resolveGameUserDataPath(project, commandLine),
        .projectId = project.id,
    });
}

/// Resolve the SaveGame service installed by runGameProject.
/// The returned pointer is borrowed and remains valid only for the active
/// Application/Host lifetime.
[[nodiscard]] inline SaveGameService* saveGameService(
    IEngineHost& host) noexcept
{
    return host.service<SaveGameService>(kHostServiceSaveGame);
}

} // namespace ayt::app
