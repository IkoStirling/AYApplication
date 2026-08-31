#pragma once

#include <AYApplication/IEngineHost.h>
#include <AYModule/IModuleContext.h>

#include <string_view>

namespace ayt::app
{

// Adapts the process-scoped engine host to AYModule's dependency-neutral
// service lookup. The adapter never owns the host or any returned service.
class EngineModuleContext final : public ayt::module::IModuleContext
{
public:
    explicit EngineModuleContext(IEngineHost& host) noexcept;

    [[nodiscard]] void* findService(std::string_view key) const noexcept override;
    [[nodiscard]] IEngineHost& host() const noexcept;

private:
    IEngineHost& _host;
};

} // namespace ayt::app
