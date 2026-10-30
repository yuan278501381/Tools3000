import { defineConfig } from 'vite'
import type { Plugin } from 'vite'
import react from '@vitejs/plugin-react'
import fs from 'fs'
import path from 'path'

const versionFile = path.resolve(__dirname, '../VERSION')
const tools3000Version = fs.readFileSync(versionFile, 'utf-8').trim()
if (!/^\d+\.\d+\.\d+$/.test(tools3000Version)) {
  throw new Error(`Invalid Tools3000 VERSION: ${tools3000Version}`)
}

// 自定义插件：将 Vite 动态分配的端口写入本地文件，供 C++ 后端在开发模式下动态读取
function writeDevServerUrl(): Plugin {
  return {
    name: 'write-dev-server-url',
    configureServer(server) {
      server.httpServer?.once('listening', () => {
        const address = server.httpServer?.address()
        if (address && typeof address === 'object') {
          const url = `http://localhost:${address.port}`
          const outPath = path.resolve(__dirname, '.dev-server-url')
          fs.writeFileSync(outPath, url, 'utf-8')
          
          console.log(`\nTools3000 动态端口就绪。按住 Ctrl 点击下方链接可在浏览器中打开调试：`)
        }
      })
    }
  }
}

// https://vite.dev/config/
export default defineConfig({
  base: './',
  define: {
    __TOOLS3000_VERSION__: JSON.stringify(tools3000Version),
  },
  plugins: [react(), writeDevServerUrl()],
  build: {
    // 保持表面入口的动态边界：Search/Tray/QuickLook 不再与设置中心一起被
    // 内联到首屏脚本。文件均由 tools3000.local 虚拟主机从本地 ui 目录加载。
    rollupOptions: {
      output: {
        manualChunks(id) {
          const normalized = id.replace(/\\/g, '/');
          if (normalized.includes('node_modules/react/') || normalized.includes('node_modules/react-dom/')) {
            return 'react';
          }
          if (normalized.includes('node_modules/i18next/') || normalized.includes('node_modules/react-i18next/') ||
              normalized.includes('node_modules/i18next-browser-languagedetector/')) {
            return 'i18n';
          }
          if (normalized.includes('node_modules/lucide-react/')) {
            return 'icons';
          }
        },
      },
    },
  },
  server: {
    host: '0.0.0.0', // 监听 0.0.0.0 以在终端展示物料局域网 IP
    strictPort: false // 允许端口被占用时动态递增选择新端口
  }
})
