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

// iOS shell: an SDL3 frontend that mirrors the Vita3K Android app layout —
// a game library, a Settings screen (Controls / Interface / Emulator tabs with
// toggle rows) and an "Install PKG" screen that scans the app Documents folder
// and installs .pkg files through packages::install_pkg. Text is rendered with
// stb_truetype (DroidSansFallbackFull.ttf) so CJK titles display correctly.
// Game sessions render with Vulkan, translated to Metal by MoltenVK.
//
// On-device note: Dynarmic is a JIT recompiler; iOS forbids JIT for App Store
// apps, so a JIT-enabled sideload is required on real devices.

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
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr int LOGICAL_WIDTH = 960;
constexpr int LOGICAL_HEIGHT = 544;
constexpr int APPBAR_H = 52;
constexpr int ROW_H = 72;
constexpr int ROW_GAP = 4;

// Android-style dark theme palette.
constexpr SDL_Color COLOR_BG{ 0x0e, 0x10, 0x13, 0xff };
constexpr SDL_Color COLOR_SURFACE{ 0x1a, 0x1e, 0x27, 0xff };
constexpr SDL_Color COLOR_SURFACE_HI{ 0x25, 0x2c, 0x3b, 0xff };
constexpr SDL_Color COLOR_TEXT{ 0xe8, 0xed, 0xf5, 0xff };
constexpr SDL_Color COLOR_TEXT_DIM{ 0x8b, 0x95, 0xa7, 0xff };
constexpr SDL_Color COLOR_ACCENT{ 0x4f, 0x8c, 0xff, 0xff };
constexpr SDL_Color COLOR_TOGGLE_ON{ 0x3a, 0x63, 0xa8, 0xff };
constexpr SDL_Color COLOR_TOGGLE_OFF{ 0x3a, 0x3f, 0x4c, 0xff };
constexpr SDL_Color COLOR_TAB_ON{ 0xff, 0x9f, 0x0a, 0xff }; // Android accent orange
constexpr SDL_Color COLOR_ERROR{ 0xff, 0x6b, 0x6b, 0xff };

// --- Unicode text rendering -----------------------------------------------

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

void draw_text(SDL_Renderer *renderer, const std::string &text, SDL_Color color, float x, float y, int pixel_h) {
    SDL_Texture *texture = make_text(renderer, text, color, pixel_h);
    if (!texture)
        return;
    float w = 0.f;
    float h = 0.f;
    SDL_GetTextureSize(texture, &w, &h);
    SDL_FRect dst{ x, y, w, h };
    SDL_RenderTexture(renderer, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
}

void fill_rect(SDL_Renderer *renderer, SDL_Color color, float x, float y, float w, float h) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_FRect rect{ x, y, w, h };
    SDL_RenderFillRect(renderer, &rect);
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

void load_shell_settings(ShellSettings &s) {
    std::ifstream in(shell_cfg_path());
    if (!in.is_open())
        return;
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string k = line.substr(0, eq);
        const bool v = line.substr(eq + 1) == "1";
        if (k == "show_onscreen")
            s.show_onscreen = v;
        else if (k == "hide_overlay_on_controller")
            s.hide_overlay_on_controller = v;
        else if (k == "show_l2r2")
            s.show_l2r2 = v;
        else if (k == "show_l3r3")
            s.show_l3r3 = v;
        else if (k == "show_touch_switch")
            s.show_touch_switch = v;
        else if (k == "show_hide_toggle")
            s.show_hide_toggle = v;
    }
}

void save_shell_settings(const ShellSettings &s) {
    std::ofstream out(shell_cfg_path());
    if (!out.is_open())
        return;
    out << "show_onscreen=" << (s.show_onscreen ? 1 : 0) << "\n"
        << "hide_overlay_on_controller=" << (s.hide_overlay_on_controller ? 1 : 0) << "\n"
        << "show_l2r2=" << (s.show_l2r2 ? 1 : 0) << "\n"
        << "show_l3r3=" << (s.show_l3r3 ? 1 : 0) << "\n"
        << "show_touch_switch=" << (s.show_touch_switch ? 1 : 0) << "\n"
        << "show_hide_toggle=" << (s.show_hide_toggle ? 1 : 0) << "\n";
}

// --- Screens / model -------------------------------------------------------

enum class Screen { Library, Settings, InstallPkg };
enum class SettingsTab { Controls, Interface, Emulator };

struct LauncherRow {
    std::string title_id;
    std::string label;
    SDL_FRect rect;
    SDL_Texture *icon = nullptr;
};

