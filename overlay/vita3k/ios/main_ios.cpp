// Vita3K emulator project — iOS port shell
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// iOS shell: an SDL3 frontend styled after the official Vita3K desktop UI —
// a dark-blue gradient library with a menu bar (File / Emulation / Debug /
// Configuration / Controls / Utilities / Help), a column header (Title ID /
// Version / Category / Last used / Name) and icon rows. A Settings screen
// keeps the Android-style Controls / Interface / Emulator tabs, and the
// Utilities screen installs .pkg files from the app Documents folder.
// Text is rendered with stb_truetype (DroidSansFallbackFull.ttf) so CJK
// titles display correctly.
//
// The UI comes up FIRST: window + renderer start before the emulator
// environment is initialised, and the heavy emuenv setup (app::init,
// init_libraries, ...) runs on a background thread. This keeps the launcher
// responsive even when upstream code takes a while on device.
//
// Game sessions render with Vulkan, translated to Metal by MoltenVK.
// On-device note: Dynarmic is a JIT recompiler; iOS forbids JIT for App
// Store apps, so a JIT-enabled sideload is required on real devices.

#include "font.h"

#include <app/functions.h>
#include <app/session_controller.h>
#include <config/functions.h>
#include <config/state.h>
#include <emuenv/state.h>
#include <io/state.h>
#include <modules/module_parent.h>
#include <packages/pkg.h>
#include <sdl-frontend/session.h>
#include <util/exit_code.h>
#include <util/fs.h>
#include <util/log.h>

#include <stb_image.h>
// stb_truetype symbols (STB_TRUETYPE_IMPLEMENTATION) are provided by
// vita3k/overlay/font.cpp which is linked into liboverlay.a; only the header
// is needed here.
#include <stb_truetype.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int LOGICAL_WIDTH = 960;
constexpr int LOGICAL_HEIGHT = 544;
constexpr int MENUBAR_H = 40;
constexpr int HEADER_H = 34;
constexpr int STATUSBAR_H = 30;
constexpr int ROW_H = 66;
constexpr int ROW_GAP = 4;
constexpr int ROW_ICON = 46;

// Official Vita3K desktop palette (dark blue).
constexpr SDL_Color COLOR_BG_TOP{ 0x28, 0x44, 0x60, 0xff };
constexpr SDL_Color COLOR_BG_BOTTOM{ 0x12, 0x20, 0x32, 0xff };
constexpr SDL_Color COLOR_MENUBAR{ 0x0f, 0x1b, 0x2b, 0xd8 };
constexpr SDL_Color COLOR_HEADER{ 0x17, 0x29, 0x3f, 0xe0 };
constexpr SDL_Color COLOR_ROW{ 0x1e, 0x35, 0x51, 0xaa };
constexpr SDL_Color COLOR_ROW_HI{ 0x29, 0x45, 0x66, 0xcc };
constexpr SDL_Color COLOR_TEXT{ 0xe8, 0xed, 0xf5, 0xff };
constexpr SDL_Color COLOR_TEXT_DIM{ 0x9c, 0xb4, 0xcc, 0xff };
constexpr SDL_Color COLOR_ACCENT{ 0x4f, 0x8c, 0xff, 0xff };
constexpr SDL_Color COLOR_STATUS{ 0x0f, 0x1b, 0x2b, 0xea };

// Android-style settings palette (Settings screen).
constexpr SDL_Color COLOR_SET_BG{ 0x0e, 0x10, 0x13, 0xff };
constexpr SDL_Color COLOR_SET_SURFACE{ 0x1a, 0x1e, 0x27, 0xff };
constexpr SDL_Color COLOR_SET_TEXT{ 0xe8, 0xed, 0xf5, 0xff };
constexpr SDL_Color COLOR_SET_DIM{ 0x8b, 0x95, 0xa7, 0xff };
constexpr SDL_Color COLOR_TOGGLE_ON{ 0x3a, 0x63, 0xa8, 0xff };
constexpr SDL_Color COLOR_TOGGLE_OFF{ 0x3a, 0x3f, 0x4c, 0xff };
constexpr SDL_Color COLOR_TAB_ON{ 0xff, 0x9f, 0x0a, 0xff };
constexpr SDL_Color COLOR_ERROR{ 0xff, 0x6b, 0x6b, 0xff };

// --- Unicode text rendering (stb_truetype, cached textures) ----------------

static stbtt_fontinfo g_font;
static unsigned char *g_font_data = nullptr;
static bool g_font_ready = false;

bool init_shell_font() {
    if (g_font_ready)
        return true;
    const char *base = SDL_GetBasePath();
    if (!base)
        return false;
    const std::string path = std::string(base) + "fonts/DroidSansFallbackFull.ttf";
    size_t size = 0;
    g_font_data = static_cast<unsigned char *>(SDL_LoadFile(path.c_str(), &size));
    if (!g_font_data || size == 0)
        return false;
    if (!stbtt_InitFont(&g_font, g_font_data, 0))
        return false;
    g_font_ready = true;
    return true;
}

