// ─────────────────────────────────────────────────────────────────────────────
// core/filter.rs — 窗口资格过滤策略（纯规则判定，跳过系统外壳、隐身 UWP、
//                  零尺寸、工具浮窗、Tools3000 自身覆盖层与用户排除应用列表）
// ─────────────────────────────────────────────────────────────────────────────

use crate::core::constants::{
    CONTROL_WINDOW_CLASS, FOCUS_BORDER_WINDOW_CLASS, LAYOUT_EDITOR_WINDOW_CLASS,
    PIN_BORDER_WINDOW_CLASS, QUICK_SWITCH_WINDOW_CLASS, SNAP_ZONES_WINDOW_CLASS,
};
use crate::core::geometry::RectI;

/// 系统壳层与绝不参与窗口管理的保留类名（全量跳过）
const IGNORED_SYSTEM_CLASSES: &[&str] = &[
    "Shell_TrayWnd",
    "Shell_SecondaryTrayWnd",
    "Progman",
    "WorkerW",
    "DV2ControlHost",
    "MsgrIMEWindowClass",
    "SysShadow",
    "Button",
    "Windows.UI.Core.CoreWindow",
    "EdgeUiInputTopWndClass",
    "EdgeUiInputWndClass",
    "MultitaskingViewFrame",
    "TaskListThumbnailWnd",
    "ForegroundStaging",
    "ApplicationManager_DesktopShellWindow",
    "NotifyIconOverflowWindow",
    "TopLevelWindowForOverflowXamlIsland",
    "Xaml_WindowedPopupClass",
    "tooltips_class32",
    CONTROL_WINDOW_CLASS,
    QUICK_SWITCH_WINDOW_CLASS,
    SNAP_ZONES_WINDOW_CLASS,
    LAYOUT_EDITOR_WINDOW_CLASS,
    PIN_BORDER_WINDOW_CLASS,
    FOCUS_BORDER_WINDOW_CLASS,
];

/// 纯数据窗口属性快照（使过滤规则完全可进行无 HWND 确定性单元测试）
#[derive(Debug, Clone)]
pub struct WindowCandidateMeta {
    pub is_visible: bool,
    pub is_cloaked: bool,
    pub is_minimized: bool,
    pub is_hung: bool,
    pub has_owner: bool,
    pub is_tool_window: bool,
    pub is_app_window: bool,
    pub is_no_activate: bool,
    pub rect: RectI,
    pub title: String,
    pub class_name: String,
    pub process_name: String,
}

/// 判断窗口是否属于系统或 Tools3000 内部不可管理类名
pub fn is_ignored_class(class_name: &str) -> bool {
    if class_name.is_empty() {
        return true;
    }
    if class_name.starts_with("Tools3000_Overlay")
        || class_name.starts_with("Tools3000_WindowKit_")
    {
        return true;
    }
    IGNORED_SYSTEM_CLASSES
        .iter()
        .any(|c| c.eq_ignore_ascii_case(class_name))
}

/// 判断进程名是否在用户排除列表中（忽略大小写及可选 .exe 后缀）
pub fn is_excluded_process(process_name: &str, excluded_apps: &[String]) -> bool {
    if process_name.is_empty() || excluded_apps.is_empty() {
        return false;
    }
    let low = process_name.trim().to_ascii_lowercase();
    let stem = low.strip_suffix(".exe").unwrap_or(&low);
    excluded_apps.iter().any(|entry| {
        let e = entry.trim().to_ascii_lowercase();
        let e_stem = e.strip_suffix(".exe").unwrap_or(&e);
        !e_stem.is_empty() && (low == e || stem == e_stem)
    })
}

/// 综合判定一个顶层窗口是否符合 Alt+Tab 快速切换 / 分区停靠 / 工作区管理资格
pub fn is_eligible_user_window(
    meta: &WindowCandidateMeta,
    include_minimized: bool,
    excluded_apps: &[String],
) -> bool {
    if !meta.is_visible || meta.is_cloaked || meta.is_hung {
        return false;
    }
    if meta.is_minimized && !include_minimized {
        return false;
    }
    if is_ignored_class(&meta.class_name) {
        return false;
    }
    if is_excluded_process(&meta.process_name, excluded_apps) {
        return false;
    }
    // 跳过工具窗口与非 AppWindow 的无激活浮层
    if meta.is_tool_window && !meta.is_app_window {
        return false;
    }
    if meta.is_no_activate && !meta.is_app_window {
        return false;
    }
    // 有父窗口且未显式声明 WS_EX_APPWINDOW 的二级弹窗不列入顶层主窗口
    if meta.has_owner && !meta.is_app_window {
        return false;
    }
    // 无标题的幽灵宿主窗口直接忽略
    if meta.title.trim().is_empty() {
        return false;
    }
    // 非最小化窗口必须具备有效可视尺寸（最小化窗口在 Windows 下常位于 -32000 处）
    if !meta.is_minimized && (meta.rect.width() < 24 || meta.rect.height() < 24) {
        return false;
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_meta() -> WindowCandidateMeta {
        WindowCandidateMeta {
            is_visible: true,
            is_cloaked: false,
            is_minimized: false,
            is_hung: false,
            has_owner: false,
            is_tool_window: false,
            is_app_window: true,
            is_no_activate: false,
            rect: RectI::new(100, 100, 1100, 800),
            title: "Visual Studio Code".to_string(),
            class_name: "Chrome_WidgetWin_1".to_string(),
            process_name: "Code.exe".to_string(),
        }
    }

    #[test]
    fn filters_system_cloaked_tool_hung_and_excluded_windows() {
        let base = sample_meta();
        assert!(is_eligible_user_window(&base, true, &[]));

        let mut cloaked = base.clone();
        cloaked.is_cloaked = true;
        assert!(!is_eligible_user_window(&cloaked, true, &[]));

        let mut tool = base.clone();
        tool.is_tool_window = true;
        tool.is_app_window = false;
        assert!(!is_eligible_user_window(&tool, true, &[]));

        let mut shell = base.clone();
        shell.class_name = "Shell_TrayWnd".to_string();
        assert!(!is_eligible_user_window(&shell, true, &[]));

        let mut self_overlay = base.clone();
        self_overlay.class_name = QUICK_SWITCH_WINDOW_CLASS.to_string();
        assert!(!is_eligible_user_window(&self_overlay, true, &[]));

        let mut hung = base.clone();
        hung.is_hung = true;
        assert!(!is_eligible_user_window(&hung, true, &[]));

        let excluded = vec!["code.exe".to_string()];
        assert!(!is_eligible_user_window(&base, true, &excluded));

        let mut minimized = base.clone();
        minimized.is_minimized = true;
        minimized.rect = RectI::new(-32000, -32000, -31840, -31972);
        assert!(is_eligible_user_window(&minimized, true, &[]));
        assert!(!is_eligible_user_window(&minimized, false, &[]));
    }
}
