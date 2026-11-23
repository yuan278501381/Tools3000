## Summary / 变更摘要

<!-- Briefly describe what problem this PR solves or what feature it introduces. / 简述本 PR 解决的问题或新增的功能。 -->

- **User-Facing Impact / 用户侧影响**:
- **Technical Approach / 技术方案**:

## Related Issue / 关联 Issue

<!-- e.g. Fixes #123 -->

## Verification Checklist / 自检清单

- [ ] Frontend & static gates pass (`cd ui && npm run lint && npx tsc -b && npm test`)
- [ ] All 8 UI locale files (`en`, `zh`, `zh-TW`, `ja`, `ko`, `ru`, `pt-BR`, `es`) are updated in lockstep if UI text changed
- [ ] Native C++ / Rust unit tests added or updated for core logic changes
- [ ] Compatible with both `x64` and `ARM64` Windows builds and non-ASCII Unicode paths
- [ ] Zero native Unicode emojis in UI code, logs, and commit messages