int utf8_next(const char *s, int &adv) {
    const unsigned char c = static_cast<unsigned char>(s[0]);
    if (c < 0x80) {
        adv = 1;
        return c;
    }
    if ((c & 0xe0) == 0xc0) {
        adv = 2;
        return ((c & 0x1f) << 6) | (static_cast<unsigned char>(s[1]) & 0x3f);
    }
    if ((c & 0xf0) == 0xe0) {
        adv = 3;
        return ((c & 0x0f) << 12) | ((static_cast<unsigned char>(s[1]) & 0x3f) << 6) | (static_cast<unsigned char>(s[2]) & 0x3f);
    }
    if ((c & 0xf8) == 0xf0) {
        adv = 4;
        return ((c & 0x07) << 18) | ((static_cast<unsigned char>(s[1]) & 0x3f) << 12)
            | ((static_cast<unsigned char>(s[2]) & 0x3f) << 6) | (static_cast<unsigned char>(s[3]) & 0x3f);
    }
    adv = 1;
    return '?';
}

SDL_Texture *make_text(SDL_Renderer *renderer, const std::string &text, SDL_Color color, int pixel_h) {
    if (!g_font_ready)
        return nullptr;
    const float scale = stbtt_ScaleForPixelHeight(&g_font, static_cast<float>(pixel_h));
    int asc = 0;
    int desc = 0;
    int lgap = 0;
    stbtt_GetFontVMetrics(&g_font, &asc, &desc, &lgap);
    const float line_h = static_cast<float>(asc - desc) * scale;

    float width = 0.f;
    for (const char *p = text.c_str(); *p;) {
        int adv = 0;
        const int cp = utf8_next(p, adv);
        int idx = stbtt_FindGlyphIndex(&g_font, cp);
        if (!idx)
            idx = stbtt_FindGlyphIndex(&g_font, '?');
        int adv_w = 0;
        int lsb = 0;
        stbtt_GetGlyphHMetrics(&g_font, idx, &adv_w, &lsb);
        width += adv_w * scale;
        p += adv;
    }
    const int w = static_cast<int>(width) + 2;
    const int h = static_cast<int>(line_h) + 2;
    if (w <= 0 || h <= 0)
        return nullptr;

    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA8888);
    if (!surface)
        return nullptr;

    const uint32_t color32 = SDL_MapSurfaceRGBA(surface, color.r, color.g, color.b, color.a);
    if (SDL_LockSurface(surface)) {
        uint32_t *pixels = static_cast<uint32_t *>(surface->pixels);
        const int pitch_px = surface->pitch / static_cast<int>(sizeof(uint32_t));
        const int base_y = static_cast<int>(asc * scale);
        float cx = 0.f;
        for (const char *p = text.c_str(); *p;) {
            int adv = 0;
            const int cp = utf8_next(p, adv);
            int idx = stbtt_FindGlyphIndex(&g_font, cp);
            if (!idx)
                idx = stbtt_FindGlyphIndex(&g_font, '?');
            int adv_w = 0;
            int lsb = 0;
            stbtt_GetGlyphHMetrics(&g_font, idx, &adv_w, &lsb);
            int bw = 0;
            int bh = 0;
            int bxoff = 0;
            int byoff = 0;
            unsigned char *bm = stbtt_GetGlyphBitmap(&g_font, scale, scale, idx, &bw, &bh, &bxoff, &byoff);
            if (bm) {
                for (int row = 0; row < bh; ++row) {
                    for (int col = 0; col < bw; ++col) {
                        if (bm[row * bw + col] == 0)
                            continue;
                        const int px = static_cast<int>(cx) + bxoff + col + 1;
                        const int py = base_y + byoff + row + 1;
                        if (px >= 0 && px < w && py >= 0 && py < h)
                            pixels[py * pitch_px + px] = color32;
                    }
                }
                stbtt_FreeBitmap(bm, nullptr);
            }
            cx += adv_w * scale;
            p += adv;
        }
    }
    SDL_UnlockSurface(surface);

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

// Textures are tied to one renderer. Rasterising stb_truetype glyphs every
// frame is very slow on the iOS Metal renderer, so cache by (text,color,size)
// and purge whenever the renderer is (re)created.
std::map<std::string, SDL_Texture *> g_text_cache;

std::string text_key(const std::string &text, SDL_Color color, int pixel_h) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "|%02x%02x%02x|%d|", color.r, color.g, color.b, pixel_h);
    return text + buf;
}

SDL_Texture *get_text(SDL_Renderer *renderer, const std::string &text, SDL_Color color, int pixel_h) {
    const std::string key = text_key(text, color, pixel_h);
    const auto it = g_text_cache.find(key);
    if (it != g_text_cache.end())
        return it->second;
    SDL_Texture *texture = make_text(renderer, text, color, pixel_h);
    if (texture)
        g_text_cache.emplace(key, texture);
    return texture;
}

void clear_text_cache() {
    for (const auto &kv : g_text_cache)
        SDL_DestroyTexture(kv.second);
    g_text_cache.clear();
}

