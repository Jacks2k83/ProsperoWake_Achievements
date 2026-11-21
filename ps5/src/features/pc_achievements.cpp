// PS5 achievements: everything that can be read from the loaded save (items, songs, upgrades,
// event flags, letters and charts).
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
#include "pc/pc_gameplay_options.h"
#include "pc_internal.h"
#include "d/d_com_inf_game.h"
#include "d/d_item_data.h"
#include "f_pc/f_pc_manager.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_audio.h"
#include "JAZelAudio/JAZelAudio_SE.h"
#include "JSystem/JKernel/JKRArchive.h"
#include "JSystem/JUtility/JUTTexture.h"
#include <imgui.h>
#include <aurora/imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

#if defined(__PROSPERO__)
extern "C" int sceKernelSendNotificationRequest(int device, void* request, size_t size, int blocking);
#endif

namespace {
struct Progress { int value, goal; };
struct Achievement {
    const char* id;
    u8 icon; // item whose inventory icon (itemicon.arc) stands for it
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

// Phase 2 helpers: event flags, letters and charts in the save.
bool event(u16 flag) { return dComIfGs_isEventBit(flag) != FALSE; }
// Letter registers (d_letter.cpp): 0 not sent, 1 sent, 2 in the mailbag, 3 read.
bool letterSent(u16 letter) { return dComIfGs_getEventReg(letter) >= 1; }
bool letterReceived(u16 letter) { return dComIfGs_getEventReg(letter) >= 2; }
// Collection-map numbers, as item_func_collectmapNN gives them: 1-8 Triforce charts,
// 35, 36, 52 and 53-61 the special charts, 9-34 and 37-51 the 41 Treasure Charts.
int treasureCharts() {
    int count = 0;
    for (int no = 9; no <= 51; ++no)
        if (no != 35 && no != 36) count += dComIfGs_isGetCollectMap(no) != FALSE;
    return count;
}
int specialCharts() {
    int count = 0;
    for (int no : {35, 36, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61}) count += dComIfGs_isGetCollectMap(no) != FALSE;
    return count;
}
// Islands drawn on the Sea Chart (fishmen): bit 0 of each square. The "reveal Sea Chart" option
// draws them all on new saves, so the mapping achievements wait while it is on.
int mappedIslands() {
    if (pc_gameplay_enabled(PC_GAMEPLAY_REVEAL_SEA_CHART)) return 0;
    int count = 0;
    auto& map = g_dComIfG_gameInfo.save.getPlayer().getMap();
    for (int i = 0; i < dIsleIdx_COUNT_e; ++i) count += map.isFmapBit(i, 0) != FALSE;
    return count;
}

// ---- item icons ------------------------------------------------------------------------------------
// The inventory icons come from the disc's /res/Msg/itemicon.arc, which the logo scene mounts for the
// pause menu. Pixel data and palettes stay big-endian GameCube texture formats: decoded here to RGBA
// once per item and handed to ImGui.
struct Rgba { u8 r, g, b, a; };
u16 be16(const u8* p) { return u16(p[0] << 8 | p[1]); }
u8 expand(u32 v, int bits) { return u8(bits == 3 ? v * 255 / 7 : bits == 4 ? v * 17 : bits == 5 ? (v << 3 | v >> 2) : (v << 2 | v >> 4)); }
Rgba rgb565(u16 v) { return {expand(v >> 11, 5), expand(v >> 5 & 63, 6), expand(v & 31, 5), 255}; }
Rgba rgb5a3(u16 v) {
    if (v & 0x8000) return {expand(v >> 10 & 31, 5), expand(v >> 5 & 31, 5), expand(v & 31, 5), 255};
    return {expand(v >> 8 & 15, 4), expand(v >> 4 & 15, 4), expand(v & 15, 4), expand(v >> 12 & 7, 3)};
}
Rgba paletteColor(const u8* palette, int format, int count, u32 index) {
    if (!palette || index >= u32(count)) return {0, 0, 0, 0};
    const u16 v = be16(palette + index * 2);
    if (format == 0) return {u8(v & 0xff), u8(v & 0xff), u8(v & 0xff), u8(v >> 8)}; // IA8
    return format == 1 ? rgb565(v) : rgb5a3(v);
}

// Decodes mip 0 of a ResTIMG; false for formats this does not know (the icon is then left out).
bool decodeTimg(const u8* base, size_t size, std::vector<Rgba>& out, int& width, int& height) {
    if (size < sizeof(ResTIMG)) return false;
    const auto* timg = reinterpret_cast<const ResTIMG*>(base);
    width = timg->width;
    height = timg->height;
    const int format = timg->format;
    if (width <= 0 || height <= 0 || width > 256 || height > 256) return false;
    const u32 imageOffset = u32(timg->imageOffset) ? u32(timg->imageOffset) : 0x20;
    const u8* data = base + imageOffset;
    const u8* palette = timg->numColors ? base + u32(timg->paletteOffset) : nullptr;
    const int colors = timg->numColors, paletteFormat = timg->colorFormat;
    // Texel block size (width, height) and bits per texel of each GX format.
    int bw = 8, bh = 8, bits = 4;
    switch (format) {
    case 0: bw = 8; bh = 8; bits = 4; break;   // I4
    case 1: bw = 8; bh = 4; bits = 8; break;   // I8
    case 2: bw = 8; bh = 4; bits = 8; break;   // IA4
    case 3: bw = 4; bh = 4; bits = 16; break;  // IA8
    case 4: case 5: bw = 4; bh = 4; bits = 16; break; // RGB565, RGB5A3
    case 6: bw = 4; bh = 4; bits = 32; break;  // RGBA8
    case 8: bw = 8; bh = 8; bits = 4; break;   // C4
    case 9: bw = 8; bh = 4; bits = 8; break;   // C8
    case 14: bw = 8; bh = 8; bits = 4; break;  // CMPR
    default: return false;
    }
    const int bx = (width + bw - 1) / bw, by = (height + bh - 1) / bh;
    const size_t bytes = size_t(bx) * by * bw * bh * bits / 8;
    if (imageOffset + bytes > size) return false;
    if (palette && (palette < base || palette + colors * 2 > base + size)) palette = nullptr;
    out.assign(size_t(width) * height, {0, 0, 0, 0});
    const auto put = [&](int x, int y, Rgba c) { if (x < width && y < height) out[size_t(y) * width + x] = c; };
    const u8* src = data;
    for (int tby = 0; tby < by; ++tby) for (int tbx = 0; tbx < bx; ++tbx) {
        const int x0 = tbx * bw, y0 = tby * bh;
        if (format == 14) {
            // Four 4x4 DXT1-style sub-blocks, colours big-endian.
            for (int sub = 0; sub < 4; ++sub, src += 8) {
                const u16 c0 = be16(src), c1 = be16(src + 2);
                Rgba table[4] = {rgb565(c0), rgb565(c1), {}, {}};
                if (c0 > c1) {
                    for (int k = 0; k < 2; ++k) {
                        const int w0 = k ? 1 : 2, w1 = k ? 2 : 1;
                        table[2 + k] = {u8((table[0].r * w0 + table[1].r * w1) / 3), u8((table[0].g * w0 + table[1].g * w1) / 3),
                                        u8((table[0].b * w0 + table[1].b * w1) / 3), 255};
                    }
                } else {
                    table[2] = {u8((table[0].r + table[1].r) / 2), u8((table[0].g + table[1].g) / 2), u8((table[0].b + table[1].b) / 2), 255};
                    table[3] = {0, 0, 0, 0};
                }
                const int sx = x0 + (sub & 1) * 4, sy = y0 + (sub >> 1) * 4;
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
                    put(sx + x, sy + y, table[src[4 + y] >> (6 - x * 2) & 3]);
            }
            continue;
        }
        if (format == 6) {
            // Two 32-byte halves: AR pairs, then GB pairs.
            for (int i = 0; i < 16; ++i)
                put(x0 + i % 4, y0 + i / 4, {src[i * 2 + 1], src[32 + i * 2], src[32 + i * 2 + 1], src[i * 2]});
            src += 64;
            continue;
        }
        for (int y = 0; y < bh; ++y) for (int x = 0; x < bw; ++x) {
            const int i = y * bw + x;
            Rgba c{};
            switch (format) {
            case 0: { const u8 v = expand(src[i / 2] >> (i & 1 ? 0 : 4) & 15, 4); c = {v, v, v, v}; break; }
            case 1: c = {src[i], src[i], src[i], src[i]}; break;
            case 2: { const u8 v = expand(src[i] & 15, 4); c = {v, v, v, expand(src[i] >> 4, 4)}; break; }
            case 3: c = {src[i * 2 + 1], src[i * 2 + 1], src[i * 2 + 1], src[i * 2]}; break;
            case 4: c = rgb565(be16(src + i * 2)); break;
            case 5: c = rgb5a3(be16(src + i * 2)); break;
            case 8: c = paletteColor(palette, paletteFormat, colors, src[i / 2] >> (i & 1 ? 0 : 4) & 15); break;
            case 9: c = paletteColor(palette, paletteFormat, colors, src[i]); break;
            }
            put(x0 + x, y0 + y, c);
        }
        src += bw * bh * bits / 8;
    }
    return true;
}

struct Icon { bool tried = false; ImTextureID texture = 0; };
Icon sIcons[256];

ImTextureID itemIcon(u8 item) {
    Icon& icon = sIcons[item];
    if (icon.tried) return icon.texture;
    const char* name = item == dItemNo_NONE_e ? nullptr : dItem_data::getTexture(item);
    JKRArchive* archive = dComIfGp_getItemIconArchive();
    if (!name || !archive) {
        icon.tried = archive != nullptr; // retry once the archive is mounted
        return 0;
    }
    icon.tried = true;
    static std::vector<u8> buffer(0x8000);
    const u32 size = JKRReadTypeResource(buffer.data(), u32(buffer.size()), 'TIMG', name, archive);
    std::vector<Rgba> rgba;
    int width = 0, height = 0;
    if (size && decodeTimg(buffer.data(), size, rgba, width, height))
        icon.texture = aurora_imgui_add_texture(u32(width), u32(height), rgba.data());
    return icon.texture;
}

// Stand-in for achievements without an inventory icon: a small Triforce.
void drawIcon(ImDrawList* draw, u8 item, ImVec2 p, float size, bool unlocked) {
    const ImTextureID texture = itemIcon(item);
    const ImU32 tint = unlocked ? IM_COL32_WHITE : IM_COL32(110, 120, 128, 150);
    if (texture) {
        draw->AddImage(texture, p, ImVec2(p.x + size, p.y + size), ImVec2(0, 0), ImVec2(1, 1), tint);
        return;
    }
    const ImU32 gold = unlocked ? IM_COL32(250, 205, 90, 255) : IM_COL32(110, 120, 128, 150);
    const float h = size * .86f, top = p.y + (size - h) * .5f;
    const ImVec2 a(p.x + size * .5f, top), b(p.x + size * .02f, top + h), c(p.x + size * .98f, top + h);
    const ImVec2 ab((a.x + b.x) * .5f, (a.y + b.y) * .5f), ac((a.x + c.x) * .5f, (a.y + c.y) * .5f), bc((b.x + c.x) * .5f, b.y);
    draw->AddTriangleFilled(a, ab, ac, gold);
    draw->AddTriangleFilled(ab, b, bc, gold);
    draw->AddTriangleFilled(ac, bc, c, gold);
}

const Achievement kAchievements[] = {
    {"bait_bag", dItemNo_BAIT_BAG_e, "Ready for Hunting", "Obtain the Bait Bag.", [] { return has(dInvSlot_BAIT_BAG_e); }, nullptr},
    {"met_korl", dItemNo_SAIL_e, "Carried by the Currents", "Meet the King of Red Lions.",
        [] { return event(dSv_event_flag_c::MET_KORL); }, nullptr},
    {"wind_waker", dItemNo_WIND_WAKER_e, "Maestro of the Mistral", "Obtain the Wind Waker.", [] { return has(dInvSlot_WIND_WAKER_e); }, nullptr},
    {"bottle", dItemNo_EMPTY_BOTTLE_e, "Bottom's Up", "Obtain a bottle.", [] { return bottles() >= 1; }, nullptr},
    {"grappling_hook", dItemNo_GRAPPLING_HOOK_e, "Rope a Dope", "Obtain the Grappling Hook.", [] { return has(dInvSlot_GRAPPLING_HOOK_e); }, nullptr},
    {"pearl_din", dItemNo_PEARL_DIN_e, "Jewel of Power", "Obtain Din's Pearl.", [] { return bool(dComIfGs_isSymbol(dSymbol_DIN_e)); }, nullptr},
    {"winds_requiem", dItemNo_WINDS_REQUIEM_e, "Blown Away", "Learn Wind's Requiem.", [] { return bool(dComIfGs_isTact(kWindsRequiem)); }, nullptr},
    {"letter_baitos_mom", dItemNo_NOTE_TO_MOM_e, "A Mother's Thanks", "Receive a letter from the mother of Baito, the part-time mail sorter on Dragon Roost Island.",
        [] { return letterReceived(dSv_event_flag_c::LETTER_BAITOS_MOM); }, nullptr},
    {"song_of_passing", dItemNo_SONG_OF_PASSING_e, "From Dusk 'til Dawn", "Learn the Song of Passing.", [] { return bool(dComIfGs_isTact(kSongOfPassing)); }, nullptr},
    {"deku_leaf", dItemNo_DEKU_LEAF_e, "Flapping in the Breeze", "Obtain the Deku Leaf.", [] { return has(dInvSlot_DEKU_LEAF_e); }, nullptr},
    {"boomerang", dItemNo_BOOMERANG_e, "Back at Ya...Fivefold!", "Obtain the Boomerang.", [] { return has(dInvSlot_BOOMERANG_e); }, nullptr},
    {"pearl_farore", dItemNo_PEARL_FARORE_e, "Farore's Dream", "Obtain Farore's Pearl.", [] { return bool(dComIfGs_isSymbol(dSymbol_FARORE_e)); }, nullptr},
    {"magic_upgrade", dItemNo_NONE_e, "Kazam", "Obtain the magic power upgrade.", [] { return dComIfGs_getMaxMagic() >= 32; }, nullptr},
    {"bombs", dItemNo_BOMB_BAG_e, "Have a Blast", "Obtain bombs.", [] { return has(dInvSlot_BOMB_e); }, nullptr},
    {"bombs_60", dItemNo_MAX_BOMB_UP1_e, "Kaboom", "Be able to carry 60 bombs.", [] { return dComIfGs_getBombMax() >= 60; }, nullptr},
    {"bombs_99", dItemNo_MAX_BOMB_UP2_e, "Gunboat Diplomacy", "Be able to carry 99 bombs.", [] { return dComIfGs_getBombMax() >= 99; }, nullptr},
    {"grandma_soup", dItemNo_SOUP_BOTTLE_e, "Soup is a Hug in a Bottle", "Heal Grandma and receive a brothy boon.",
        [] { return event(dSv_event_flag_c::GRANDMA_HEALED); }, nullptr},
    // Orca's Piece of Heart: d_a_npc_ji1.cpp's createItem sets 0x0F10 with it.
    {"orca_heart", dItemNo_HEART_PIECE_e, "En Garde", "Win a Piece of Heart from Orca for your sword profiency.",
        [] { return event(dSv_event_flag_c::UNK_0F10); }, nullptr},
    // setClearRecord sets 0x0F20 once a training session reaches 1,000 hits.
    {"orca_1000", dItemNo_SWORD_e, "First Intention", "Score 1,000 hits on Orca during sword training.",
        [] { return event(dSv_event_flag_c::UNK_0F20); }, nullptr},
    {"pearl_nayru", dItemNo_PEARL_NAYRU_e, "For the Love of Nayru", "Obtain Nayru's Pearl.", [] { return bool(dComIfGs_isSymbol(dSymbol_NAYRU_e)); }, nullptr},
    {"wallet_1000", dItemNo_MAX_RUPEE_UP1_e, "Silver Status", "Obtain the first wallet upgrade.", [] { return dComIfGs_getWalletSize() >= 1; }, nullptr},
    {"wallet_5000", dItemNo_MAX_RUPEE_UP2_e, "Deep Pockets", "Obtain the largest wallet.", [] { return dComIfGs_getWalletSize() >= 2; }, nullptr},
    {"deluxe_picto_box", dItemNo_DELUXE_PICTO_BOX_e, "Let There Be Light", "Obtain the Deluxe Picto Box.", [] { return dComIfGs_getItem(dInvSlot_CAMERA_e) == dItemNo_DELUXE_PICTO_BOX_e; }, nullptr},
    {"tingle_balloon_discount", dItemNo_TINGLE_TUNER_e, "Head of the Class", "Receive a permanent discount on the Tingle Balloon and Tingle Shield.",
        [] { return event(dSv_event_flag_c::UNLOCK_TINGLE_BALLOON_DISCOUNT); }, nullptr},
    {"tower_of_the_gods", dItemNo_NONE_e, "From the Sea to the Heavens", "Reveal the Tower of the Gods.",
        [] { return event(dSv_event_flag_c::PLACED_DINS_PEARL) && event(dSv_event_flag_c::PLACED_FARORES_PEARL) &&
                    event(dSv_event_flag_c::PLACED_NAYRUS_PEARL); }, nullptr},
    {"command_melody", dItemNo_COMMAND_MELODY_e, "Tench Hut", "Learn the Command Melody.", [] { return bool(dComIfGs_isTact(kCommandMelody)); }, nullptr},
    {"bow", dItemNo_BOW_e, "A Bowyer's Wrath", "Obtain the Hero's Bow.", [] { return has(dInvSlot_BOW_e); }, nullptr},
    {"master_sword", dItemNo_MASTER_SWORD_1_e, "The Successor", "Obtain the Master Sword.", [] { return sword(1); }, nullptr},
    {"ballad_of_gales", dItemNo_BALLAD_OF_GALES_e, "The Voice of Boreas", "Learn the Ballad of Gales.", [] { return bool(dComIfGs_isTact(kBalladOfGales)); }, nullptr},
    {"tingle_tuner", dItemNo_TINGLE_TUNER_e, "Second Hand Junk", "Obtain the Hand-Me-Down Tuner.", [] { return has(dInvSlot_TINGLE_TUNER_e); }, nullptr},
    {"skull_hammer", dItemNo_SKULL_HAMMER_e, "Drop the Hammer", "Obtain the Skull Hammer.", [] { return has(dInvSlot_SKULL_HAMMER_e); }, nullptr},
    {"ting_discount", dItemNo_TINGLE_TUNER_e, "Left-Over Magic Broth", "Receive a permanent discount on Tingle's Tings.",
        [] { return event(dSv_event_flag_c::UNLOCK_TING_DISCOUNT); }, nullptr},
    {"magic_arrows", dItemNo_MAGIC_ARROW_e, "Diametrically Opposed", "Obtain the Fire and Ice Arrows.", [] { return arrows(false); }, nullptr},
    {"heros_charm", dItemNo_HEROS_CHARM_e, "The Face of a Champion", "Obtain the Hero's Charm.", [] { return bool(dComIfGs_isCollect(4, 0)); }, nullptr},
    {"magic_armor", dItemNo_MAGIC_ARMOR_e, "Impervious", "Obtain the Magic Armor.", [] { return has(dInvSlot_MAGIC_ARMOR_e); }, nullptr},
    {"golden_feathers", dItemNo_GOLDEN_FEATHER_e, "Birds of a Feather", "Receive a reward in the mail for helping a Rito collect Golden Feathers.",
        [] { return letterReceived(dSv_event_flag_c::LETTER_HOSKITS_GIRLFRIEND); }, nullptr},
    {"letter_aryll", dItemNo_MAGGIES_LETTER_e, "Warmest Regards", "Receive a letter from Aryll.",
        [] { return letterReceived(dSv_event_flag_c::LETTER_ARYLL); }, nullptr},
    {"power_bracelets", dItemNo_POWER_BRACELETS_e, "Double Up to Catch Up", "Obtain the Power Bracelets.", [] { return bool(dComIfGs_isCollect(2, 0)); }, nullptr},
    {"iron_boots", dItemNo_IRON_BOOTS_e, "In Irons", "Obtain the Iron Boots.", [] { return has(dInvSlot_IRON_BOOTS_e); }, nullptr},
    {"arrows_60", dItemNo_MAX_ARROW_UP1_e, "Adept Archery", "Be able to carry 60 arrows.", [] { return dComIfGs_getArrowMax() >= 60; }, nullptr},
    {"arrows_99", dItemNo_MAX_ARROW_UP2_e, "Full Quiver", "Be able to carry 99 arrows.", [] { return dComIfGs_getArrowMax() >= 99; }, nullptr},
    {"earth_gods_lyric", dItemNo_EARTH_GODS_LYRIC_e, "Laruto's Legacy", "Learn the Earth God's Lyric.", [] { return bool(dComIfGs_isTact(kEarthGodsLyric)); }, nullptr},
    {"mirror_shield", dItemNo_MIRROR_SHIELD_e, "Under the Gods' Aegis", "Obtain the Mirror Shield.", [] { return bool(dComIfGs_isCollect(1, 1)); }, nullptr},
    {"master_sword_half", dItemNo_MASTER_SWORD_2_e, "Re-energized", "Restore some of the Master Sword's former energy.", [] { return sword(2); }, nullptr},
    {"wind_gods_aria", dItemNo_WIND_GODS_ARIA_e, "Fado's Fate", "Learn the Wind God's Aria.", [] { return bool(dComIfGs_isTact(kWindGodsAria)); }, nullptr},
    {"hookshot", dItemNo_HOOKSHOT_e, "Reach Out and Touch Someone", "Obtain the Hookshot.", [] { return has(dInvSlot_HOOKSHOT_e); }, nullptr},
    {"master_sword_full", dItemNo_MASTER_SWORD_3_e, "Hylia's Blessing", "Completely restore the Master Sword's power.", [] { return sword(3); }, nullptr},
    {"bottles_4", dItemNo_FAIRY_BOTTLE_e, "Boukalophile", "Collect all 4 bottles.", [] { return bottles() >= 4; },
        [] { return Progress{bottles(), 4}; }},
    {"hearts_20", dItemNo_HEART_CONTAINER_e, "You Can't Teach Heart", "Have 20 Heart Containers.", [] { return hearts() >= 20; },
        [] { return Progress{std::min(hearts(), 20), 20}; }},
    {"triforce", dItemNo_TRIFORCE1_e, "Some Assembly Required", "Gather all 8 Triforce shards and reassemble the Triforce of Courage.",
        [] { return dComIfGs_getTriforceNum() >= 8; }, [] { return Progress{dComIfGs_getTriforceNum(), 8}; }},
    {"light_arrows", dItemNo_LIGHT_ARROW_e, "The Power to Kill", "Obtain the Light Arrow.", [] { return arrows(true); }, nullptr},
    // The name-entry scene after the credits starts the second quest from the cleared save.
    {"ganondorf", dItemNo_SAIL_e, "As Dandelions", "Defeat Ganondorf and set sail for a new land.",
        [] { return dComIfGs_getClearCount() != 0; }, nullptr},
    {"special_charts", dItemNo_COLLECT_MAP_36_e, "Cartophile", "Obtain all special charts.",
        [] { return specialCharts() >= 12; }, [] { return Progress{specialCharts(), 12}; }},
    {"sea_chart_half", dItemNo_COLLECT_MAP_53_e, "Rookie Cartographer", "Map half of the islands on your Sea Chart.",
        [] { return mappedIslands() >= 25; }, [] { return Progress{mappedIslands(), 25}; }},
    {"sea_chart_full", dItemNo_COLLECT_MAP_53_e, "Master Cartographer", "Map the entire Great Sea.",
        [] { return mappedIslands() >= dIsleIdx_COUNT_e; }, [] { return Progress{mappedIslands(), dIsleIdx_COUNT_e}; }},
    {"treasure_charts_20", dItemNo_COLLECT_MAP_09_e, "Treasure Hunter", "Collect 20 Treasure Charts.",
        [] { return treasureCharts() >= 20; }, [] { return Progress{treasureCharts(), 20}; }},
    {"treasure_charts_all", dItemNo_COLLECT_MAP_09_e, "Legendary Atlas", "Collect all Treasure Charts.",
        [] { return treasureCharts() >= 41; }, [] { return Progress{treasureCharts(), 41}; }},
    {"beedle_silver", dItemNo_COMPLIMENTARY_ID_e, "VIP Customer", "Earn a Silver Membership from Beedle's Shop Ship.",
        [] { return letterSent(dSv_event_flag_c::LETTER_SILVER_MEMBERSHIP); }, nullptr},
    {"beedle_gold", dItemNo_FILL_UP_COUPON_e, "Indulgent Shopper", "Earn a Gold Membership from Beedle's Shop Ship.",
        [] { return letterSent(dSv_event_flag_c::LETTER_GOLD_MEMBERSHIP); }, nullptr},
};
constexpr int kCount = int(sizeof(kAchievements) / sizeof(kAchievements[0]));

// index < 0: a summary of -index unlocks found at once (a save loaded with past progress).
struct Banner { int index; uint64_t shownAt; };
constexpr int kTestBanner = 1 << 20;
struct State {
    bool loaded = false;
    bool unlocked[kCount] = {};
    long long unlockedAt[kCount] = {};
    uint64_t lastCheck = 0;
    std::deque<Banner> banners;
    std::string testIcon; // the notification icon the last test tried, shown on its banner
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

// Icons to try for the console notification: user/notification-icon.txt, one URI or path per line.
// Unlocks use the first; each press of the test button tries the next. Read on every use, so the
// file can be edited over FTP and tried again without restarting.
std::vector<std::string> notificationIcons() {
    std::vector<std::string> icons;
    std::string path = pc_settings_path() ? pc_settings_path() : "";
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos) path = path.substr(0, slash + 1) + "notification-icon.txt";
    if (FILE* file = std::fopen(path.c_str(), "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), file)) {
            std::string value(line);
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) value.pop_back();
            if (!value.empty() && value[0] != '#') icons.push_back(value);
        }
        std::fclose(file);
    }
    if (icons.empty()) icons.push_back("cxml://psnotification/tex_icon_system");
    return icons;
}

void notifyConsole(const char* line, const std::string& iconUri) {
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
    std::snprintf(request.iconUri, sizeof(request.iconUri), "%s", iconUri.c_str());
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
#else
    (void)line;
    (void)iconUri;
#endif
}
void notifyConsole(const char* line) { notifyConsole(line, notificationIcons().front()); }

// The game's small "item get" jingle (as for rupees from a chest), once per announcement.
void jingle() { mDoAud_seStart(JA_SE_ME_ITEM_GET_S); }

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
        jingle();
        char line[96];
        std::snprintf(line, sizeof(line), pc_menu_text("%d achievements from your saved progress"), count);
        notifyConsole(line);
        return;
    }
    jingle();
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
        const float iconSize = ImGui::GetTextLineHeight() * 2.6f;
        const u8 item = banner.index >= 0 && banner.index < kCount ? kAchievements[banner.index].icon : u8(dItemNo_TRIFORCE1_e);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        drawIcon(ImGui::GetWindowDrawList(), item, at, iconSize, true);
        ImGui::Dummy(ImVec2(iconSize, iconSize));
        ImGui::SameLine(0.f, ImGui::GetTextLineHeight() * .6f);
        ImGui::BeginGroup();
        ImGui::TextColored(pc::ui::warm, "%s", pc_menu_text("Achievement unlocked"));
        if (banner.index == kTestBanner) {
            ImGui::TextColored(pc::ui::text, "%s", pc_menu_text("Test notification"));
            ImGui::SetWindowFontScale(scale * .85f);
            ImGui::TextColored(pc::ui::muted, "%s", pc_menu_text("Nothing was unlocked."));
            ImGui::TextColored(pc::ui::muted, "%s", s.testIcon.c_str());
        } else if (banner.index >= 0) {
            const auto& a = kAchievements[banner.index];
            ImGui::TextColored(pc::ui::text, "%s", pc_menu_text(a.title));
            ImGui::SetWindowFontScale(scale * .85f);
            ImGui::TextColored(pc::ui::muted, "%s", pc_menu_text(a.description));
        } else {
            ImGui::TextColored(pc::ui::text, pc_menu_text("%d achievements from your saved progress"), -banner.index);
            ImGui::SetWindowFontScale(scale * .85f);
            ImGui::TextColored(pc::ui::muted, "%s", pc_menu_text("See them in Options > System > Achievements."));
        }
        ImGui::EndGroup();
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

