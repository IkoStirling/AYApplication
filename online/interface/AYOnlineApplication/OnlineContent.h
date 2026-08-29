#pragma once
// Trusted local mapping from backend content identities to installed scenes.

#include <AYNetwork/OnlineServices.h>

#include <string>
#include <vector>

namespace ayt::app::online
{

struct OnlineContentResolveResult {
    std::string scenePath;
    std::string sceneName;
    std::string message;

    bool isValid() const { return !scenePath.empty(); }
};

class IOnlineContentResolver {
public:
    virtual ~IOnlineContentResolver() = default;
    virtual OnlineContentResolveResult resolve(
        const ::ayt::net::OnlineContentDescriptor& content) const = 0;
};

struct OnlineContentMapping {
    std::string contentId;
    std::string contentVersion;
    std::string scenePath;
    std::string sceneName;

    bool isValid() const;
};

// Small exact-version catalog suitable for the first integration. Projects
// can replace it with an asset manifest/downloader by implementing the same
// resolver interface.
class OnlineContentCatalog final : public IOnlineContentResolver {
public:
    bool addOrReplace(OnlineContentMapping mapping);
    OnlineContentResolveResult resolve(
        const ::ayt::net::OnlineContentDescriptor& content) const override;

private:
    std::vector<OnlineContentMapping> _mappings;
};

} // namespace ayt::app::online
