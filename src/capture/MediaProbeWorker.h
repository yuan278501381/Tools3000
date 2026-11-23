#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MediaProbeWorker.h — QuickLook 视频元信息与关键帧胶片条短命隔离 Worker
//
// 职责:
//   以独立短命子进程 (Tools3000.exe --worker=media-probe --job=<json>) 复用
//   Plugin_Capture.dll 已延迟加载的 FFmpeg (avformat/avcodec/swscale)，为 Chromium
//   不支持的视频容器/编码 (.avi/.mkv HEVC/.mov ProRes/.wmv/.flv/.ts 等) 快速提取:
//     - 时长、分辨率、帧率、码率、视频/音频编码名称
//     - 4~6 张均匀分布的关键帧缩略图 (JPEG Data URI)
//   退场后由操作系统 100% 回收物理与虚拟内存，宿主进程 0 常驻 FFmpeg 开销。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_MEDIAPROBEWORKER_H
#define TOOLS3000_CAPTURE_MEDIAPROBEWORKER_H

namespace tools3000::capture {

int runMediaProbeWorkerProcess(int argc, const wchar_t* const* argv);

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_MEDIAPROBEWORKER_H