bool pc_achievements_row(int index, bool selected, const ImVec4& dim, const ImVec4& accent) {
    if (index < 0 || index >= kCount) return false;
    if (!s.loaded) load();
    const auto& a = kAchievements[index];
    const PcAchievementInfo info = pc_achievements_at(index);
    const float f = ImGui::GetFontSize(), padding = f * .3f, iconSize = f * 1.9f;
    const float width = ImGui::GetContentRegionAvail().x, height = iconSize + padding * 2;
    const bool clicked = ImGui::InvisibleButton("##achievement", ImVec2(width, height));
    const ImVec2 p = ImGui::GetItemRectMin(), end(p.x + width, p.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, end, ImGui::GetColorU32(selected ? pc::ui::selected : pc::ui::surface), f * .18f);
    if (selected) {
        draw->AddRect(p, end, ImGui::GetColorU32(accent), f * .18f);
        draw->AddRectFilled(ImVec2(p.x, p.y + padding), ImVec2(p.x + f * .12f, end.y - padding), ImGui::GetColorU32(accent), f * .06f);
    }
    drawIcon(draw, a.icon, ImVec2(p.x + f * .45f, p.y + padding), iconSize, info.unlocked);
    char value[48];
    if (info.unlocked) std::snprintf(value, sizeof(value), "%s", pc_menu_text("Unlocked"));
    else if (info.progress >= 0) std::snprintf(value, sizeof(value), "%d / %d", info.progress, info.goal);
    else std::snprintf(value, sizeof(value), "%s", pc_menu_text("Locked"));
    const float valueWidth = ImGui::CalcTextSize(value).x;
    const float textX = p.x + f * .45f + iconSize + f * .55f, textRight = end.x - valueWidth - f * 1.2f;
    const ImU32 titleInk = ImGui::GetColorU32(info.unlocked ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : dim);
    draw->PushClipRect(ImVec2(textX, p.y), ImVec2(textRight, end.y), true);
    draw->AddText(ImVec2(textX, p.y + padding), titleInk, pc_menu_text(a.title));
    draw->AddText(nullptr, f * .8f, ImVec2(textX, p.y + padding + f * 1.05f), ImGui::GetColorU32(dim), pc_menu_text(a.description));
    draw->PopClipRect();
    draw->AddText(ImVec2(end.x - valueWidth - f * .6f, p.y + (height - f) * .5f),
        ImGui::GetColorU32(info.unlocked ? pc::ui::warm : dim), value);
    return clicked;
}

void pc_achievements_test_notice() {
    static size_t next = 0;
    const auto icons = notificationIcons();
    s.testIcon = icons[next++ % icons.size()];
    s.banners.push_back({kTestBanner, 0});
    jingle();
    notifyConsole(pc_menu_text("Test notification"), s.testIcon);
}
