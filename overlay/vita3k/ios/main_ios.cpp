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

// The iOS shell is an SDL3 launcher styled after the Vita3K Android app:
// a dark game library (icon + title + title id per row) with a top app bar.
// Tapping a row boots the title through sdl_frontend::run; the game session
// renders with Vulkan, which MoltenVK translates to Metal. There is no Qt
// on iOS, mirroring how the Android port uses its own frontend.
//
// On-device note: Vita3K's only CPU backend is Dynarmic (a JIT recompiler).
// iOS forbids JIT for App Store apps; a JIT-enabled sideload is therefore
// required on real devices (see Docs/INSTALLING.md). The simulator is not
// affected.

#include "font.h"

#include <app/functions.h>
#include <app/session_controller.h>
#include <config/functions.h>
#include <config/state.h>
#include <emuenv/state.h>
#include <io/state.h>
#include <modules/module_parent.h>
#include <sdl-frontend/session.h>
#include <util/exit_code.h>
#include <util/fs.h>
#include <util/log.h>

#include <stb_image.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <string>
#include <vector>

namespace {

constexpr int LOGICAL_WIDTH = 960;
constexpr int LOGICAL_HEIGHT = 544;

// Android-style dark theme palette.
constexpr SDL_Color COLOR_BG{ 0x0e, 0x10, 0x13, 0xff };
constexpr SDL_Color COLOR_SURFACE{ 0x1a, 0x1e, 0x27, 0xff };
constexpr SDL_Color COLOR_SURFACE_HI{ 0x25, 0x2c, 0x3b, 0xff };
constexpr SDL_Color COLOR_TEXT{ 0xe8, 0xed, 0xf5, 0xff };
constexpr SDL_Color COLOR_TEXT_DIM{ 0x8b, 0x95, 0xa7, 0xff };
constexpr SDL_Color COLOR_ACCENT{ 0x4f, 0x8c, 0xff, 0xff };

struct LauncherRow {
    std::string title_id;
    std::string label;
    SDL_FRect rect;
    SDL_Texture *icon = nullptr;
};

// The launcher bitmap font only covers ASCII; strip anything else.
std::string ascii_label(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7f) {
            out.push_back(static_cast<char>(u));
        } else if (u == '\t' || u == '\n' || u == '\r') {
            out.push_back(' ');
        }
    }
    return out;
}

// Renders a single line of text (5x7 bitmap font) to a texture.
SDL_Texture *make_text(SDL_Renderer *renderer, const std::string &text, SDL_Color color, int scale = 2) {
    constexpr int GLYPH_W = 5;
    constexpr int GLYPH_H = 7;
    constexpr int PAD = 1;

    const int w = static_cast<int>(text.size()) * (GLYPH_W + 1) * scale + PAD * 2;
    const int h = GLYPH_H * scale + PAD * 2;

    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA8888);
    if (!surface)
        return nullptr;

    const uint32_t color32 = SDL_MapSurfaceRGBA(surface, color.r, color.g, color.b, color.a);
    if (SDL_LockSurface(surface)) {
        uint32_t *pixels = static_cast<uint32_t *>(surface->pixels);
        const int pitch_px = surface->pitch / static_cast<int>(sizeof(uint32_t));
        int cursor_x = PAD;
        for (unsigned char c : text) {
            if (c < 0x20 || c > 0x7e)
                c = '?';
            const auto &glyph = FONT5X7[c - 0x20];
            for (int row = 0; row < GLYPH_H; ++row) {
                const unsigned char bits = glyph[row];
                for (int col = 0; col < GLYPH_W; ++col) {
                    if (!((bits >> col) & 1))
                        continue;
                    for (int dy = 0; dy < scale; ++dy) {
                        for (int dx = 0; dx < scale; ++dx) {
                            const int px = cursor_x + col * scale + dx;
                            const int py = PAD + row * scale + dy;
                            pixels[py * pitch_px + px] = color32;
                        }
                    }
                }
            }
            cursor_x += (GLYPH_W + 1) * scale;
        }
    }
    SDL_UnlockSurface(surface);

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

