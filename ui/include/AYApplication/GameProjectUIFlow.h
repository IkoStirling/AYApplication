#pragma once

#include <string>

namespace ayt::app
{

class GameProject;

/// Declarative UIFlow presentation for a standalone GameProject client.
/// flowPath is relative to GameProject::assetRoot. Screen layout paths remain
/// relative to the directory containing the UIFlow document.
struct GameProjectUIFlowConfig
{
    std::string flowPath;
    std::string entry;
};

/// Adds the standard client UI presentation module to a GameProject while
/// preserving its existing game-module composition callback. The module owns
/// AYUI presentation, device input routing and the persistent UIFlow runtime.
void enableGameProjectUIFlow(
    GameProject& project,
    GameProjectUIFlowConfig config);

} // namespace ayt::app
