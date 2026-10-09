# Vita3K-iOS

Vita3K — the PlayStation Vita emulator — for iOS, rendering through **Metal**.

Vita3K's rendering backend is Vulkan. iOS has no native Vulkan driver, so this
port follows the exact same strategy Vita3K already uses on macOS:
[MoltenVK](https://github.com/KhronosGroup/MoltenVK) translates every Vulkan
call to Metal at runtime. No OpenGL, no software rendering — the GPU is driven
through Metal end to end.

This repository does **not** fork the whole upstream tree. It is a thin,
patch-based port:

- `Patches/` — five patches that adapt the pinned upstream source for iOS
  (CMake, MoltenVK/OpenSSL/cubeb handling, data paths, audio backend).
- `overlay/` — the iOS application shell (`vita3k/ios/`): a small SDL3
  launcher that lists installed titles and boots them through Vita3K's
  upstream `sdl-frontend`.
- `Scripts/` — bootstrap, patch, build and package scripts.
- `.github/workflows/` — CI that cross-compiles unsigned `.app` bundles for
  device and simulator and uploads them as artifacts.

## Status

| Area | Status |
| --- | --- |
| Vulkan → Metal rendering (MoltenVK) | Wired in the build (iOS MoltenVK dylib embedded) |
| iOS app shell (SDL3 launcher + `sdl-frontend`) | Implemented in `overlay/` |
| Dynarmic JIT on-device | Requires a JIT-enabled sideload (see below) |
| Audio | SDL audio backend only (cubeb disabled on iOS) |
| First end-to-end build | Exercised by CI — check the latest run |

This is a bring-up port, not a polished emulator app yet: expect rough edges,
and expect games to be slow or broken. The point of this repository is to get
the whole toolchain (patches → build → sideloadable app) reproducible and
open, so the actual game-by-game bring-up work can happen in the open.

## How rendering reaches Metal

```
Vita3K (Vulkan) ──► MoltenVK (libMoltenVK.dylib) ──► Metal ──► A13/A16/M-series GPU
```

- Upstream's default renderer is already `Vulkan`; no code change is needed.
- `SDL3` on iOS (UIKit driver) creates the Vulkan surface via
  `SDL_Vulkan_GetInstanceExtensions` / `SDL_Vulkan_CreateSurface`, and the
  underlying CAMetalLayer is driven by the OS compositor.
- `MoltenVK-ios.tar` (v1.4.1) is downloaded during configure and embedded in
  the bundle, matching how the official macOS build embeds `libMoltenVK.dylib`.

## Quick start (download a build)

1. Open the **Actions** tab, pick the latest run of the *iOS build* workflow.
2. Download the `Vita3K-device` artifact (or `Vita3K-simulator`).
3. Sideload per [Docs/INSTALLING.md](Docs/INSTALLING.md) — note the **JIT
   requirement** for real devices.

## Building locally (macOS + Xcode)

```bash
bash Scripts/bootstrap.sh     # clone pinned upstream + submodules, overlay shell
bash Scripts/apply-patches.sh # apply Patches/*.patch on top
bash Scripts/build-ios.sh device     # or: simulator
bash Scripts/package-ios.sh device   # → dist/Vita3K-iOS-device-unsigned.zip
```

Requirements: macOS (CI uses `macos-14`), Xcode command line tools, CMake ≥ 3.25,
Ninja. OpenSSL and Boost are built from source for iOS by the patched CMake
during configure/build.

## JIT and why a normal sideload is not enough

Vita3K's only CPU backend is **Dynarmic**, a JIT recompiler. iOS marks memory
that is both writable and executable as forbidden for App Store apps
(`W^X`), and the OS refuses to launch them. To run the JIT on a real device
you need one of:

- **AltStore / SideStore with JIT** — the standard community route (the app
  gets a JIT entitlement through the helper).
- A **jailbreak** with an entitlement plist granting
  `com.apple.security.cs.allow-jit`.
- **Developer tools** (`devicectl` / Xcode) for your own device with a
  development signing identity — entitlements can then include
  `com.apple.security.cs.allow-jit`.

The simulator does not enforce `W^X` and runs the JIT without extra steps.
See [Docs/INSTALLING.md](Docs/INSTALLING.md) for details.

## Repository layout

```
Patches/                       # 0001..0005 iOS adaptation patches (git apply)
overlay/vita3k/ios/            # iOS app shell (CMake, SDL3 launcher, Info.plist)
Scripts/                       # bootstrap / apply-patches / build-ios / package-ios
.github/workflows/build-ios.yml# CI: unsigned .app for device + simulator
Docs/INSTALLING.md             # sideloading, JIT, entitlements
LICENSE                        # GPL-2.0 (same as upstream)
```

## License

This project is GPL-2.0-licensed, matching the upstream
[Vita3K/Vita3K](https://github.com/Vita3K/Vita3K) project it ports. MoltenVK
is Apache-2.0 / MIT-licensed by the Khronos Group and is downloaded as a
binary dependency at configure time, exactly as upstream does for macOS.

**Legal note:** emulators are legal software in most jurisdictions, but game
firmware and game dumps are copyrighted. You must dump your own PS Vita
firmware and games from hardware you own. This project does not ship any
copyrighted Sony content.
