#include <AYOnlineApplication/OnlineContent.h>

#include <algorithm>
#include <utility>

namespace ayt::app::online
{
namespace
{

constexpr size_t kMaximumScenePathLength = 4096;
constexpr size_t kMaximumSceneNameLength = 256;

} // namespace

bool OnlineContentMapping::isValid() const {
    return !contentId.empty() && contentId.size() <= 128 &&
           !contentVersion.empty() && contentVersion.size() <= 64 &&
           !scenePath.empty() && scenePath.size() <= kMaximumScenePathLength &&
           sceneName.size() <= kMaximumSceneNameLength;
}

bool OnlineContentCatalog::addOrReplace(OnlineContentMapping mapping) {
    if (!mapping.isValid()) return false;
    const auto found = std::find_if(
        _mappings.begin(), _mappings.end(), [&](const auto& current) {
            return current.contentId == mapping.contentId &&
                   current.contentVersion == mapping.contentVersion;
        });
    if (found == _mappings.end()) {
        _mappings.push_back(std::move(mapping));
    } else {
        *found = std::move(mapping);
    }
    return true;
}

OnlineContentResolveResult OnlineContentCatalog::resolve(
    const ::ayt::net::OnlineContentDescriptor& content) const {
    if (!content.isValid()) {
        return {.message = "Online content descriptor is invalid"};
    }
    const auto found = std::find_if(
        _mappings.begin(), _mappings.end(), [&](const auto& mapping) {
            return mapping.contentId == content.contentId &&
                   mapping.contentVersion == content.contentVersion;
        });
    if (found == _mappings.end()) {
        return {.message = "Content is not installed: " + content.contentId +
                           "@" + content.contentVersion};
    }
    return {
        .scenePath = found->scenePath,
        .sceneName = found->sceneName,
    };
}

} // namespace ayt::app::online
