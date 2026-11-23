# Privacy Policy

**Effective Date:** November 24, 2026

**Tools3000** is an open-source, local-first desktop productivity suite for Windows licensed under GPL-3.0. Protecting user privacy and sensitive local data is a foundational architectural requirement.

---

## 1. Zero Telemetry & Zero Tracking

- Tools3000 collects **zero telemetry, zero analytics, zero crash tracking, and zero usage statistics**.
- No user files, filenames, document contents, clipboard items, screenshots, recordings, keystrokes, text-expansion rules, or mouse gestures are ever transmitted to any server operated by the authors or third parties.

## 2. Local Data Storage

All configuration files, search indexes, persistent clipboard history, capture region presets, offline OCR models, and diagnostic logs are stored strictly on your local machine:
- **Standard Installer**: `%LOCALAPPDATA%\Tools3000\`
- **Portable Edition**: `<AppFolder>\data\` (or `.tools3000\`)

Clipboard captures automatically exclude known password managers (such as KeePass, 1Password, Bitwarden, Enpass, LastPass, RoboForm, and Dashlane) and honor Windows clipboard privacy format flags (`ExcludeClipboardContentFromMonitorProcessing`).

## 3. Optional Network Features (User-Initiated Only)

Tools3000 makes outbound network connections **only** when explicitly initiated or configured by the user:

1. **Update Check**: Queries the public GitHub Releases API (`api.github.com/repos/yuan278501381/Tools3000/releases/latest`) to check for new versions and download verified release packages upon user confirmation.
2. **On-Demand Offline OCR Model Download**: Downloads open-source ONNX recognition models from ModelScope (`modelscope.cn` / `modelscope.ai`) only when the user clicks Download in Settings. Once downloaded, all OCR inference runs 100% offline.
3. **Optional Translation API**: Screen translation sends recognized text snippets only to the translation endpoint configured by the user in Settings.
4. **In-App Issue Feedback**: Generates a local `.zip` diagnostic bundle on your device after running a 10-rule local privacy scrubber (redacting Windows usernames in paths, emails, IPv4 addresses, tokens, and API keys; clipboard contents are never included). Nothing is uploaded unless you explicitly submit it.

## 4. Local CLI & MCP Server Security Boundary

The bundled `tools3000-cli` / MCP server is **disabled by default** (`externalApiEnabled = false`). Even when explicitly enabled by the user in Settings, it is strictly read-only and enforces a hardcoded blocklist preventing access to credential directories and files (`.ssh`, `.gnupg`, `.aws`, `.env`, private keys, and browser credential stores).

## 5. Code Signing & Build Provenance

Official release binaries are built from the public source code in [`yuan278501381/Tools3000`](https://github.com/yuan278501381/Tools3000) and published alongside `SHA256SUMS.txt` and an SPDX Software Bill of Materials (`SBOM.spdx.json`).
