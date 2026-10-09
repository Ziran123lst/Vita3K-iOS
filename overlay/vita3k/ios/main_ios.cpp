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

// The iOS shell is a minimal SDL3 launcher: it lists the installed titles
// (ux0/app) and boots them through sdl_frontend::run. Rendering inside the
// game session is Vulkan, which MoltenVK translates to Metal. There is no Qt
// on iOS, mirroring how the Android port replaces the Qt GUI with a native
// frontend.
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

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int LOGICAL_WIDTH = 960;
constexpr int LOGICAL_HEIGHT = 544;

struct LauncherRow {
    std::string title_id;
    std::string label;
    SDL_FRect rect;
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

// Renders a single line of text (5x7 bitmap font, 2x scale) to a texture.
SDL_Texture *make_text(SDL_Renderer *renderer, const std::string &text, SDL_Color color) {
    constexpr int GLYPH_W = 5;
    constexpr int GLYPH_H = 7;
    constexpr int SCALE = 2;
    constexpr int PAD = SCALE;

    const int w = static_cast<int>(text.size()) * (GLYPH_W + 1) * SCALE + PAD * 2;
    const int h = GLYPH_H * SCALE + PAD * 2;

    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA8888);
    if (!surface)
        return nullptr;

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
                for (int dy = 0; dy < SCALE; ++dy) {
                    for (int dx = 0; dx < SCALE; ++dx) {
                        SDL_WritePixel(surface, cursor_x + col * SCALE + dx, PAD + row * SCALE + dy,
                            SDL_MapSurfaceRGBA(surface, color.r, color.g, color.b, color.a));
                    }
                }
            }
        }
        cursor_x += (GLYPH_W + 1) * SCALE;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

void draw_text(SDL_Renderer *renderer, const std::string &text, SDL_Color color, float x, float y) {
    SDL_Texture *texture = make_text(renderer, text, color);
    if (!texture)
        return;
    float w = 0.f;
    float h = 0.f;
    SDL_GetTextureSize(texture, &w, &h);
    SDL_FRect dst{ x, y, w, h };
    SDL_RenderTexture(renderer, texture, nullptr, &dst);
    SDL_DestroyTexture(texture);
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
            row.label = entry.title_id + "   " + ascii_label(entry.title);
            rows.push_back(std::move(row));
        }
        for (std::size_t i = 0; i < rows.size(); ++i) {
            rows[i].rect = SDL_FRect{ 24.f, 84.f + static_cast<float>(i) * 46.f, LOGICAL_WIDTH - 48.f, 40.f };
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

            SDL_SetRenderDrawColor(renderer, 0x10, 0x12, 0x1a, 0xff);
            SDL_RenderClear(renderer);

            draw_text(renderer, "Vita3K for iOS", SDL_Color{ 235, 240, 250, 255 }, 24.f, 24.f);
            draw_text(renderer, "Vulkan rendered on Metal via MoltenVK", SDL_Color{ 130, 140, 160, 255 }, 24.f, 50.f);

            if (rows.empty()) {
                draw_text(renderer, "No games found.", SDL_Color{ 200, 205, 215, 255 }, 24.f, 96.f);
                draw_text(renderer, "Copy a game folder into:", SDL_Color{ 130, 140, 160, 255 }, 24.f, 122.f);
                draw_text(renderer, "Files app -> Vita3K -> Documents -> ux0/app/<TITLEID>/", SDL_Color{ 130, 140, 160, 255 }, 24.f, 146.f);
            } else {
                for (const auto &row : rows) {
                    SDL_SetRenderDrawColor(renderer, 0x1c, 0x20, 0x2e, 0xff);
                    SDL_RenderFillRect(renderer, &row.rect);
                    draw_text(renderer, row.label, SDL_Color{ 205, 210, 222, 255 }, row.rect.x + 10.f, row.rect.y + 8.f);
                }
            }

            draw_text(renderer, "Tap a title to boot. Use a physical controller (MFi / PS / Xbox) for buttons.",
                SDL_Color{ 110, 118, 134, 255 }, 24.f, LOGICAL_HEIGHT - 60.f);
            draw_text(renderer, "On-device Dynarmic JIT requires a JIT-enabled sideload (see Docs/INSTALLING.md).",
                SDL_Color{ 110, 118, 134, 255 }, 24.f, LOGICAL_HEIGHT - 34.f);

            SDL_RenderPresent(renderer);
            SDL_Delay(16);
        }

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
