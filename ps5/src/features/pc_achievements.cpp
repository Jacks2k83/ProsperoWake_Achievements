// PS5 achievements (phase 1: everything that can be read from the loaded save).
//
// Checked a few times a second while the PLAY scene runs, so unlocks are also
// retroactive: loading a save that already has an item unlocks its achievement.
// Unlocks live in user/achievements.txt beside settings.ini, one "id time" line
// each, and never touch the game's own save. Titles follow the RetroAchievements
// set for the game (126 achievements); challenge and side-quest entries come later.
#include "pc/pc_achievements.h"
#include "pc/pc_menu_language.h"
#include "pc/pc_settings.h"
#include "pc/pc_ui_style.h"
#include "pc_internal.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>

#if defined(__PROSPERO__)
extern "C" int sceKernelSendNotificationRequest(int device, void* request, size_t size, int blocking);
#endif

namespace {
struct Progress { int value, goal; };
struct Achievement {
    const char* id;
    const char* title;
    const char* description;
    bool (*check)();
    Progress (*progress)();
};

// Song bits of dSv_player_collect_c (dComIfGs_isTact), as d_com_inf_game.cpp's checkItemGet reads them.
enum Song { kWindsRequiem, kBalladOfGales, kCommandMelody, kEarthGodsLyric, kWindGodsAria, kSongOfPassing };

bool has(int slot) { return dComIfGs_getItem(slot) != dItemNo_NONE_e; }
int bottles() {
    int count = 0;
    for (int i = 0; i < dInvSlot_BOTTLE_COUNT_e; ++i) count += has(dInvSlot_BOTTLE0_e + i);
    return count;
}
bool sword(int level) {
    for (int i = level; i <= 3; ++i) if (dComIfGs_isCollect(0, i)) return true;
    return false;
}
int hearts() { return dComIfGs_getMaxLife() / 4; }
bool arrows(bool light) {
    const u8 bow = dComIfGs_getItem(dInvSlot_BOW_e);
    return bow == dItemNo_LIGHT_ARROW_e || (!light && bow == dItemNo_MAGIC_ARROW_e);
}
void* findScene(void* proc, void* name) {
    return fpcM_GetName(proc) == *static_cast<s16*>(name) ? proc : nullptr;
}
bool sceneRunning(s16 name) { return fpcM_Search(findScene, &name) != nullptr; }

const Achievement kAchievements[] = {
    {"bait_bag", "Ready for Hunting", "Obtain the Bait Bag.", [] { return has(dInvSlot_BAIT_BAG_e); }, nullptr},
    {"wind_waker", "Maestro of the Mistral", "Obtain the Wind Waker.", [] { return has(dInvSlot_WIND_WAKER_e); }, nullptr},
    {"bottle", "Bottom's Up", "Obtain a bottle.", [] { return bottles() >= 1; }, nullptr},
    {"grappling_hook", "Rope a Dope", "Obtain the Grappling Hook.", [] { return has(dInvSlot_GRAPPLING_HOOK_e); }, nullptr},
    {"pearl_din", "Jewel of Power", "Obtain Din's Pearl.", [] { return bool(dComIfGs_isSymbol(dSymbol_DIN_e)); }, nullptr},
    {"winds_requiem", "Blown Away", "Learn Wind's Requiem.", [] { return bool(dComIfGs_isTact(kWindsRequiem)); }, nullptr},
    {"song_of_passing", "From Dusk 'til Dawn", "Learn the Song of Passing.", [] { return bool(dComIfGs_isTact(kSongOfPassing)); }, nullptr},
    {"deku_leaf", "Flapping in the Breeze", "Obtain the Deku Leaf.", [] { return has(dInvSlot_DEKU_LEAF_e); }, nullptr},
    {"boomerang", "Back at Ya...Fivefold!", "Obtain the Boomerang.", [] { return has(dInvSlot_BOOMERANG_e); }, nullptr},
    {"pearl_farore", "Farore's Dream", "Obtain Farore's Pearl.", [] { return bool(dComIfGs_isSymbol(dSymbol_FARORE_e)); }, nullptr},
    {"magic_upgrade", "Kazam", "Obtain the magic power upgrade.", [] { return dComIfGs_getMaxMagic() >= 32; }, nullptr},
    {"bombs", "Have a Blast", "Obtain bombs.", [] { return has(dInvSlot_BOMB_e); }, nullptr},
    {"bombs_60", "Kaboom", "Be able to carry 60 bombs.", [] { return dComIfGs_getBombMax() >= 60; }, nullptr},
    {"bombs_99", "Gunboat Diplomacy", "Be able to carry 99 bombs.", [] { return dComIfGs_getBombMax() >= 99; }, nullptr},
    {"pearl_nayru", "For the Love of Nayru", "Obtain Nayru's Pearl.", [] { return bool(dComIfGs_isSymbol(dSymbol_NAYRU_e)); }, nullptr},
    {"wallet_1000", "Silver Status", "Obtain the first wallet upgrade.", [] { return dComIfGs_getWalletSize() >= 1; }, nullptr},
    {"wallet_5000", "Deep Pockets", "Obtain the largest wallet.", [] { return dComIfGs_getWalletSize() >= 2; }, nullptr},
    {"deluxe_picto_box", "Let There Be Light", "Obtain the Deluxe Picto Box.", [] { return dComIfGs_getItem(dInvSlot_CAMERA_e) == dItemNo_DELUXE_PICTO_BOX_e; }, nullptr},
    {"command_melody", "Tench Hut", "Learn the Command Melody.", [] { return bool(dComIfGs_isTact(kCommandMelody)); }, nullptr},
    {"bow", "A Bowyer's Wrath", "Obtain the Hero's Bow.", [] { return has(dInvSlot_BOW_e); }, nullptr},
    {"master_sword", "The Successor", "Obtain the Master Sword.", [] { return sword(1); }, nullptr},
    {"ballad_of_gales", "The Voice of Boreas", "Learn the Ballad of Gales.", [] { return bool(dComIfGs_isTact(kBalladOfGales)); }, nullptr},
    {"tingle_tuner", "Second Hand Junk", "Obtain the Hand-Me-Down Tuner.", [] { return has(dInvSlot_TINGLE_TUNER_e); }, nullptr},
    {"skull_hammer", "Drop the Hammer", "Obtain the Skull Hammer.", [] { return has(dInvSlot_SKULL_HAMMER_e); }, nullptr},
    {"magic_arrows", "Diametrically Opposed", "Obtain the Fire and Ice Arrows.", [] { return arrows(false); }, nullptr},
    {"heros_charm", "The Face of a Champion", "Obtain the Hero's Charm.", [] { return bool(dComIfGs_isCollect(4, 0)); }, nullptr},
    {"magic_armor", "Impervious", "Obtain the Magic Armor.", [] { return has(dInvSlot_MAGIC_ARMOR_e); }, nullptr},
    {"power_bracelets", "Double Up to Catch Up", "Obtain the Power Bracelets.", [] { return bool(dComIfGs_isCollect(2, 0)); }, nullptr},
    {"iron_boots", "In Irons", "Obtain the Iron Boots.", [] { return has(dInvSlot_IRON_BOOTS_e); }, nullptr},
    {"arrows_60", "Adept Archery", "Be able to carry 60 arrows.", [] { return dComIfGs_getArrowMax() >= 60; }, nullptr},
    {"arrows_99", "Full Quiver", "Be able to carry 99 arrows.", [] { return dComIfGs_getArrowMax() >= 99; }, nullptr},
    {"earth_gods_lyric", "Laruto's Legacy", "Learn the Earth God's Lyric.", [] { return bool(dComIfGs_isTact(kEarthGodsLyric)); }, nullptr},
    {"mirror_shield", "Under the Gods' Aegis", "Obtain the Mirror Shield.", [] { return bool(dComIfGs_isCollect(1, 1)); }, nullptr},
    {"master_sword_half", "Re-energized", "Restore some of the Master Sword's former energy.", [] { return sword(2); }, nullptr},
    {"wind_gods_aria", "Fado's Fate", "Learn the Wind God's Aria.", [] { return bool(dComIfGs_isTact(kWindGodsAria)); }, nullptr},
    {"hookshot", "Reach Out and Touch Someone", "Obtain the Hookshot.", [] { return has(dInvSlot_HOOKSHOT_e); }, nullptr},
    {"master_sword_full", "Hylia's Blessing", "Completely restore the Master Sword's power.", [] { return sword(3); }, nullptr},
    {"bottles_4", "Boukalophile", "Collect all 4 bottles.", [] { return bottles() >= 4; },
        [] { return Progress{bottles(), 4}; }},
    {"hearts_20", "You Can't Teach Heart", "Have 20 Heart Containers.", [] { return hearts() >= 20; },
        [] { return Progress{std::min(hearts(), 20), 20}; }},
    {"triforce", "Some Assembly Required", "Gather all 8 Triforce shards and reassemble the Triforce of Courage.",
        [] { return dComIfGs_getTriforceNum() >= 8; }, [] { return Progress{dComIfGs_getTriforceNum(), 8}; }},
    {"light_arrows", "The Power to Kill", "Obtain the Light Arrow.", [] { return arrows(true); }, nullptr},
    // The name-entry scene after the credits starts the second quest from the cleared save.
    {"ganondorf", "As Dandelions", "Defeat Ganondorf and set sail for a new land.",
        [] { return dComIfGs_getClearCount() != 0; }, nullptr},
};
constexpr int kCount = int(sizeof(kAchievements) / sizeof(kAchievements[0]));

// index < 0: a summary of -index unlocks found at once (a save loaded with past progress).
struct Banner { int index; uint64_t shownAt; };
struct State {
    bool loaded = false;
    bool unlocked[kCount] = {};
    long long unlockedAt[kCount] = {};
    uint64_t lastCheck = 0;
    std::deque<Banner> banners;
} s;

std::string storePath() {
    std::string path = pc_settings_path() ? pc_settings_path() : "";
    const auto slash = path.find_last_of('/');
    path = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    return path + "achievements.txt";
}

int indexOf(const char* id) {
    for (int i = 0; i < kCount; ++i) if (std::strcmp(kAchievements[i].id, id) == 0) return i;
    return -1;
}

void load() {
    s.loaded = true;
    FILE* file = std::fopen(storePath().c_str(), "r");
    if (!file) return;
    char id[64];
    long long when = 0;
    while (std::fscanf(file, "%63s %lld", id, &when) == 2) {
        const int i = indexOf(id);
        if (i >= 0) { s.unlocked[i] = true; s.unlockedAt[i] = when; }
    }
    std::fclose(file);
}

void save() {
    const std::string path = storePath(), temporary = path + ".tmp";
    FILE* file = std::fopen(temporary.c_str(), "w");
    if (!file) return;
    // Unknown ids from a later build are dropped only if this build never saw them;
    // keep the format trivially readable for that reason.
    for (int i = 0; i < kCount; ++i)
        if (s.unlocked[i]) std::fprintf(file, "%s %lld\n", kAchievements[i].id, s.unlockedAt[i]);
    const bool ok = std::fflush(file) == 0;
    std::fclose(file);
    if (ok) std::rename(temporary.c_str(), path.c_str());
    else std::remove(temporary.c_str());
}

void notifyConsole(const char* line) {
#if defined(__PROSPERO__)
    // The shell's notification request (OrbisNotificationRequest), as payloads use it.
    struct Request {
        int type, reqId, priority, msgId, targetId, userId, unk1, unk2, appId, errorNum, unk3;
        char useIconImageUri;
        char message[1024];
        char iconUri[1024];
        char unk[1024];
    } request{};
    request.targetId = -1;
    request.useIconImageUri = 1;
    std::snprintf(request.message, sizeof(request.message), "%s\n%s",
        pc_menu_text("Achievement unlocked"), line);
    std::snprintf(request.iconUri, sizeof(request.iconUri), "%s", "cxml://psnotification/tex_icon_system");
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
#else
    (void)line;
#endif
}

void* findPlayer(void* proc, void*) {
    return fpcM_GetName(proc) == fpcNm_PLAY_SCENE_e && dComIfGp_getLinkPlayer() ? proc : nullptr;
}

void check(uint64_t now) {
    // Four checks a second are plenty, and only while the game proper runs:
    // the title demo and file select have no player save loaded.
    if (s.lastCheck && now - s.lastCheck < 250000000ull) return;
    s.lastCheck = now;
    // Link must exist too: the title screen's sea runs a PLAY scene without a loaded save.
    if (!fpcM_Search(findPlayer, nullptr) && !sceneRunning(fpcNm_NAMEEX_SCENE_e)) return;
    int found[kCount], count = 0;
    const long long when = static_cast<long long>(std::time(nullptr));
    for (int i = 0; i < kCount; ++i) {
        if (s.unlocked[i] || !kAchievements[i].check()) continue;
        s.unlocked[i] = true;
        s.unlockedAt[i] = when;
        found[count++] = i;
    }
    if (!count) return;
    save();
    // A save with earlier progress unlocks many at once: one summary, not a queue of banners.
    if (count > 3) {
        s.banners.push_back({-count, 0});
        char line[96];
        std::snprintf(line, sizeof(line), pc_menu_text("%d achievements from your saved progress"), count);
        notifyConsole(line);
        return;
    }
    for (int k = 0; k < count; ++k) {
        s.banners.push_back({found[k], 0});
        notifyConsole(pc_menu_text(kAchievements[found[k]].title));
    }
}

void drawBanner(uint64_t now) {
    if (s.banners.empty() || !ImGui::GetCurrentContext()) return;
    auto& banner = s.banners.front();
    if (!banner.shownAt) banner.shownAt = now;
    const double age = (now - banner.shownAt) / 1e9;
    constexpr double duration = 5.0, fade = 0.35;
    if (age >= duration) { s.banners.pop_front(); return; }
    const float alpha = float(std::min({1.0, age / fade, (duration - age) / fade}));
    const auto screen = ImGui::GetIO().DisplaySize;
    if (screen.x <= 0 || screen.y <= 0) return;
    const float scale = std::max(1.f, screen.y / 720.f);
    const float f = ImGui::GetFontSize() * scale, margin = 16.f * scale;
    ImGui::SetNextWindowPos(ImVec2(screen.x * .5f, margin), ImGuiCond_Always, ImVec2(.5f, 0.f));
    ImGui::SetNextWindowBgAlpha(.94f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(f * .9f, f * .6f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, f * .5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, pc::ui::surface);
    ImGui::PushStyleColor(ImGuiCol_Border, pc::ui::warm);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##cos_achievement_banner", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale);
        ImGui::TextColored(pc::ui::warm, "%s", pc_menu_text("Achievement unlocked"));
        if (banner.index >= 0) {
            const auto& a = kAchievements[banner.index];
            ImGui::TextColored(pc::ui::text, "%s", pc_menu_text(a.title));
            ImGui::SetWindowFontScale(scale * .85f);
            ImGui::TextColored(pc::ui::muted, "%s", pc_menu_text(a.description));
        } else {
            ImGui::TextColored(pc::ui::text, pc_menu_text("%d achievements from your saved progress"), -banner.index);
            ImGui::SetWindowFontScale(scale * .85f);
            ImGui::TextColored(pc::ui::muted, "%s", pc_menu_text("See them in Options > System > Achievements."));
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}
}

void pc_achievements_frame() {
    if (!s.loaded) load();
    const uint64_t now = pc::monotonicNs();
    check(now);
    drawBanner(now);
}

int pc_achievements_count() { return kCount; }

int pc_achievements_unlocked() {
    if (!s.loaded) load();
    return int(std::count(std::begin(s.unlocked), std::end(s.unlocked), true));
}

PcAchievementInfo pc_achievements_at(int index) {
    if (!s.loaded) load();
    if (index < 0 || index >= kCount) return {"", "", false, -1, 0};
    const auto& a = kAchievements[index];
    PcAchievementInfo info{a.title, a.description, s.unlocked[index], -1, 0};
    if (a.progress && !s.unlocked[index] && fpcM_Search(findPlayer, nullptr)) {
        const Progress p = a.progress();
        info.progress = std::clamp(p.value, 0, p.goal);
        info.goal = p.goal;
    }
    return info;
}
