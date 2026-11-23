# Tools3000 应用内问题反馈 Cloudflare Worker 中继（方案 C）

本目录提供约 150 行零服务器运维成本的 Cloudflare Worker 中继参考实现，配合 Tools3000 客户端的「应用内问题反馈」双轨机制运行：

- **客户端未配置中继或离线时**：自动无缝降级为「本地导出 10 维脱敏 `.zip` 诊断包 + 一键预填 GitHub Issue + 复制 Markdown 报告」。
- **部署本 Worker 并配置中继地址后**：用户在软件内点击提交即可免 GitHub 账号直达反馈通道，日志/截图物理隔离存入私有 R2，仅脱敏摘要创建为 GitHub Issue 并即时回显追踪单号。

## 三步快速部署

1. 安装并登录 Wrangler CLI：
   ```bash
   npm i -g wrangler
   wrangler login
   ```
2. 写入最小权限 GitHub Token（仅需 `issues:write`）：
   ```bash
   cd scripts/feedback-worker
   wrangler secret put GITHUB_TOKEN
   ```
3. （可选）创建私有 R2 存储桶并在 `wrangler.toml` 取消注释 `[[r2_buckets]]`，随后部署：
   ```bash
   wrangler deploy
   ```
4. 在 Tools3000 配置文件 `config.json` 中设置 `/feedback/relayEndpoint` 为你的 HTTPS Worker URL（如 `https://tools3000-feedback-relay.<account>.workers.dev`），即可启用一键直交。