void draw_text(SDL_Renderer *renderer, const std::string &text, SDL_Color color, float x, float y, int scale = 2) {
    SDL_Texture *texture = make_text(renderer, text, color, scale);
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

// Loads a PNG icon (stb_image) into a texture; returns nullptr on failure.
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

// Draws a rounded-ish row card: surface fill, icon, title, title id.
void draw_row(SDL_Renderer *renderer, const LauncherRow &row) {
    const SDL_Color fill = COLOR_SURFACE_HI;
    fill_rect(renderer, fill, row.rect.x, row.rect.y, row.rect.w, row.rect.h);

    const float icon_x = row.rect.x + 14.f;
    const float icon_y = row.rect.y + 8.f;
    constexpr float ICON_SIZE = 56.f;
    if (row.icon) {
        SDL_FRect dst{ icon_x, icon_y, ICON_SIZE, ICON_SIZE };
        SDL_RenderTexture(renderer, row.icon, nullptr, &dst);
    } else {
        fill_rect(renderer, COLOR_ACCENT, icon_x, icon_y, ICON_SIZE, ICON_SIZE);
    }

    draw_text(renderer, row.label, COLOR_TEXT, row.rect.x + 84.f, row.rect.y + 12.f, 2);
    draw_text(renderer, row.title_id, COLOR_TEXT_DIM, row.rect.x + 84.f, row.rect.y + 40.f, 2);
}

void destroy_rows(std::vector<LauncherRow> &rows) {
    for (auto &row : rows) {
        SDL_DestroyTexture(row.icon);
        row.icon = nullptr;
    }
}

int run_launcher(EmuEnvState &emuenv, app::AppSessionController &session) {
    bool quit = false;

    while (!quit) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_SENSOR)) {
            LOG_ERROR("SDL_Init failed in launcher: {}", SDL_GetError());
            return 1;
        }

        SDL_Window *window = SDL_CreateWindow("Vita3K", LOGICAL_WIDTH, LOGICAL_HEIGHT, SDL_WINDOW_FULLSCREEN);
        if (!window) {
            LOG_ERROR("SDL_CreateWindow failed in launcher: {}", SDL_GetError());
            return 1;
        }
        SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
        if (!renderer) {
            LOG_ERROR("SDL_CreateRenderer failed in launcher: {}", SDL_GetError());
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
            row.label = ascii_label(entry.title);
            if (row.label.empty())
                row.label = entry.title_id;
            row.rect = SDL_FRect{ 12.f, 56.f + static_cast<float>(rows.size()) * 76.f, 936.f, 72.f };
            if (!entry.icon_path.empty())
                row.icon = load_icon(renderer, entry.icon_path);
            rows.push_back(std::move(row));
        }

        std::string selected;
        bool boot = false;

        while (!boot && !quit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                switch (event.type) {
                case SDL_EVENT_QUIT:
                    quit = true;
                    break;
                case SDL_EVENT_FINGER_DOWN: {
                    const float px = event.tfinger.x * LOGICAL_WIDTH;
                    const float py = event.tfinger.y * LOGICAL_HEIGHT;
                    for (const auto &row : rows) {
                        if (px >= row.rect.x && px <= row.rect.x + row.rect.w
                            && py >= row.rect.y && py <= row.rect.y + row.rect.h) {
                            selected = row.title_id;
                            boot = true;
                            break;
                        }
                    }
                    break;
                }
                default:
                    break;
                }
            }

            // Background.
            fill_rect(renderer, COLOR_BG, 0.f, 0.f, LOGICAL_WIDTH, LOGICAL_HEIGHT);

            // Top app bar.
            fill_rect(renderer, COLOR_SURFACE, 0.f, 0.f, LOGICAL_WIDTH, 52.f);
            draw_text(renderer, "Vita3K", COLOR_TEXT, 16.f, 14.f, 3);
            draw_text(renderer, "iOS port", COLOR_TEXT_DIM, 900.f, 24.f, 2);

            if (rows.empty()) {
                // Empty library, Android-app style centred hint.
                draw_text(renderer, "No games found", COLOR_TEXT, 320.f, 200.f, 3);
                draw_text(renderer, "Copy a game folder into:", COLOR_TEXT_DIM, 330.f, 244.f, 2);
                draw_text(renderer, "Files app -> Vita3K -> Documents -> ux0/app/<TITLEID>/",
                    COLOR_TEXT_DIM, 260.f, 272.f, 2);
            } else {
                for (const auto &row : rows)
                    draw_row(renderer, row);
            }

            draw_text(renderer, "Tap a title to boot. Physical controller (MFi / PS / Xbox) for buttons.",
                COLOR_TEXT_DIM, 24.f, LOGICAL_HEIGHT - 48.f, 2);
            draw_text(renderer, "Dynarmic JIT needs a JIT-enabled sideload (Docs/INSTALLING.md).",
                COLOR_TEXT_DIM, 24.f, LOGICAL_HEIGHT - 22.f, 2);

            SDL_RenderPresent(renderer);
            SDL_Delay(16);
        }

        destroy_rows(rows);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);

        if (quit)
            break;

        LOG_INFO("Booting title '{}'", selected);
        AppLaunchRequest request;
        request.app_path = selected;
        const int exit_code = sdl_frontend::run(emuenv, session, request);
        LOG_INFO("Game session '{}' ended with code {}", selected, exit_code);

        // The session tears SDL down with it; refresh the app list for the
        // next launcher pass in case content changed.
        app::init_apps_list(emuenv);
    }

    return 0;
}

} // namespace

// SDL3 entry point. SDL3 provides the UIKit application lifecycle; this runs
// on the main thread once the app is up.
int SDL_main(int argc, char *argv[]) {
    // Lock the app to landscape on real devices. The Info.plist already
    // declares landscape-only orientations, but SDL3's UIKit backend drives
    // the view orientation from this hint; without it a portrait-held device
    // rotates the 960x544 frame by 90 degrees and touch coordinates no longer
    // match the UI.
    SDL_SetHint("SDL_HINT_ORIENTATIONS", "LandscapeLeft,LandscapeRight");

    Root root_paths;
    const bool portable = app::init_paths(root_paths);

    if (logging::init(root_paths, true) != Success)
        return InitConfigFailed;

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
    return run_launcher(emuenv, session);
}