struct ToggleItem {
    std::string label;
    std::string status;
    bool *value;
};

struct StaticItem {
    std::string label;
    std::string status;
};

struct PkgEntry {
    std::string path;
    std::string name;
};

struct PkgInstallState {
    bool installing = false;
    float progress = 0.f;
    std::string message;
    std::string active_name;
};

// --- UI helpers ------------------------------------------------------------

void draw_toggle(SDL_Renderer *r, bool on, float x, float y) {
    constexpr float W = 52.f;
    constexpr float H = 28.f;
    fill_rect(r, on ? COLOR_TOGGLE_ON : COLOR_TOGGLE_OFF, x, y, W, H);
    const float knob = on ? x + W - H : x;
    fill_rect(r, COLOR_TEXT, knob + 2.f, y + 2.f, H - 4.f, H - 4.f);
}

void draw_row_card(SDL_Renderer *r, float x, float y, float w, float h) {
    fill_rect(r, COLOR_SURFACE_HI, x, y, w, h);
}

void draw_toggle_row(SDL_Renderer *r, float x, float y, float w, const ToggleItem &item) {
    draw_row_card(r, x, y, w, ROW_H);
    draw_text(r, item.label, COLOR_TEXT, x + 16.f, y + 10.f, 14);
    draw_text(r, item.status, COLOR_TEXT_DIM, x + 16.f, y + 40.f, 11);
    draw_toggle(r, *item.value, x + w - 70.f, y + (ROW_H - 28.f) / 2.f);
}

void draw_static_row(SDL_Renderer *r, float x, float y, float w, const StaticItem &item) {
    draw_row_card(r, x, y, w, ROW_H);
    draw_text(r, item.label, COLOR_TEXT, x + 16.f, y + 10.f, 14);
    draw_text(r, item.status, COLOR_TEXT_DIM, x + 16.f, y + 40.f, 11);
}

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

} // namespace

