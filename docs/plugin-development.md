# Tools3000 plugin contract

Tools3000 discovers plugins as `Plugin_*.dll` files in the `plugins` directory. Every DLL must have a same-stem sidecar manifest, for example:

```text
Plugin_Example.dll
Plugin_Example.plugin.json
```

The host validates the manifest before mapping the DLL. Disabled, malformed, incompatible, and too-new plugins therefore do not load their transitive dependencies.

## Manifest schema 1

```json
{
  "schemaVersion": 1,
  "abiVersion": 1,
  "id": "example",
  "name": "Example Plugin",
  "version": "1.0.0",
  "minimumHostVersion": "1.0.0",
  "entryPoint": "CreatePlugin",
  "capabilities": ["example-action"],
  "permissions": ["clipboard"]
}
```

- `id` must match the DLL stem after removing `Plugin_` and normalizing it to lowercase.
- Versions are numeric `major.minor.patch` values. Missing minor or patch components are treated as zero.
- Capabilities describe user-facing features. Permissions disclose sensitive operating-system access.
- Lists contain at most 64 unique tokens. Tokens may contain ASCII letters, digits, `.`, `_`, and `-`.

## Binary ABI handshake

ABI 1 plugins export both functions with C linkage:

```cpp
extern "C" __declspec(dllexport) std::uint32_t GetPluginAbiVersion() {
    return tools3000::core::CurrentPluginAbiVersion;
}

extern "C" __declspec(dllexport) tools3000::core::IPlugin* CreatePlugin();
```

The ABI export must match the sidecar before `CreatePlugin` is called. Plugin metadata returned by `IPlugin` must also match the manifest.

## Lifetime rules

`shutdown()` must stop and join plugin workers, destroy plugin windows, unregister every hotkey and IPC method, and use the blocking EventBus unsubscribe path for callbacks that may already be running. No function object whose code belongs to the plugin may remain stored in a core singleton when the DLL is unloaded.

Plugin setting changes intentionally take effect after restart. This keeps the runtime boundary deterministic while still avoiding the memory and startup cost of disabled modules.
