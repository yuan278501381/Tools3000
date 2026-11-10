# Tools3000 版本管理

Tools3000 的产品版本采用稳定 SemVer（`MAJOR.MINOR.PATCH`），唯一事实源是仓库
根目录的 `VERSION` 文件。发布新版本时只修改这一行，禁止在构建过程中批量替换
源码里的版本字符串。

版本由以下消费者自动读取：

- CMake 生成 C++ 版本常量、Windows VERSIONINFO 和原生插件清单；
- Vite 注入设置界面、开发 Mock 与展示页面；
- `deploy.ps1` 将版本传给 Inno Setup，并用于符号包等产物命名；
- `scripts/build.ps1` 用于便携包命名；
- `scripts/publish_release.ps1` 默认生成同版本 Git 标签，并拒绝不匹配标签；
- `scripts/verify-version.ps1` 可在本地上传前校验标签与 `VERSION` 一致。

`ui/package.json` 是私有前端工程清单，`vcpkg.json` 是依赖清单；它们不是
Tools3000 产品版本源，因此不再复制产品版本。

发布步骤：

1. 将 `VERSION` 修改为目标版本，例如 `1.1.0`。
2. 在本地完成测试和 Release 构建。
3. 运行 `scripts/verify-version.ps1 -Tag v1.1.0` 校验版本。
4. 创建 `v1.1.0` 标签并手工上传，或运行本地 `scripts/publish_release.ps1`。

如果版本格式、发布标签或构建输入不一致，本地配置或发布脚本必须立即失败，不允许生成
版本身份不明的安装包。
