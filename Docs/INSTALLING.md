# Sideloading and running Vita3K on iOS

## What you need

- An iPhone or iPad running iOS/iPadOS 15.0 or later (arm64).
- A Mac (for initial sideloads) or a PC with AltStore/other sideload tooling.
- Your own PS Vita firmware dump and game dumps (see the legal note in the
  README).

## Downloading a build

The CI workflow produces one artifact per run:

- `Vita3K-device` — unsigned `Vita3K.app` for real devices (`iphoneos` SDK,
  arm64, iOS 15+).

Download the artifact from the Actions page and unzip it.

## Real device: the JIT requirement

Vita3K uses Dynarmic, a dynamic recompiler (JIT). iOS enforces `W^X`: no
memory region can be simultaneously writable and executable, except when a
specific entitlement is present:

```
com.apple.security.cs.allow-jit
```

A **stock, App-Store-style signature will not run this app** — the process is
killed on launch or on first JIT page fault. Pick one of these routes:

### 1. AltStore / SideStore with JIT (recommended, no jailbreak)

1. Install AltStore on your iPhone and trust the certificate
   (Settings → General → VPN & Device Management).
2. Install the unsigned app via AltStore (or Sideloadly).
3. Launch **AltStore → JIT** (on-device JIT toggle) or use
   `altserver`'s JIT command from the Mac to enable the JIT entitlement for
   the running app, then launch Vita3K from the Home screen.

Note: free Apple IDs re-sign apps for 7 days; the JIT enable step must be
repeated after each reboot/re-sign.

### 2. Jailbroken device

Install the app and add an entitlement plist with:

```xml
<key>com.apple.security.cs.allow-jit</key>
<true/>
```

then re-sign and install (`ldid` / `codesign` with the entitlements).

### 3. Developer signing (your own device only)

With a paid developer account, sign with your development certificate and
embed the same `allow-jit` entitlement via Xcode/`codesign`. On developer
devices the entitlement is honored for development-signed binaries.

## Simulator (no JIT workaround needed)

The official MoltenVK iOS release only ships a device (`ios-arm64`) slice, so
CI builds the device target only. To run in the iOS Simulator you must build
MoltenVK for the simulator yourself (arm64 Macs only; Rosetta/x86_64 is not
supported by this build), then build the `simulator` target with
`Scripts/build-ios.sh simulator`.

## Getting content in

The app exposes its Documents directory through the Files app
(`UIFileSharingEnabled`). Structure it the Vita3K way:

```
Vita3K Documents/ux0/app/<TITLEID>/          ← installed games (eboot.bin + LiveArea)
Vita3K Documents/ux0/                ← optional data dirs (etc.)
```

Put your `PSP2UPDATE.PUP`-based firmware files where the app expects them
per Vita3K conventions (`ux0/data`/system paths), then launch the app; the
launcher lists installed titles. Tap a title to boot it.

## Controls

Touch screen maps to the Vita touch screen; buttons and analog sticks require
a physical controller (MFi / PS4 / PS5 / Xbox controllers work via the
GameController framework).
