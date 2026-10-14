-- ─────────────────────────────────────────────────────────────────────────────
-- translate_clipboard.lua — 翻译剪贴板文本
--
-- 行为: 读取剪贴板文本, 在默认浏览器中打开 Google 翻译 (自动检测源语言 → 中文)。
-- 若剪贴板为空, 弹出提示。
-- ─────────────────────────────────────────────────────────────────────────────

local text = easy.clipboard.getText()
if not text or text == "" then
    easy.ui.toast("剪贴板没有可翻译的文本")
    return
end

text = text:gsub("^%s+", ""):gsub("%s+$", "")

local url = "https://translate.google.com/?sl=auto&tl=zh-CN&op=translate&text="
         .. easy.url.encode(text)
easy.shell.open(url)
easy.log.info("translate_clipboard: 已打开翻译页面")
