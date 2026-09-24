#pragma once

#include "../MotionWallpaper.Common/Common.h"
#include "../MotionWallpaper.Common/DisplayTopology.h"
#include <algorithm>

namespace motion::app
{
    // Browsing is deliberately outside this command. Only an explicit Apply
    // may change assignments; a disconnected target must never become "all".
    [[nodiscard]] inline bool apply_wallpaper_assignment(Settings& settings,
        std::string const& groupId, std::string const& mediaId,
        std::string const& targetId, std::vector<DisplayTarget> const& connected)
    {
        if (!valid_id(groupId) || !valid_id(mediaId) || connected.empty()) return false;
        if (!targetId.empty() && std::none_of(connected.begin(), connected.end(),
            [&](auto const& display) { return display.id == targetId; })) return false;
        settings.displayMode = "independent";
        if (targetId.empty()) {
            settings.selectedGroupId = groupId;
            settings.selectedMediaId = mediaId;
            settings.displayAssignments.clear();
        } else {
            auto entry = std::find_if(settings.displayAssignments.begin(), settings.displayAssignments.end(),
                [&](auto const& value) { return value.displayId == targetId; });
            if (entry == settings.displayAssignments.end())
                settings.displayAssignments.push_back({ targetId, groupId, mediaId });
            else { entry->groupId = groupId; entry->mediaId = mediaId; }
        }
        return true;
    }
}