void draw_text(SDL_Renderer *renderer, const std::string &text, SDL_Color color, float x, float y, int pixel_h) {
    SDL_Texture *texture = get_text(renderer, text, color, pixel_h);
    if (!texture)
        return;
    float w = 0.f;
    float h = 0.f;
    SDL_GetTextureSize(texture, &w, &h);
    SDL_FRect dst{ x, y, w, h };
    SDL_RenderTexture(renderer, texture, nullptr, &dst);
}

void fill_rect(SDL_Renderer *renderer, SDL_Color color, float x, float y, float w, float h) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_FRect rect{ x, y, w, h };
    SDL_RenderFillRect(renderer, &rect);
}

SDL_Texture *build_gradient(SDL_Renderer *renderer, SDL_Color top, SDL_Color bottom, int w, int h) {
    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA8888);
    if (!surface)
        return nullptr;
    if (SDL_LockSurface(surface)) {
        uint32_t *pixels = static_cast<uint32_t *>(surface->pixels);
        const int pitch_px = surface->pitch / static_cast<int>(sizeof(uint32_t));
        for (int row = 0; row < h; ++row) {
            const float t = h > 1 ? static_cast<float>(row) / static_cast<float>(h - 1) : 0.f;
            const uint8_t r = static_cast<uint8_t>(top.r + (bottom.r - top.r) * t);
            const uint8_t g = static_cast<uint8_t>(top.g + (bottom.g - top.g) * t);
            const uint8_t b = static_cast<uint8_t>(top.b + (bottom.b - top.b) * t);
            const uint32_t c = static_cast<uint32_t>(0xff000000u) | (static_cast<uint32_t>(r) << 16)
                | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
            for (int col = 0; col < w; ++col)
                pixels[row * pitch_px + col] = c;
        }
    }
    SDL_UnlockSurface(surface);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

SDL_Texture *load_icon(SDL_Renderer *renderer, const std::string &path) {
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
    if (!pixels || w <= 0 || h <= 0)
        return nullptr;
    SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA8888, pixels, w * 4);
    stbi_image_free(pixels);
    if (!surface)
        return nullptr;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

// --- Shell settings (persisted to <home>/Documents/vita3k_shell.cfg) -------

struct ShellSettings {
    bool show_onscreen = true;
    bool hide_overlay_on_controller = true;
    bool show_l2r2 = false;
    bool show_l3r3 = false;
    bool show_touch_switch = true;
    bool show_hide_toggle = false;
};

std::string shell_cfg_path() {
    const char *home = getenv("HOME");
    return std::string(home ? home : ".") + "/Documents/vita3k_shell.cfg";
}

void load_shell_settings(ShellSettings &shell) {
    std::ifstream in(shell_cfg_path());
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const bool val = line.substr(eq + 1) == "1";
        if (key == "show_onscreen")
            shell.show_onscreen = val;
        else if (key == "hide_overlay_on_controller")
            shell.hide_overlay_on_controller = val;
        else if (key == "show_l2r2")
            shell.show_l2r2 = val;
        else if (key == "show_l3r3")
            shell.show_l3r3 = val;
        else if (key == "show_touch_switch")
            shell.show_touch_switch = val;
        else if (key == "show_hide_toggle")
            shell.show_hide_toggle = val;
    }
}

void save_shell_settings(const ShellSettings &shell) {
    std::ofstream out(shell_cfg_path());
    if (!out)
        return;
    out << "show_onscreen=" << (shell.show_onscreen ? 1 : 0) << "\n";
    out << "hide_overlay_on_controller=" << (shell.hide_overlay_on_controller ? 1 : 0) << "\n";
    out << "show_l2r2=" << (shell.show_l2r2 ? 1 : 0) << "\n";
    out << "show_l3r3=" << (shell.show_l3r3 ? 1 : 0) << "\n";
    out << "show_touch_switch=" << (shell.show_touch_switch ? 1 : 0) << "\n";
    out << "show_hide_toggle=" << (shell.show_hide_toggle ? 1 : 0) << "\n";
}

// --- UI data ---------------------------------------------------------------

struct ToggleItem {
    std::string label;
    std::string status;
    bool *value;
};

struct StaticItem {
    std::string label;
    std::string value;
};

struct PkgEntry {
    std::string path;
    std::string name;
};

std::vector<PkgEntry> scan_pkgs() {
    std::vector<PkgEntry> out;
    const char *home = getenv("HOME");
    const std::string dir = std::string(home ? home : ".") + "/Documents";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec))
        return out;
    for (const auto &ent : std::filesystem::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file(ec))
            continue;
        const auto ext = ent.path().extension().string();
        if (ext == ".pkg" || ext == ".PKG") {
            PkgEntry e;
            e.path = ent.path().string();
            e.name = ent.path().filename().string();
            out.push_back(std::move(e));
        }
    }
    std::sort(out.begin(), out.end(), [](const PkgEntry &a, const PkgEntry &b) { return a.name < b.name; });
    return out;
}

struct PkgInstallState {
    bool installing = false;
    float progress = 0.f;
    std::string message;
    std::string active_name;
};