// SDL3 entry point. SDL3 provides the UIKit application lifecycle; this runs
// on the main thread once the app is up.
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

    if (!app::init(emuenv, cfg, root_paths)) {
        LOG_ERROR("Emulated environment initialization failed.");
        return 1;
    }

    if (emuenv.cfg.controller_binds.empty() || (emuenv.cfg.controller_binds.size() != 15)
        || emuenv.cfg.controller_axis_binds.empty() || (emuenv.cfg.controller_axis_binds.size() != 6))
        app::reset_controller_binding(emuenv);

    init_libraries(emuenv);

    if (!app::init_apps_list(emuenv)) {
        LOG_ERROR("Failed to initialize apps list.");
        return 1;
    }
    app::load_users(emuenv);

    app::AppSessionController session(emuenv);

    // --- Shell state -------------------------------------------------------
    ShellSettings shell;
    load_shell_settings(shell);

    Screen screen = Screen::Library;
    SettingsTab settings_tab = SettingsTab::Controls;
    float scroll = 0.f;

    std::vector<ToggleItem> controls_items;
    controls_items.push_back({ "Show On-Screen Controls", "Enabled", &shell.show_onscreen });
    controls_items.push_back({ "Hide Overlay When Controller Is Connected", "No controller connected", &shell.hide_overlay_on_controller });
    controls_items.push_back({ "Show L2 / R2 Buttons", "Disabled", &shell.show_l2r2 });
    controls_items.push_back({ "Show L3 / R3 Buttons", "Disabled", &shell.show_l3r3 });
    controls_items.push_back({ "Show Touchscreen Switch", "Enabled", &shell.show_touch_switch });
    controls_items.push_back({ "Show Hide Toggle", "Disabled", &shell.show_hide_toggle });

    std::vector<StaticItem> interface_items;
    interface_items.push_back({ "Language", "English" });
    interface_items.push_back({ "Theme", "Dark" });

    std::vector<StaticItem> emulator_items;
    emulator_items.push_back({ "CPU Mode", "JIT (Dynarmic)" });
    emulator_items.push_back({ "Allow JIT", "Enabled (sideload required)" });

    auto pkgs = scan_pkgs();
    PkgInstallState pkg_state;

    bool quit = false;

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

        const auto apps = app::get_apps(emuenv);
        std::vector<LauncherRow> rows;
        rows.reserve(apps.size());
        for (const auto &entry : apps) {
            if (entry.category != "gd")
                continue;
            LauncherRow row;
            row.title_id = entry.title_id;
            row.label = entry.title;
            if (row.label.empty())
                row.label = entry.title_id;
            row.rect = SDL_FRect{ 12.f, APPBAR_H + 4.f + static_cast<float>(rows.size()) * (ROW_H + ROW_GAP), 936.f, ROW_H };
            if (!entry.icon_path.empty())
                row.icon = load_icon(renderer, entry.icon_path);
            rows.push_back(std::move(row));
        }

        std::string boot_title;
        bool boot = false;

        bool touching = false;
        bool dragged = false;
        float touch_x = 0.f;
        float touch_y = 0.f;
        float last_finger_y = 0.f;

        auto content_height = [&]() -> int {
            if (screen == Screen::Library)
                return APPBAR_H + 4 + static_cast<int>(rows.size()) * (ROW_H + ROW_GAP) - ROW_GAP;
            if (screen == Screen::Settings) {
                const int count = settings_tab == SettingsTab::Controls ? static_cast<int>(controls_items.size())
                    : settings_tab == SettingsTab::Interface ? static_cast<int>(interface_items.size())
                    : static_cast<int>(emulator_items.size());
                return APPBAR_H + 4 + 44 + count * (ROW_H + ROW_GAP);
            }
            return APPBAR_H + 4 + 44 + static_cast<int>(pkgs.size()) * (ROW_H + ROW_GAP);
        };

        while (!boot && !quit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    quit = true;
                    break;
                case SDL_EVENT_FINGER_DOWN: {
                    touching = true;
                    dragged = false;
                    touch_x = event.tfinger.x * LOGICAL_WIDTH;
                    touch_y = event.tfinger.y * LOGICAL_HEIGHT;
                    last_finger_y = touch_y;
                    break;
                }
                case SDL_EVENT_FINGER_MOTION: {
                    if (!touching)
                        break;
                    const float fy = event.tfinger.y * LOGICAL_HEIGHT;
                    const float dy = fy - last_finger_y;
                    last_finger_y = fy;
                    if (!dragged && (dy > 6.f || dy < -6.f))
                        dragged = true;
                    if (dragged) {
                        const int max_scroll = std::max(0, content_height() - (LOGICAL_HEIGHT - APPBAR_H));
                        scroll -= dy;
                        scroll = std::clamp(scroll, 0.f, static_cast<float>(max_scroll));
                    }
                    break;
                }
                case SDL_EVENT_FINGER_UP: {
                    if (!touching)
                        break;
                    const bool was_drag = dragged;
                    touching = false;
                    if (was_drag)
                        break;

                    const float px = touch_x;
                    const float py = touch_y;

                    if (py < APPBAR_H) {
                        if (screen == Screen::Library) {
                            if (px > 660.f && px < 780.f) { // Settings
                                screen = Screen::Settings;
                                scroll = 0.f;
                                break;
                            }
                            if (px > 790.f && px < 946.f) { // Install
                                pkgs = scan_pkgs();
                                screen = Screen::InstallPkg;
                                scroll = 0.f;
                                break;
                            }
                        } else if (px < 80.f) { // back
                            screen = Screen::Library;
                            scroll = 0.f;
                            break;
                        }
                        break;
                    }

                    const float list_y = py + scroll;

                    if (screen == Screen::Library) {
                        for (const auto &row : rows) {
                            if (px >= row.rect.x && px <= row.rect.x + row.rect.w
                                && list_y >= row.rect.y && list_y <= row.rect.y + row.rect.h) {
                                boot_title = row.title_id;
                                boot = true;
                                break;
                            }
                        }
                    } else if (screen == Screen::Settings) {
                        if (list_y > APPBAR_H && list_y < APPBAR_H + 44.f) {
                            if (px > 8.f && px < 324.f)
                                settings_tab = SettingsTab::Controls;
                            else if (px > 324.f && px < 640.f)
                                settings_tab = SettingsTab::Interface;
                            else
                                settings_tab = SettingsTab::Emulator;
                            scroll = 0.f;
                            break;
                        }
                        const float tab_bottom = APPBAR_H + 44.f;
                        if (settings_tab == SettingsTab::Controls) {
                            for (int i = 0; i < static_cast<int>(controls_items.size()); ++i) {
                                const float ry = tab_bottom + 4.f + i * (ROW_H + ROW_GAP);
                                if (list_y >= ry && list_y <= ry + ROW_H) {
                                    *controls_items[i].value = !*controls_items[i].value;
                                    controls_items[i].status = *controls_items[i].value ? "Enabled" : "Disabled";
                                    save_shell_settings(shell);
                                    break;
                                }
                            }
                        }
                    } else if (screen == Screen::InstallPkg) {
                        const float tab_bottom = APPBAR_H + 44.f;
                        if (!pkg_state.installing) {
                            for (int i = 0; i < static_cast<int>(pkgs.size()); ++i) {
                                const float ry = tab_bottom + 4.f + i * (ROW_H + ROW_GAP);
                                if (list_y >= ry && list_y <= ry + ROW_H) {
                                    pkg_state.installing = true;
                                    pkg_state.progress = 0.f;
                                    pkg_state.active_name = pkgs[i].name;
                                    pkg_state.message.clear();
                                    std::string zrif;
                                    const bool ok = install_pkg(fs::path(pkgs[i].path), emuenv, zrif,
                                        [&](float prog) { pkg_state.progress = prog; });
                                    pkg_state.installing = false;
                                    pkg_state.message = ok ? "Installed successfully"
                                        : "Install failed (needs zRIF / corrupt pkg)";
                                    app::init_apps_list(emuenv);
                                    break;
                                }
                            }
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }

            // --- Draw frame -------------------------------------------------
            fill_rect(renderer, COLOR_BG, 0.f, 0.f, LOGICAL_WIDTH, LOGICAL_HEIGHT);

            // App bar.
            fill_rect(renderer, COLOR_SURFACE, 0.f, 0.f, LOGICAL_WIDTH, APPBAR_H);
            if (screen == Screen::Library) {
                draw_text(renderer, "Vita3K", COLOR_TEXT, 16.f, 13.f, 24);
                draw_text(renderer, "iOS port", COLOR_TEXT_DIM, 860.f, 20.f, 13);
                draw_text(renderer, "Settings", COLOR_TEXT_DIM, 678.f, 18.f, 14);
                draw_text(renderer, "Install", COLOR_TEXT_DIM, 806.f, 18.f, 14);
            } else {
                draw_text(renderer, "<", COLOR_TEXT, 18.f, 13.f, 24);
                draw_text(renderer, screen == Screen::Settings ? "Settings" : "Install PKG",
                    COLOR_TEXT, 84.f, 13.f, 20);
            }

            if (screen == Screen::Library) {
                for (const auto &row : rows) {
                    const float ry = row.rect.y - scroll;
                    if (ry + ROW_H < APPBAR_H || ry > LOGICAL_HEIGHT)
                        continue;
                    draw_row_card(renderer, row.rect.x, ry, row.rect.w, row.rect.h);
                    const float icon_x = row.rect.x + 14.f;
                    const float icon_y = ry + 8.f;
                    constexpr float ICON_SIZE = 56.f;
                    if (row.icon) {
                        SDL_FRect dst{ icon_x, icon_y, ICON_SIZE, ICON_SIZE };
                        SDL_RenderTexture(renderer, row.icon, nullptr, &dst);
                    } else {
                        fill_rect(renderer, COLOR_ACCENT, icon_x, icon_y, ICON_SIZE, ICON_SIZE);
                    }
                    draw_text(renderer, row.label, COLOR_TEXT, row.rect.x + 84.f, ry + 8.f, 15);
                    draw_text(renderer, row.title_id, COLOR_TEXT_DIM, row.rect.x + 84.f, ry + 34.f, 12);
                }
                if (rows.empty()) {
                    draw_text(renderer, "No games found", COLOR_TEXT, 320.f, 190.f, 22);
                    draw_text(renderer, "Tap Install in the top bar to add a .pkg, or copy a game folder into:",
                        COLOR_TEXT_DIM, 210.f, 228.f, 13);
                    draw_text(renderer, "Files app -> Vita3K -> Documents -> ux0/app/<TITLEID>/",
                        COLOR_TEXT_DIM, 240.f, 250.f, 12);
                }
                draw_text(renderer, "Tap a title to boot. Physical controller (MFi / PS / Xbox) for buttons.",
                    COLOR_TEXT_DIM, 24.f, LOGICAL_HEIGHT - 22.f, 11);
            } else if (screen == Screen::Settings) {
                const float tab_y = APPBAR_H + 4.f;
                const std::array<const char *, 3> tabs = { "Controls", "Interface", "Emulator" };
                const std::array<SDL_Color, 3> tab_colors = {
                    settings_tab == SettingsTab::Controls ? COLOR_TAB_ON : COLOR_TEXT_DIM,
                    settings_tab == SettingsTab::Interface ? COLOR_TAB_ON : COLOR_TEXT_DIM,
                    settings_tab == SettingsTab::Emulator ? COLOR_TAB_ON : COLOR_TEXT_DIM,
                };
                const std::array<float, 3> tab_x = { 8.f, 324.f, 640.f };
                for (int i = 0; i < 3; ++i)
                    draw_text(renderer, tabs[i], tab_colors[i], tab_x[i], tab_y + 8.f, 16);
                fill_rect(renderer, COLOR_SURFACE_HI, 0.f, APPBAR_H + 44.f, LOGICAL_WIDTH, 1.f);

                const float tab_bottom = APPBAR_H + 44.f;
                if (settings_tab == SettingsTab::Controls) {
                    for (int i = 0; i < static_cast<int>(controls_items.size()); ++i) {
                        const float ry = tab_bottom + 4.f - scroll + i * (ROW_H + ROW_GAP);
                        if (ry + ROW_H < APPBAR_H || ry > LOGICAL_HEIGHT)
                            continue;
                        draw_toggle_row(renderer, 12.f, ry, 936.f, controls_items[i]);
                    }
                } else if (settings_tab == SettingsTab::Interface) {
                    for (int i = 0; i < static_cast<int>(interface_items.size()); ++i) {
                        const float ry = tab_bottom + 4.f - scroll + i * (ROW_H + ROW_GAP);
                        if (ry + ROW_H < APPBAR_H || ry > LOGICAL_HEIGHT)
                            continue;
                        draw_static_row(renderer, 12.f, ry, 936.f, interface_items[i]);
                    }
                } else {
                    for (int i = 0; i < static_cast<int>(emulator_items.size()); ++i) {
                        const float ry = tab_bottom + 4.f - scroll + i * (ROW_H + ROW_GAP);
                        if (ry + ROW_H < APPBAR_H || ry > LOGICAL_HEIGHT)
                            continue;
                        draw_static_row(renderer, 12.f, ry, 936.f, emulator_items[i]);
                    }
                }
            } else if (screen == Screen::InstallPkg) {
                if (pkg_state.installing) {
                    draw_text(renderer, "Installing " + pkg_state.active_name, COLOR_TEXT, 40.f, 110.f, 18);
                    fill_rect(renderer, COLOR_SURFACE_HI, 40.f, 140.f, 880.f, 22.f);
                    fill_rect(renderer, COLOR_ACCENT, 40.f, 140.f, 880.f * std::clamp(pkg_state.progress, 0.f, 1.f), 22.f);
                    draw_text(renderer, std::to_string(static_cast<int>(pkg_state.progress * 100.f)) + "%",
                        COLOR_TEXT, 460.f, 142.f, 14);
                } else {
                    if (!pkg_state.message.empty()) {
                        const bool failed = pkg_state.message.rfind("Install failed", 0) == 0;
                        draw_text(renderer, pkg_state.message, failed ? COLOR_ERROR : COLOR_ACCENT, 40.f, 104.f, 15);
                    }
                    draw_text(renderer, "PKG files in the app Documents folder:", COLOR_TEXT_DIM, 40.f, 64.f, 12);
                    if (pkgs.empty()) {
                        draw_text(renderer, "No .pkg files found.", COLOR_TEXT, 40.f, 130.f, 16);
                        draw_text(renderer, "Put <name>.pkg into Files app -> Vita3K, then reopen this screen.",
                            COLOR_TEXT_DIM, 40.f, 158.f, 12);
                    } else {
                        const float tab_bottom = APPBAR_H + 44.f;
                        for (int i = 0; i < static_cast<int>(pkgs.size()); ++i) {
                            const float ry = tab_bottom + 4.f - scroll + i * (ROW_H + ROW_GAP);
                            if (ry + ROW_H < APPBAR_H || ry > LOGICAL_HEIGHT)
                                continue;
                            draw_row_card(renderer, 12.f, ry, 936.f, ROW_H);
                            draw_text(renderer, pkgs[i].name, COLOR_TEXT, 28.f, ry + 10.f, 14);
                            draw_text(renderer, "Tap to install", COLOR_TEXT_DIM, 28.f, ry + 40.f, 11);
                        }
                    }
                }
            }

            SDL_RenderPresent(renderer);
            SDL_Delay(16);
        }

        for (auto &row : rows)
            SDL_DestroyTexture(row.icon);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);

        if (quit)
            break;

        LOG_INFO("Booting title '{}'", boot_title);
        AppLaunchRequest request;
        request.app_path = boot_title;
        const int exit_code = sdl_frontend::run(emuenv, session, request);
        LOG_INFO("Game session '{}' ended with code {}", boot_title, exit_code);
        app::init_apps_list(emuenv);
    }

    return 0;
}
