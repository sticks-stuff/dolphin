// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Config/AchievementSettings.h"

#include <string>

#include "Common/Config/Config.h"

namespace Config
{
// Configuration Information
const Info<bool> RA_ENABLED{{System::Achievements, "Achievements", "Enabled"}, false};
const Info<std::string> RA_USERNAME{{System::Achievements, "Achievements", "Username"}, ""};
const Info<std::string> RA_API_TOKEN{{System::Achievements, "Achievements", "ApiToken"}, ""};
const Info<bool> RA_ACHIEVEMENTS_ENABLED{
    {System::Achievements, "Achievements", "AchievementsEnabled"}, false};
const Info<bool> RA_LEADERBOARDS_ENABLED{
    {System::Achievements, "Achievements", "LeaderboardsEnabled"}, false};
const Info<bool> RA_RICH_PRESENCE_ENABLED{
    {System::Achievements, "Achievements", "RichPresenceEnabled"}, false};
const Info<bool> RA_UNOFFICIAL_ENABLED{{System::Achievements, "Achievements", "UnofficialEnabled"},
                                       false};
const Info<bool> RA_ENCORE_ENABLED{{System::Achievements, "Achievements", "EncoreEnabled"}, false};
}  // namespace Config