struct LauncherRow {
    std::string title_id;
    std::string label;
    std::string version;
    std::string category;
    std::string last_used;
    SDL_Texture *icon = nullptr;
    SDL_FRect rect{};
};

enum class Screen {
    Library,
    Settings,
    Install,
};

enum class SettingsTab {
    Controls,
    Interface,
    Emulator,
};

// Menu bar entries (official desktop order). Configuration / Utilities map
// to real screens; the rest show a toast (they are desktop-only).
struct MenuEntry {
    const char *label;
    int x0;
    int x1;
    bool live;
};

} // namespace

// --- Launcher --------------------------------------------------------------

int run_launcher(EmuEnvState &emuenv, std::unique_ptr<app::AppSessionController> &session,
    const std::atomic<bool> &init_done, const std::atomic<bool> &init_ok) {
    constexpr int MENU_Y = 0;
    constexpr int MENU_H = MENUBAR_H;
    constexpr int HEADER_Y = MENUBAR_H;
    constexpr int LIST_TOP = HEADER_Y + HEADER_H;
    constexpr int STATUS_Y = LOGICAL_HEIGHT - STATUSBAR_H;

    std::vector<MenuEntry> menu{
        { "File", 10, 60, false },
        { "Emulation", 62, 150, false },
        { "Debug", 152, 218, false },
        { "Configuration", 220, 330, true },
        { "Controls", 332, 402, true },
        { "Utilities", 404, 486, true },
        { "Help", 488, 540, false },
    };

    std::vector<LauncherRow> rows;
    auto pkgs = scan_pkgs();
    PkgInstallState pkg_state;
    ShellSettings shell;
    load_shell_settings(shell);

    std::vector<ToggleItem> controls_items;
    controls_items.push_back({ "Show On-Screen Controls", shell.show_onscreen ? "Enabled" : "Disabled", &shell.show_onscreen });
    controls_items.push_back({ "Hide Overlay When Controller Is Connected", "No controller connected", &shell.hide_overlay_on_controller });
    controls_items.push_back({ "Show L2 / R2 Buttons", shell.show_l2r2 ? "Enabled" : "Disabled", &shell.show_l2r2 });
    controls_items.push_back({ "Show L3 / R3 Buttons", shell.show_l3r3 ? "Enabled" : "Disabled", &shell.show_l3r3 });
    controls_items.push_back({ "Show Touchscreen Switch", shell.show_touch_switch ? "Enabled" : "Disabled", &shell.show_touch_switch });
    controls_items.push_back({ "Show Hide Toggle", shell.show_hide_toggle ? "Enabled" : "Disabled", &shell.show_hide_toggle });

    std::vector<StaticItem> interface_items{
        { "Language", "English" },
        { "Theme", "Dark" },
    };
    std::vector<StaticItem> emulator_items{
        { "CPU Mode", "JIT (Dynarmic)" },
        { "Allow JIT", "Enabled (sideload required)" },
    };

    Screen screen = Screen::Library;
    SettingsTab settings_tab = SettingsTab::Controls;
    float scroll = 0.f;
    float last_finger_y = 0.f;
    std::string toast;
    uint64_t toast_until = 0;

    bool quit = false;
    bool boot = false;
    std::string boot_title;

    const auto build_rows = [&](SDL_Renderer *r) {
        rows.clear();
        clear_text_cache();
        const auto apps = app::get_apps(emuenv);
        rows.reserve(apps.size());
        for (const auto &entry : apps) {
            if (entry.category != "gd")
                continue;
            LauncherRow row;
            row.title_id = entry.title_id;
            row.label = entry.title.empty() ? entry.title_id : entry.title;
            row.version = entry.app_ver.empty() ? "1.00" : entry.app_ver;
            row.category = entry.category;
            row.last_used = "Never";
            row.icon = entry.icon_path.empty() ? nullptr : load_icon(r, entry.icon_path);
            rows.push_back(std::move(row));
        }
        scroll = 0.f;
    };

    while (!quit) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_SENSOR)) {
            LOG_ERROR("SDL_Init failed: {}", SDL_GetError());
            return 1;
        }
        SDL_Window *window = SDL_CreateWindow("Vita3K", LOGICAL_WIDTH, LOGICAL_HEIGHT, SDL_WINDOW_FULLSCREEN);
        if (!window) {
            LOG_ERROR("SDL_CreateWindow failed: {}", SDL_GetError());
            return 1;
        }
        SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            LOG_ERROR("SDL_CreateRenderer failed: {}", SDL_GetError());
            SDL_DestroyWindow(window);
            return 1;
        }
        SDL_SetRenderLogicalPresentation(renderer, LOGICAL_WIDTH, LOGICAL_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_Texture *bg = build_gradient(renderer, COLOR_BG_TOP, COLOR_BG_BOTTOM, LOGICAL_WIDTH, LOGICAL_HEIGHT);

        bool rows_built = false;

        const auto content_height = [&]() -> int {
            if (screen == Screen::Library)
                return LIST_TOP + static_cast<int>(rows.size()) * (ROW_H + ROW_GAP) - ROW_GAP;
            if (screen == Screen::Settings) {
                const int count = settings_tab == SettingsTab::Controls ? static_cast<int>(controls_items.size())
                    : settings_tab == SettingsTab::Interface ? static_cast<int>(interface_items.size())
                    : static_cast<int>(emulator_items.size());
                return LIST_TOP + 44 + count * (ROW_H + ROW_GAP);
            }
            return LIST_TOP + 44 + static_cast<int>(pkgs.size()) * (ROW_H + ROW_GAP);
        };

        while (!boot && !quit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    quit = true;
                    break;
                case SDL_EVENT_FINGER_DOWN: {
                    if (toast_until)
                        toast_until = 0;
                    const float px = event.tfinger.x * LOGICAL_WIDTH;
                    const float py = event.tfinger.y * LOGICAL_HEIGHT;
                    last_finger_y = py;

                    // Menu bar.
                    if (py < MENU_H) {
                        if (screen != Screen::Library) {
                            screen = Screen::Library;
                            scroll = 0.f;
                            break;
                        }
                        for (const auto &m : menu) {
                            if (px >= m.x0 && px < m.x1) {
                                if (m.live) {
                                    if (std::string(m.label) == "Configuration") {
                                        screen = Screen::Settings;
                                        settings_tab = SettingsTab::Controls;
                                        scroll = 0.f;
                                    } else if (std::string(m.label) == "Controls") {
                                        screen = Screen::Settings;
                                        settings_tab = SettingsTab::Controls;
                                        scroll = 0.f;
                                    } else if (std::string(m.label) == "Utilities") {
                                        pkgs = scan_pkgs();
                                        screen = Screen::Install;
                                        scroll = 0.f;
                                    }
                                } else {
                                    toast = std::string(m.label) + " is not available on iOS";
                                    toast_until = SDL_GetTicks() + 2200;
                                }
                                break;
                            }
                        }
                        break;
                    }

                    // Header (Refresh) — library only.
                    if (screen == Screen::Library && py >= HEADER_Y && py < LIST_TOP && px > 830.f) {
                        if (init_ok.load()) {
                            build_rows(renderer);
                        } else {
                            toast = "Waiting for emulator to initialise";
                            toast_until = SDL_GetTicks() + 2000;
                        }
                        break;
                    }

                    const float list_y = py + scroll;
                    if (screen == Screen::Library) {
                        if (py < STATUS_Y) {
                            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                                const float ry = LIST_TOP + 4.f + i * (ROW_H + ROW_GAP);
                                if (list_y >= ry && list_y <= ry + ROW_H) {
                                    boot_title = rows[i].title_id;
                                    boot = true;
                                    break;
                                }
                            }
                        }
                    } else if (screen == Screen::Settings) {
                        if (py < STATUS_Y && list_y > LIST_TOP && list_y < LIST_TOP + 44.f) {
                            if (px > 8.f && px < 324.f)
                                settings_tab = SettingsTab::Controls;
                            else if (px > 324.f && px < 640.f)
                                settings_tab = SettingsTab::Interface;
                            else
                                settings_tab = SettingsTab::Emulator;
                            scroll = 0.f;
                            break;
                        }
                        const float tab_bottom = LIST_TOP + 44.f;
                        if (settings_tab == SettingsTab::Controls) {
                            for (int i = 0; i < static_cast<int>(controls_items.size()); ++i) {
                                const float ry = tab_bottom + 4.f + i * (ROW_H + ROW_GAP);
                                if (py >= STATUS_Y)
                                    break;
                                if (list_y >= ry && list_y <= ry + ROW_H) {
                                    *controls_items[i].value = !*controls_items[i].value;
                                    controls_items[i].status = *controls_items[i].value ? "Enabled" : "Disabled";
                                    save_shell_settings(shell);
                                    break;
                                }
                            }
                        }
                    } else if (screen == Screen::Install) {
                        const float tab_bottom = LIST_TOP + 44.f;
                        if (!pkg_state.installing) {
                            for (int i = 0; i < static_cast<int>(pkgs.size()); ++i) {
                                const float ry = tab_bottom + 4.f + i * (ROW_H + ROW_GAP);
                                if (py >= STATUS_Y)
                                    break;
                                if (list_y >= ry && list_y <= ry + ROW_H) {
                                    pkg_state.installing = true;
                                    pkg_state.active_name = pkgs[i].name;
                                    pkg_state.message = "Installing " + pkgs[i].name + "...";
                                    pkg_state.progress = 0.f;
                                    std::string zRIF;
                                    const bool ok = install_pkg(pkgs[i].path, emuenv, zRIF, [&](float p) {
                                        pkg_state.progress = p;
                                    });
                                    pkg_state.installing = false;
                                    pkg_state.progress = ok ? 1.f : 0.f;
                                    pkg_state.message = ok ? "Installed successfully"
                                        : "Install failed (needs zRIF / corrupt pkg)";
                                    app::init_apps_list(emuenv);
                                    if (init_ok.load())
                                        build_rows(renderer);
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }
                case SDL_EVENT_FINGER_MOTION: {
                    const float fy = event.tfinger.y * LOGICAL_HEIGHT;
                    const float dy = fy - last_finger_y;
                    if (dy > 6.f || dy < -6.f) {
                        const int max_scroll = std::max(0, content_height() - (LOGICAL_HEIGHT - STATUSBAR_H - LIST_TOP));
                        scroll = std::clamp(scroll - dy, 0.f, static_cast<float>(max_scroll));
                    }
                    last_finger_y = fy;
                    break;
                }
                default:
                    break;
                }
            }

            // --- Draw frame -------------------------------------------------
            if (bg)
                SDL_RenderTexture(renderer, bg, nullptr, nullptr);
            else
                fill_rect(renderer, COLOR_BG_TOP, 0.f, 0.f, LOGICAL_WIDTH, LOGICAL_HEIGHT);

            const auto status_text = [&]() -> std::string {
                if (toast_until && SDL_GetTicks() < toast_until)
                    return toast;
                if (!init_done.load())
                    return "Starting Vita3K emulator...";
                if (!init_ok.load())
                    return "Emulator initialisation failed — see log";
                return std::to_string(rows.size()) + " titles | Vita3K iOS port";
            };

            if (screen == Screen::Settings) {
                // Settings: Android-style dark screen with tabs.
                fill_rect(renderer, COLOR_SET_BG, 0.f, 0.f, LOGICAL_WIDTH, LOGICAL_HEIGHT);
                fill_rect(renderer, COLOR_SET_SURFACE, 0.f, 0.f, LOGICAL_WIDTH, MENUBAR_H);
                draw_text(renderer, "<", COLOR_SET_TEXT, 16.f, 13.f, 22);
                draw_text(renderer, "Settings", COLOR_SET_TEXT, 84.f, 14.f, 19);

                const std::array<const char *, 3> tab_labels{ "Controls", "Interface", "Emulator" };
                const std::array<float, 3> tab_x{ 28.f, 324.f, 640.f };
                const int selected = settings_tab == SettingsTab::Controls ? 0
                    : settings_tab == SettingsTab::Interface ? 1 : 2;
                for (int i = 0; i < 3; ++i) {
                    const SDL_Color c = i == selected ? COLOR_TAB_ON : COLOR_SET_DIM;
                    draw_text(renderer, tab_labels[i], c, tab_x[i], LIST_TOP + 10.f, 16);
                }
                fill_rect(renderer, COLOR_TAB_ON, tab_x[selected], LIST_TOP + 38.f, 260.f, 3.f);

                const auto draw_rows = [&](const std::vector<std::pair<std::string, std::string>> &items,
                    bool with_toggles, const std::vector<bool> &toggles) {
                    const float tab_bottom = LIST_TOP + 44.f;
                    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                        const float ry = tab_bottom + 4.f + i * (ROW_H + ROW_GAP) - scroll;
                        if (ry > LOGICAL_HEIGHT || ry + ROW_H < LIST_TOP)
                            continue;
                        fill_rect(renderer, COLOR_SET_SURFACE, 12.f, ry, 936.f, ROW_H);
                        draw_text(renderer, items[i].first, COLOR_SET_TEXT, 24.f, ry + 12.f, 15);
                        draw_text(renderer, items[i].second, COLOR_SET_DIM, 24.f, ry + 42.f, 12);
                        if (with_toggles && i < static_cast<int>(toggles.size())) {
                            const float tx = 896.f;
                            const float ty = ry + (ROW_H - 26.f) / 2.f;
                            const bool on = toggles[i];
                            fill_rect(renderer, on ? COLOR_TOGGLE_ON : COLOR_TOGGLE_OFF, tx, ty, 52.f, 26.f);
                            const float knob = on ? tx + 26.f : tx;
                            fill_rect(renderer, COLOR_SET_TEXT, knob + 2.f, ty + 2.f, 22.f, 22.f);
                        }
                    }
                };

                if (settings_tab == SettingsTab::Controls) {
                    std::vector<std::pair<std::string, std::string>> items;
                    std::vector<bool> toggles;
                    for (const auto &it : controls_items) {
                        items.emplace_back(it.label, it.status);
                        toggles.push_back(*it.value);
                    }
                    draw_rows(items, true, toggles);
                } else if (settings_tab == SettingsTab::Interface) {
                    std::vector<std::pair<std::string, std::string>> items;
                    for (const auto &it : interface_items)
                        items.emplace_back(it.label, it.value);
                    draw_rows(items, false, {});
                } else {
                    std::vector<std::pair<std::string, std::string>> items;
                    for (const auto &it : emulator_items)
                        items.emplace_back(it.label, it.value);
                    draw_rows(items, false, {});
                }

                draw_text(renderer, "Physical controller (MFi / PS / Xbox) recommended.",
                    COLOR_SET_DIM, 24.f, STATUS_Y + 10.f, 12);
                SDL_RenderPresent(renderer);
                SDL_Delay(16);
                continue;
            }

            if (screen == Screen::Install) {
                fill_rect(renderer, COLOR_BG_TOP, 0.f, 0.f, LOGICAL_WIDTH, LOGICAL_HEIGHT);
                fill_rect(renderer, COLOR_MENUBAR, 0.f, 0.f, LOGICAL_WIDTH, MENUBAR_H);
                draw_text(renderer, "<", COLOR_TEXT, 16.f, 13.f, 22);
                draw_text(renderer, "Install PKG", COLOR_TEXT, 84.f, 14.f, 19);
                if (pkg_state.installing) {
                    draw_text(renderer, "Installing " + pkg_state.active_name, COLOR_TEXT, 40.f, 130.f, 17);
                    draw_text(renderer, std::to_string(static_cast<int>(pkg_state.progress * 100.f)) + "%",
                        COLOR_ACCENT, 40.f, 170.f, 15);
                    fill_rect(renderer, COLOR_HEADER, 40.f, 210.f, 880.f, 8.f);
                    fill_rect(renderer, COLOR_ACCENT, 40.f, 210.f, 880.f * pkg_state.progress, 8.f);
                } else {
                    draw_text(renderer, pkg_state.message.empty() ? "" : pkg_state.message,
                        pkg_state.message.find("failed") != std::string::npos ? COLOR_ERROR : COLOR_ACCENT,
                        40.f, 110.f, 15);
                    draw_text(renderer, "PKG files in the app Documents folder:",
                        COLOR_TEXT_DIM, 40.f, 64.f, 12);
                    if (pkgs.empty()) {
                        draw_text(renderer, "No .pkg files found.", COLOR_TEXT, 40.f, 150.f, 17);
                        draw_text(renderer, "Put <name>.pkg into Files app -> Vita3K, then reopen this screen.",
                            COLOR_TEXT_DIM, 40.f, 190.f, 13);
                    } else {
                        const float tab_bottom = LIST_TOP + 44.f;
                        for (int i = 0; i < static_cast<int>(pkgs.size()); ++i) {
                            const float ry = tab_bottom + 4.f + i * (ROW_H + ROW_GAP) - scroll;
                            if (ry > LOGICAL_HEIGHT || ry + ROW_H < LIST_TOP)
                                continue;
                            fill_rect(renderer, COLOR_ROW, 12.f, ry, 936.f, ROW_H);
                            draw_text(renderer, pkgs[i].name, COLOR_TEXT, 28.f, ry + 12.f, 15);
                            draw_text(renderer, "Tap to install", COLOR_TEXT_DIM, 28.f, ry + 42.f, 12);
                        }
                    }
                }
                draw_text(renderer, status_text(), COLOR_TEXT_DIM, 24.f, STATUS_Y + 10.f, 12);
                SDL_RenderPresent(renderer);
                SDL_Delay(16);
                continue;
            }

            // --- Library (official look) -----------------------------------
            // Menu bar.
            fill_rect(renderer, COLOR_MENUBAR, 0.f, 0.f, LOGICAL_WIDTH, MENUBAR_H);
            for (const auto &m : menu) {
                draw_text(renderer, m.label, m.live ? COLOR_TEXT : COLOR_TEXT_DIM, m.x0 + 4.f, 12.f, 14);
            }

            // Column header.
            fill_rect(renderer, COLOR_HEADER, 0.f, HEADER_Y, LOGICAL_WIDTH, HEADER_H);
            draw_text(renderer, "Title ID", COLOR_TEXT_DIM, 76.f, HEADER_Y + 10.f, 13);
            draw_text(renderer, "Version", COLOR_TEXT_DIM, 270.f, HEADER_Y + 10.f, 13);
            draw_text(renderer, "Category", COLOR_TEXT_DIM, 350.f, HEADER_Y + 10.f, 13);
            draw_text(renderer, "Last used", COLOR_TEXT_DIM, 430.f, HEADER_Y + 10.f, 13);
            draw_text(renderer, "Name >", COLOR_TEXT_DIM, 610.f, HEADER_Y + 10.f, 13);
            draw_text(renderer, "Refresh", COLOR_ACCENT, 878.f, HEADER_Y + 10.f, 13);

            if (!init_done.load()) {
                draw_text(renderer, "Vita3K", COLOR_TEXT, 380.f, 210.f, 40);
                draw_text(renderer, "PlayStation Vita emulator for iOS", COLOR_TEXT_DIM, 320.f, 268.f, 15);
                draw_text(renderer, "Initialising emulator core...", COLOR_ACCENT, 380.f, 300.f, 13);
            } else if (!init_ok.load()) {
                draw_text(renderer, "Vita3K", COLOR_TEXT, 380.f, 190.f, 40);
                draw_text(renderer, "Emulator initialisation failed", COLOR_ERROR, 330.f, 248.f, 16);
                draw_text(renderer, "Check the device log and reinstall if needed", COLOR_TEXT_DIM, 300.f, 278.f, 13);
            } else {
                if (!rows_built) {
                    build_rows(renderer);
                    rows_built = true;
                }
                if (rows.empty()) {
                    draw_text(renderer, "Vita3K", COLOR_TEXT, 400.f, 170.f, 44);
                    draw_text(renderer, "PlayStation Vita emulator for iOS", COLOR_TEXT_DIM, 330.f, 232.f, 15);
                    draw_text(renderer, "Tap Utilities to install a .pkg, or copy a game folder into:",
                        COLOR_TEXT_DIM, 250.f, 266.f, 13);
                    draw_text(renderer, "Files app -> Vita3K -> Documents -> ux0/app/<TITLEID>/",
                        COLOR_TEXT_DIM, 280.f, 290.f, 13);
                } else {
                    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                        const float ry = LIST_TOP + 4.f + i * (ROW_H + ROW_GAP) - scroll;
                        if (ry > LOGICAL_HEIGHT || ry + ROW_H < LIST_TOP)
                            continue;
                        fill_rect(renderer, COLOR_ROW, 12.f, ry, 936.f, ROW_H);
                        if (rows[i].icon) {
                            SDL_FRect idst{ 20.f, ry + (ROW_H - ROW_ICON) / 2.f, ROW_ICON, ROW_ICON };
                            SDL_RenderTexture(renderer, rows[i].icon, nullptr, &idst);
                        }
                        draw_text(renderer, rows[i].title_id, COLOR_TEXT, 80.f, ry + 14.f, 14);
                        draw_text(renderer, rows[i].version.empty() ? "1.00" : rows[i].version, COLOR_TEXT_DIM, 270.f, ry + 14.f, 13);
                        draw_text(renderer, rows[i].category, COLOR_TEXT_DIM, 350.f, ry + 14.f, 13);
                        draw_text(renderer, rows[i].last_used, COLOR_TEXT_DIM, 430.f, ry + 14.f, 13);
                        draw_text(renderer, rows[i].label, COLOR_TEXT, 610.f, ry + 14.f, 14);
                    }
                }
            }

            // Status bar.
            fill_rect(renderer, COLOR_STATUS, 0.f, STATUS_Y, LOGICAL_WIDTH, STATUSBAR_H);
            draw_text(renderer, status_text(), COLOR_TEXT_DIM, 24.f, STATUS_Y + 9.f, 12);

            SDL_RenderPresent(renderer);
            SDL_Delay(16);
        }

        // Tear the launcher window down before handing over to the session.
        clear_text_cache();
        SDL_DestroyTexture(bg);
        for (auto &row : rows) {
            if (row.icon)
                SDL_DestroyTexture(row.icon);
        }
        rows.clear();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);

        if (quit)
            break;

        LOG_INFO("Booting title '{}'", boot_title);
        if (!session)
            session = std::make_unique<app::AppSessionController>(emuenv);
        AppLaunchRequest request;
        request.app_path = boot_title;
        const int exit_code = sdl_frontend::run(emuenv, *session, request);
        LOG_INFO("Game session '{}' ended with code {}", boot_title, exit_code);
        app::init_apps_list(emuenv);
        boot = false;
    }
    return 0;
}

