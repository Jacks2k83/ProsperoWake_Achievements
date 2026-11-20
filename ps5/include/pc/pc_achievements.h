#pragma once
// PS5 achievements: checked from the loaded save while the game is played,
// stored in user/achievements.txt (never in the game's save) and announced
// with an in-game banner and, optionally, a console notification.

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
