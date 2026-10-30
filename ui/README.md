# Tools3000 UI

Tools3000 的 React 19 + TypeScript 设置、搜索和托盘界面。生产构建由
Vite 会输出带内容哈希的多文件 chunk，由原生 WebView2
容器通过 `https://tools3000.local/` 虚拟域加载；前后端通过带请求 ID、
超时和错误回传的消息桥通信。

## 本地开发

```powershell
npm ci
npm run dev
```

原生 Debug 构建会连接 `http://localhost:5173`。脱离原生宿主打开时，
`useBridge` 提供可预测的开发 Mock，便于检查页面状态。

## 质量检查

```powershell
npm run lint
npm run i18n-check
npm run build
```

- `lint`：ESLint 与 React Hooks 规则。
- `i18n-check`：校验中英文键集合一致。
- `build`：TypeScript 工程检查并生成单文件生产资源。

产品版本只维护在根目录 `VERSION`。Vite 在开发与生产构建时读取该文件并
注入 `__TOOLS3000_VERSION__`；原生宿主仍会通过消息桥返回已编译版本。
`package.json` 是私有前端工程清单，不再冒充产品版本源。