// --- Entry point -----------------------------------------------------------

int SDL_main(int argc, char *argv[]) {
    SDL_SetHint("SDL_HINT_ORIENTATIONS", "LandscapeLeft,LandscapeRight");

    Root root_paths;
    const bool portable = app::init_paths(root_paths);

    if (logging::init(root_paths, true) != Success)
        return InitConfigFailed;

    if (!init_shell_font())
        LOG_WARN("Launcher font (DroidSansFallbackFull.ttf) not loaded; text will be missing");

    Config cfg{};
    EmuEnvState emuenv;
    if (config::init_config(cfg, argc, argv, root_paths, portable) != Success) {
        LOG_ERROR("Failed to initialise config");
        return InitConfigFailed;
    }

    fs::create_directories(cfg.get_vita_fs_path());

    // Emulator environment setup can take a while (and upstream code keeps
    // growing); run it on a background thread so the launcher UI comes up
    // immediately instead of a black screen.
    std::atomic<bool> init_done{ false };
    std::atomic<bool> init_ok{ false };
    std::thread init_thread([&] {
        if (!app::init(emuenv, cfg, root_paths)) {
            LOG_ERROR("Emulated environment initialization failed.");
            init_done = true;
            return;
        }
        if (emuenv.cfg.controller_binds.empty() || (emuenv.cfg.controller_binds.size() != 15)
            || emuenv.cfg.controller_axis_binds.empty() || (emuenv.cfg.controller_axis_binds.size() != 6))
            app::reset_controller_binding(emuenv);

        init_libraries(emuenv);

        if (!app::init_apps_list(emuenv)) {
            LOG_ERROR("Failed to initialize apps list.");
            init_done = true;
            return;
        }
        app::load_users(emuenv);
        init_done = true;
        init_ok = true;
    });

    std::unique_ptr<app::AppSessionController> session;
    const int rc = run_launcher(emuenv, session, init_done, init_ok);
    init_thread.join();
    return rc;
}
