# Contributing to Tools3000

Thank you for your interest in contributing to **Tools3000**! Whether you are fixing a bug, proposing a feature, improving performance, or refining one of our 8 UI languages, contributions from both human developers and AI-assisted workflows are warmly welcome.

> 中文开发者请同时参阅 [开发者指南 (docs/DEVELOPER.md)](../docs/DEVELOPER.md)。

---

## Architecture Overview

Tools3000 is a native Windows desktop productivity suite built with zero Electron overhead:

- **Native C++20 Microkernel & Plugins (`src/`, `CMakeLists.txt`)**: Low-level input hooks, Direct2D / DirectComposition hardware-accelerated overlays, screen capture / auto-zoom studio recorder, offline PP-OCR & 1:1 in-place translation, File Dialog enhancer, File Locksmith, and Text Expander.
- **High-Performance Rust Crates**:
  - `src/service/rust_content/`: SIMD multi-format full-text document search engine;
  - `src/clipboard/rust_clipboard/`: Crash-safe persistent clipboard storage engine;
  - `src/cli/`: Zero-dependency ~350 KB CLI & Model Context Protocol (`stdio` MCP) server;
  - `src/viewer/`: <2 MB standalone Rust + Direct2D Picasa-style photo viewer (`Tools3000_Picasa_Viewer.exe`).
- **Settings, Search & Tray Frontend (`ui/`)**: React 19 + TypeScript + Vite hosted on Microsoft Edge WebView2 (created and suspended on demand).

---

## Prerequisites (Windows 10 / 11, x64 or ARM64)

1. **Visual Studio 2022 / Build Tools** (Desktop development with C++, C++20, Windows 10/11 SDK)
2. **CMake >= 3.25** & **vcpkg**
3. **Rust stable toolchain** (`rustup`)
4. **Node.js >= 20** & `npm`
5. **PowerShell 7 (`pwsh`)**

You can bootstrap local build tools via `pwsh ./scripts/setup_local_build_tools.ps1`.

---

## Building & Running Tests

### 1. Full Local Build & End-to-End Verification
```powershell
pwsh -NoProfile -File .\deploy.ps1 -Configuration Release
```
This orchestrates the UI bundle, Rust crates, C++ native binaries, unit tests, and OS-level end-to-end verification gates.

### 2. Frontend & Repository-Wide Static Gates (Fast, Cross-Platform)
```bash
cd ui
npm ci
npm run lint      # ESLint + 7 static gates (9-layer i18n, C++ log catalog, CSS tokens, typography, path encoding)
npx tsc -b        # Strict TypeScript check
npm test          # Vitest unit & component suite
```

### 3. Rust CLI & MCP Server
```bash
cargo test --manifest-path src/cli/Cargo.toml
cargo build --release --manifest-path src/cli/Cargo.toml
```

---

## Engineering & Quality Standards

1. **Cross-Architecture Portability**: Native code must support both `x64` and `ARM64` (`#ifdef _M_X64` / `#ifdef _M_ARM64` guards for architecture-specific intrinsics).
2. **Unicode Path Safety**: Always use `WinUtils::pathToUtf8()` / `utf8ToPath()` and wide Win32 APIs (`*W`) so non-ASCII Windows usernames and installation paths work across all system codepages.
3. **8-Language Localization Parity**: Every user-facing string must be added across all 8 locale files in `ui/src/i18n/locales/` (`en`, `zh`, `zh-TW`, `ja`, `ko`, `ru`, `pt-BR`, `es`) and pass `npm run lint`.
4. **Zero Native Emoji in UI & Logs**: Use crisp Lucide vector SVG icons in UI components and structured ASCII status tags (`[OK]`, `[INFO]`, `[WARN]`, `[ERROR]`) in console/log output.
5. **Regression Tests Required**: Every bug fix or core logic change must include a corresponding C++ GoogleTest (`tests/unit/`) or Vitest (`ui/src/**/*.test.*`) case.

---

## Submitting a Pull Request

1. Fork the repository and create a topic branch from `main`.
2. Ensure `npm run lint`, `npx tsc -b`, `npm test` (under `ui/`), and relevant native/Rust tests pass cleanly.
3. Open a Pull Request describing **what user problem it solves** and **how it was verified**.

By contributing to Tools3000, you agree that your contributions will be licensed under the [GPL-3.0 License](../LICENSE) with the [Windows System & WebView2 Linking Exception](../LICENSE-EXCEPTION.md).
