# Tools3000 — Lua 脚本 API 参考

手势动作可绑定到 Lua 脚本（动作类型选择 **Lua 脚本**）。脚本运行在内置的
Lua 5.4 沙箱中（基于 [sol2](https://github.com/ThePhD/sol2)）。所有原生能力通过
全局表 `easy` 暴露（`tools3000` 与旧名 `tools3000` 为其别名，向后兼容）。

> 沙箱：已开放 `base / string / math / table / coroutine / utf8 / os(受限)`。
> 出于安全考虑 **未开放 `io`**，且 `os.execute / os.remove / os.rename / os.exit /
> os.tmpname / os.setlocale` 已被移除。需要启动进程请使用 `easy.shell.run`。
>
> 此环境面向用户本机可信自动化脚本，不是执行不可信代码的安全边界；完整的能力与
> 限制评估见 [Lua 安全模型](../lua-security-model.md)。

字符串编码统一为 **UTF-8**。

---

## easy.log — 日志
| 函数 | 说明 |
|------|------|
| `easy.log.info(msg)` | 写入 INFO 日志 |
| `easy.log.warn(msg)` | 写入 WARN 日志 |
| `easy.log.error(msg)` | 写入 ERROR 日志 |

## easy.keyboard — 键盘
| 函数 | 说明 |
|------|------|
| `easy.keyboard.sendKeys(combo)` | 合成组合键，如 `"Ctrl+Shift+T"`、`"Alt+F4"`、`"Win+D"` |
| `easy.keyboard.keyDown(vk)` | 按下某个虚拟键码 |
| `easy.keyboard.keyUp(vk)` | 释放某个虚拟键码 |

支持的键名：`Ctrl/Alt/Shift/Win` 修饰键；`F1..F12`、`Tab/Enter/Space/Esc/
Delete/Backspace/Home/End/Left/Right/Up/Down/PageUp/PageDown/Insert`；单个字母数字。

## easy.mouse — 鼠标
| 函数 | 说明 |
|------|------|
| `easy.mouse.moveTo(x, y)` | 移动光标到屏幕坐标 |
| `easy.mouse.click(button)` | 点击：`1`=左键，`2`=右键，`3`=中键 |
| `easy.mouse.scroll(amount)` | 滚轮滚动，正数向上、负数向下（单位：刻度） |

## easy.clipboard — 剪贴板
| 函数 | 说明 |
|------|------|
| `easy.clipboard.getText()` | 返回剪贴板文本（无则空串） |
| `easy.clipboard.setText(s)` | 写入文本到剪贴板，返回是否成功 |

## easy.shell — 进程 / 打开
| 函数 | 说明 |
|------|------|
| `easy.shell.open(target)` | 用关联程序打开文件、文件夹或 URL |
| `easy.shell.run(cmd)` | 启动命令行并 **等待** 结束（最多 30s），返回是否成功 |
| `easy.shell.runAsync(cmd)` | 启动命令行，不等待，返回是否成功 |

## easy.window — 窗口
所有函数的句柄参数可省略，默认作用于当前前台窗口。
| 函数 | 说明 |
|------|------|
| `easy.window.getForeground()` | 返回前台窗口句柄（整数） |
| `easy.window.getTitle([hwnd])` | 窗口标题 |
| `easy.window.getClass([hwnd])` | 窗口类名 |
| `easy.window.getProcess([hwnd])` | 所属进程名（如 `chrome.exe`） |
| `easy.window.minimize([hwnd])` | 最小化 |
| `easy.window.maximize([hwnd])` | 最大化 |
| `easy.window.restore([hwnd])` | 还原 |
| `easy.window.close([hwnd])` | 关闭窗口 |
| `easy.window.setTopmost(bool, [hwnd])` | 置顶 / 取消置顶 |

## easy.screen — 屏幕
| 函数 | 说明 |
|------|------|
| `easy.screen.getPixelColor(x, y)` | 返回 `{r=, g=, b=}`（0–255） |

## easy.fs — 文件
| 函数 | 说明 |
|------|------|
| `easy.fs.exists(path)` | 路径是否存在 |
| `easy.fs.readFile(path)` | 读取文件内容（二进制安全），失败返回空串 |
| `easy.fs.writeFile(path, content)` | 覆盖写入文件，返回是否成功 |

## easy.http — HTTP（WinHTTP）
| 函数 | 说明 |
|------|------|
| `easy.http.get(url)` | 返回 `{status=, body=}` |
| `easy.http.post(url, [body])` | `application/x-www-form-urlencoded` POST，返回 `{status=, body=}` |

## easy.ui — 交互
| 函数 | 说明 |
|------|------|
| `easy.ui.toast(msg)` | 信息提示框 |
| `easy.ui.notify(title, msg)` | 带标题的提示框 |
| `easy.ui.confirm(msg)` | 是/否确认框，返回 `true`/`false` |
| `easy.ui.inputBox(prompt)` | 简易输入（当前回填剪贴板内容，后续接入 WebView 弹窗） |

## easy.url — URL 编解码
| 函数 | 说明 |
|------|------|
| `easy.url.encode(s)` | percent-encode |
| `easy.url.decode(s)` | percent-decode（`+` 解为空格） |

---

## 示例

```lua
-- 选中文本 → 是 URL 就打开, 否则搜索
easy.keyboard.sendKeys("Ctrl+C")
local t = easy.clipboard.getText()
if t:match("^https?://") then
    easy.shell.open(t)
else
    easy.shell.open("https://www.google.com/search?q=" .. easy.url.encode(t))
end
```

更多示例见 [`resources/scripts/`](../../resources/scripts/)。
