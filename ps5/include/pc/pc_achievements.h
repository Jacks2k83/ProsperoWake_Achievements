#pragma once
// PS5 achievements: checked from the loaded save while the game is played,
// stored in user/achievements.txt (never in the game's save) and announced
// with an in-game banner, the game's item jingle and a console notification.
#include <imgui.h>

struct PcAchievementInfo {
    const char* title;       // English key; translate with pc_menu_text()
    const char* description; // English key; translate with pc_menu_text()
    bool unlocked;
    int progress;            // current value, or -1 when the achievement has no counter
    int goal;
};

// Once per presented frame, inside the ImGui frame: checks the save and draws the banner.
void pc_achievements_frame();
int pc_achievements_count();
int pc_achievements_unlocked();
PcAchievementInfo pc_achievements_at(int index);
// One row of the Achievements page: icon, title, description and state. Returns true when clicked.
bool pc_achievements_row(int index, bool selected, const ImVec4& dim, const ImVec4& accent);
// Developer tools: plays the unlock banner, jingle and notification without unlocking anything.
void pc_achievements_test_notice();
