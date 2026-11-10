# 发布签名与供应链材料

正式发布必须对 Tools3000 自有的 EXE、DLL 和安装包执行 Authenticode SHA-256 签名及时间戳验证。开发构建在没有证书时可以跳过；`scripts/release.ps1` 和 `scripts/publish_release.ps1` 会启用强制签名门禁。

## 签名身份

推荐使用 Windows 当前用户 `My` 证书库中的代码签名证书：

```powershell
$env:TOOLS3000_SIGNING_CERT_SHA1 = "40位证书SHA-1指纹"
```

CI 也可使用 PFX：

```powershell
$env:TOOLS3000_SIGNING_PFX = "C:\secure\tools3000-signing.pfx"
$env:TOOLS3000_SIGNING_PASSWORD = "由CI密钥存储注入"
```

可用 `TOOLS3000_TIMESTAMP_URL` 覆盖默认 RFC 3161 时间戳服务。不要把证书、密码或上述环境变量写入仓库。

签名门禁会同时验证 Authenticode 信任状态和时间戳证书；仅产生未加时间戳签名或警告也会导致正式发布失败。

## 随包材料

每次打包会从当前构建的 vcpkg 状态文件、固定版本 WebView2 SDK 和 `ui/package-lock.json` 生成：

- `THIRD_PARTY_NOTICES.txt`：第三方许可证和通知正文；
- `SBOM.spdx.json`：SPDX 2.3 软件物料清单；
- `LICENSE`：Tools3000 源码许可证。

正式发布的 SHA-256 校验和基于完成签名后的最终字节生成。
