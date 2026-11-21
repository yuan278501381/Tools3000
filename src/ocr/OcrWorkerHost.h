#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OcrWorkerHost.h — 独立短命 OCR Worker 进程服务端主循环
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_OCR_OCRWORKERHOST_H
#define TOOLS3000_OCR_OCRWORKERHOST_H

namespace tools3000::ocr {

/// 执行 --worker=ocr 子进程服务循环（闲置超时或收到 Shutdown/父进程退出即正常返回释放全部 GPU 虚拟内存）
int runOcrWorkerProcess(int argc, const wchar_t* const* argv);

} // namespace tools3000::ocr

#endif // TOOLS3000_OCR_OCRWORKERHOST_H
