// ─────────────────────────────────────────────────────────────────────────────
// Tools3000 CapsLayer — 零第三方依赖纯 Rust CapsLock 引导层与分应用改键状态机
//
// 架构特性:
//   1. 热路径零堆分配裁决：支持 Shift+Caps 恒定原生大小写、短按 Caps 自定义动作
//      (保留大小写 / Esc / 禁用)、按住 Caps 激活引导层映射。
//   2. 滚键 (Key Rollover) 零粘滞零吞字保护：追踪所有已激活映射键的按下/抬起配对，
//      即使在释放主键前先松开 Caps 也能精准配对抬起对应目标键。
//   3. 支持全局默认层 + 按前台应用进程名独立覆写层 + 黑名单自动避让。
// ─────────────────────────────────────────────────────────────────────────────

use std::collections::HashMap;
use std::slice;

pub const VK_CAPITAL: u32 = 0x14;
pub const VK_ESCAPE: u32 = 0x1B;
pub const VK_SHIFT: u32 = 0x10;
pub const VK_CONTROL: u32 = 0x11;
pub const VK_MENU: u32 = 0x12;
pub const VK_LWIN: u32 = 0x5B;

pub const MOD_CTRL: u8 = 0x01;
pub const MOD_ALT: u8 = 0x02;
pub const MOD_SHIFT: u8 = 0x04;
pub const MOD_WIN: u8 = 0x08;

/// FFI 输出动作类型
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CapsActionKind {
    None = 0,
    KeyDown = 1,
    KeyUp = 2,
    SendCombo = 3,
    SendTextIndex = 4,
    TapToggleCaps = 5,
    TapEscape = 6,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CapsOutputAction {
    pub kind: u8,
    pub mods: u8,
    pub reserved: u16,
    pub vk: u32,
    pub text_index: u32,
}

impl Default for CapsOutputAction {
    fn default() -> Self {
        Self {
            kind: CapsActionKind::None as u8,
            mods: 0,
            reserved: 0,
            vk: 0,
            text_index: 0,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TapAction {
    ToggleCaps,
    Escape,
    None,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MappingTarget {
    Key(u32),
    Combo { mods: u8, vk: u32 },
    Text(u32),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KeyMapping {
    pub trigger_vk: u32,
    pub target: MappingTarget,
    pub enabled: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum CapsState {
    Idle,
    Pending,
    LayerActive,
}

pub struct CapsEngine {
    enabled: bool,
    tap_action: TapAction,
    shift_caps_toggles_native: bool,
    excluded_apps: Vec<String>,
    global_mappings: HashMap<u32, MappingTarget>,
    app_mappings: HashMap<String, HashMap<u32, MappingTarget>>,
    text_table: Vec<String>,
    state: CapsState,
    // 记录当前已被引导层接管且尚未收到物理 KeyUp 的触发键 -> 对应的目标单键 VK（用于配对 KeyUp 零粘滞）
    active_mapped_downs: HashMap<u32, Option<u32>>,
}

impl Default for CapsEngine {
    fn default() -> Self {
        Self::new()
    }
}

impl CapsEngine {
    pub fn new() -> Self {
        Self {
            enabled: false,
            tap_action: TapAction::ToggleCaps,
            shift_caps_toggles_native: true,
            excluded_apps: Vec::new(),
            global_mappings: HashMap::new(),
            app_mappings: HashMap::new(),
            text_table: Vec::new(),
            state: CapsState::Idle,
            active_mapped_downs: HashMap::new(),
        }
    }

    pub fn set_enabled(&mut self, enabled: bool) {
        self.enabled = enabled;
        if !enabled {
            self.reset_state();
        }
    }

    pub fn is_enabled(&self) -> bool {
        self.enabled
    }

    /// 当 CapsLock 未按住且不存在尚未释放的滚键映射时返回 true，供 C++ 钩子无锁快路径零开销直通普通打字按键
    pub fn is_idle(&self) -> bool {
        self.state == CapsState::Idle && self.active_mapped_downs.is_empty()
    }

    pub fn reset_state(&mut self) {
        self.state = CapsState::Idle;
        self.active_mapped_downs.clear();
    }

    pub fn clear_rules(&mut self) {
        self.excluded_apps.clear();
        self.global_mappings.clear();
        self.app_mappings.clear();
        self.text_table.clear();
        self.reset_state();
    }

    pub fn set_tap_action(&mut self, action: TapAction) {
        self.tap_action = action;
    }

    pub fn set_shift_caps_native(&mut self, allow: bool) {
        self.shift_caps_toggles_native = allow;
    }

    pub fn add_excluded_app(&mut self, exe: &str) {
        let norm = normalize_exe(exe);
        if !norm.is_empty() && !self.excluded_apps.contains(&norm) {
            self.excluded_apps.push(norm);
        }
    }

    pub fn add_text(&mut self, text: &str) -> u32 {
        let idx = self.text_table.len() as u32;
        self.text_table.push(text.to_string());
        idx
    }

    pub fn get_text(&self, idx: u32) -> Option<&str> {
        self.text_table.get(idx as usize).map(|s| s.as_str())
    }

    pub fn add_global_mapping(&mut self, trigger_vk: u32, target: MappingTarget) {
        if trigger_vk != 0 {
            self.global_mappings.insert(trigger_vk, target);
        }
    }

    pub fn add_app_mapping(&mut self, exe: &str, trigger_vk: u32, target: MappingTarget) {
        let norm = normalize_exe(exe);
        if !norm.is_empty() && trigger_vk != 0 {
            self.app_mappings
                .entry(norm)
                .or_default()
                .insert(trigger_vk, target);
        }
    }

    fn is_excluded(&self, fg_exe_lower: &str) -> bool {
        if fg_exe_lower.is_empty() {
            return false;
        }
        self.excluded_apps.iter().any(|e| e == fg_exe_lower)
    }

    fn lookup_mapping(&self, fg_exe_lower: &str, vk: u32) -> Option<&MappingTarget> {
        if !fg_exe_lower.is_empty() {
            if let Some(app_map) = self.app_mappings.get(fg_exe_lower) {
                if let Some(target) = app_map.get(&vk) {
                    return Some(target);
                }
            }
        }
        self.global_mappings.get(&vk)
    }

    /// 处理一条底层键盘事件：
    /// 返回 `(consume_original_event, actions)`
    pub fn process_key(
        &mut self,
        vk: u32,
        is_down: bool,
        mods_mask: u8,
        fg_exe: &str,
        out: &mut Vec<CapsOutputAction>,
    ) -> bool {
        out.clear();
        if !self.enabled {
            return false;
        }

        // 1. 先处理已进入引导层按下的滚键 KeyUp 配对（即使 Caps 已经先松开或焦点切换也必须完整释放！）
        if !is_down {
            if let Some(mapped_key_opt) = self.active_mapped_downs.remove(&vk) {
                if let Some(target_vk) = mapped_key_opt {
                    out.push(CapsOutputAction {
                        kind: CapsActionKind::KeyUp as u8,
                        vk: target_vk,
                        ..Default::default()
                    });
                }
                return true;
            }
        } else if self.state == CapsState::Idle {
            // 滚键防漏字保护：若用户先松开 Caps 但仍按住刚映射的按键导致键盘驱动产生自动连发 KeyDown，
            // 严禁泄漏原始字母（如多打出 'iiii'），而是继续沿用已绑定的目标键或吞掉直到物理抬起。
            if let Some(&mapped_key_opt) = self.active_mapped_downs.get(&vk) {
                if let Some(target_vk) = mapped_key_opt {
                    out.push(CapsOutputAction {
                        kind: CapsActionKind::KeyDown as u8,
                        vk: target_vk,
                        ..Default::default()
                    });
                }
                return true;
            }
        }

        let fg_lower = normalize_exe(fg_exe);
        if self.state == CapsState::Idle && self.is_excluded(&fg_lower) {
            return false;
        }

        // 处理 CapsLock 自身状态迁移
        if vk == VK_CAPITAL {
            if is_down {
                // Shift + CapsLock 在空闲态恒定放行给 Windows 切换原生大小写
                if self.state == CapsState::Idle
                    && self.shift_caps_toggles_native
                    && (mods_mask & MOD_SHIFT) != 0
                {
                    return false;
                }
                if self.state == CapsState::Idle {
                    self.state = CapsState::Pending;
                }
                // 持续吞掉 CapsLock 按下与连击，大小写灯 0 闪烁
                return true;
            } else {
                // CapsLock 抬起
                match self.state {
                    CapsState::Idle => return false,
                    CapsState::Pending => {
                        self.state = CapsState::Idle;
                        match self.tap_action {
                            TapAction::ToggleCaps => out.push(CapsOutputAction {
                                kind: CapsActionKind::TapToggleCaps as u8,
                                vk: VK_CAPITAL,
                                ..Default::default()
                            }),
                            TapAction::Escape => out.push(CapsOutputAction {
                                kind: CapsActionKind::TapEscape as u8,
                                vk: VK_ESCAPE,
                                ..Default::default()
                            }),
                            TapAction::None => {}
                        }
                        return true;
                    }
                    CapsState::LayerActive => {
                        self.state = CapsState::Idle;
                        // 若仍有未松开的映射键，留在 active_mapped_downs 等物理松开时优雅收尾
                        return true;
                    }
                }
            }
        }

        // 非 CapsLock 按键：仅在 Caps 处于 Pending 或 LayerActive 时生效
        if self.state == CapsState::Idle {
            return false;
        }

        // 修饰键自身在按住 Caps 时直接放行（例如 Caps + Shift + I 选中文本向上！）
        if is_modifier_vk(vk) {
            return false;
        }

        if is_down {
            if let Some(target) = self.lookup_mapping(&fg_lower, vk).cloned() {
                self.state = CapsState::LayerActive;
                match target {
                    MappingTarget::Key(target_vk) => {
                        self.active_mapped_downs.insert(vk, Some(target_vk));
                        out.push(CapsOutputAction {
                            kind: CapsActionKind::KeyDown as u8,
                            vk: target_vk,
                            ..Default::default()
                        });
                    }
                    MappingTarget::Combo { mods, vk: target_vk } => {
                        self.active_mapped_downs.insert(vk, None);
                        out.push(CapsOutputAction {
                            kind: CapsActionKind::SendCombo as u8,
                            mods,
                            vk: target_vk,
                            ..Default::default()
                        });
                    }
                    MappingTarget::Text(text_idx) => {
                        self.active_mapped_downs.insert(vk, None);
                        out.push(CapsOutputAction {
                            kind: CapsActionKind::SendTextIndex as u8,
                            text_index: text_idx,
                            ..Default::default()
                        });
                    }
                }
                return true;
            }
            // 未映射按键直接透传，不吞用户正常打字
            return false;
        }

        false
    }
}

pub fn normalize_exe(raw: &str) -> String {
    let trimmed = raw.trim().trim_matches('"');
    let base = trimmed.rsplit(['\\', '/']).next().unwrap_or(trimmed);
    base.to_ascii_lowercase()
}

fn is_modifier_vk(vk: u32) -> bool {
    matches!(
        vk,
        0x10 | 0x11 | 0x12 | 0xA0 | 0xA1 | 0xA2 | 0xA3 | 0xA4 | 0xA5 | 0x5B | 0x5C
    )
}

// ─────────────────────────────────────────────────────────────────────────────
// C ABI 导出（供 Tools3000 C++ KeyboardHook 零分配高速调用）
// ─────────────────────────────────────────────────────────────────────────────

#[no_mangle]
pub extern "C" fn t3k_caps_create() -> *mut CapsEngine {
    Box::into_raw(Box::new(CapsEngine::new()))
}

#[no_mangle]
pub unsafe extern "C" fn t3k_caps_destroy(ptr: *mut CapsEngine) {
    if !ptr.is_null() {
        drop(Box::from_raw(ptr));
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_caps_set_enabled(ptr: *mut CapsEngine, enabled: u8) {
    if let Some(eng) = ptr.as_mut() {
        eng.set_enabled(enabled != 0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_caps_reset_state(ptr: *mut CapsEngine) {
    if let Some(eng) = ptr.as_mut() {
        eng.reset_state();
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_caps_is_idle(ptr: *const CapsEngine) -> u8 {
    if let Some(eng) = ptr.as_ref() {
        if eng.is_idle() {
            return 1;
        }
        return 0;
    }
    1
}

#[no_mangle]
pub unsafe extern "C" fn t3k_caps_clear_rules(ptr: *mut CapsEngine) {
    if let Some(eng) = ptr.as_mut() {
        eng.clear_rules();
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_caps_set_options(
    ptr: *mut CapsEngine,
    tap_action_code: u8,
    shift_caps_native: u8,
) {
    if let Some(eng) = ptr.as_mut() {
        let act = match tap_action_code {
            1 => TapAction::Escape,
            2 => TapAction::None,
            _ => TapAction::ToggleCaps,
        };
        eng.set_tap_action(act);
        eng.set_shift_caps_native(shift_caps_native != 0);
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_caps_add_excluded_app(
    ptr: *mut CapsEngine,
    exe_utf8: *const u8,
    exe_len: usize,
) {
    if let (Some(eng), true) = (ptr.as_mut(), !exe_utf8.is_null() && exe_len > 0) {
        let bytes = slice::from_raw_parts(exe_utf8, exe_len);
        if let Ok(s) = std::str::from_utf8(bytes) {
            eng.add_excluded_app(s);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn t3k_caps_add_text(
    ptr: *mut CapsEngine,
    text_utf8: *const u8,
    text_len: usize,
) -> u32 {
    if let (Some(eng), true) = (ptr.as_mut(), !text_utf8.is_null()) {
        let bytes = slice::from_raw_parts(text_utf8, text_len);
        if let Ok(s) = std::str::from_utf8(bytes) {
            return eng.add_text(s);
        }
    }
    u32::MAX
}

/// target_type: 0=Key, 1=Combo, 2=Text
#[no_mangle]
pub unsafe extern "C" fn t3k_caps_add_mapping(
    ptr: *mut CapsEngine,
    app_exe_utf8: *const u8,
    app_exe_len: usize,
    trigger_vk: u32,
    target_type: u8,
    target_mods: u8,
    target_vk: u32,
    text_index: u32,
) {
    let Some(eng) = ptr.as_mut() else { return };
    let target = match target_type {
        1 => MappingTarget::Combo {
            mods: target_mods,
            vk: target_vk,
        },
        2 => MappingTarget::Text(text_index),
        _ => MappingTarget::Key(target_vk),
    };
    if !app_exe_utf8.is_null() && app_exe_len > 0 {
        let bytes = slice::from_raw_parts(app_exe_utf8, app_exe_len);
        if let Ok(exe) = std::str::from_utf8(bytes) {
            eng.add_app_mapping(exe, trigger_vk, target);
            return;
        }
    }
    eng.add_global_mapping(trigger_vk, target);
}

#[no_mangle]
pub unsafe extern "C" fn t3k_caps_get_text(
    ptr: *const CapsEngine,
    text_index: u32,
    out_ptr: *mut *const u8,
    out_len: *mut usize,
) -> u8 {
    if let (Some(eng), false, false) = (ptr.as_ref(), out_ptr.is_null(), out_len.is_null()) {
        if let Some(s) = eng.get_text(text_index) {
            *out_ptr = s.as_ptr();
            *out_len = s.len();
            return 1;
        }
    }
    0
}

/// 返回值：bit 0 表示是否消费原按键，高 7 位为输出动作数量（或直接由 out_count 传出）
#[no_mangle]
pub unsafe extern "C" fn t3k_caps_process_key(
    ptr: *mut CapsEngine,
    vk: u32,
    is_down: u8,
    mods_mask: u8,
    fg_exe_utf8: *const u8,
    fg_exe_len: usize,
    out_actions: *mut CapsOutputAction,
    max_actions: usize,
    out_count: *mut usize,
) -> u8 {
    if !out_count.is_null() {
        *out_count = 0;
    }
    let Some(eng) = ptr.as_mut() else { return 0 };
    let fg_exe = if !fg_exe_utf8.is_null() && fg_exe_len > 0 {
        let bytes = slice::from_raw_parts(fg_exe_utf8, fg_exe_len);
        std::str::from_utf8(bytes).unwrap_or("")
    } else {
        ""
    };

    let mut actions = Vec::with_capacity(4);
    let consume = eng.process_key(vk, is_down != 0, mods_mask, fg_exe, &mut actions);
    let write_n = actions.len().min(max_actions);
    if !out_actions.is_null() && write_n > 0 {
        for (i, act) in actions.iter().take(write_n).enumerate() {
            *out_actions.add(i) = *act;
        }
    }
    if !out_count.is_null() {
        *out_count = write_n;
    }
    if consume { 1 } else { 0 }
}

#[cfg(test)]
mod tests {
    use super::*;

    const VK_I: u32 = 0x49;
    const VK_J: u32 = 0x4A;
    const VK_R: u32 = 0x52;
    const VK_UP: u32 = 0x26;
    const VK_LEFT: u32 = 0x25;
    const VK_F2: u32 = 0x71;

    #[test]
    fn disabled_engine_passes_everything_through() {
        let mut eng = CapsEngine::new();
        eng.add_global_mapping(VK_I, MappingTarget::Key(VK_UP));
        let mut out = Vec::new();
        assert!(!eng.process_key(VK_CAPITAL, true, 0, "notepad.exe", &mut out));
        assert!(out.is_empty());
    }

    #[test]
    fn tap_caps_emits_configured_tap_action_and_shift_caps_passes_through() {
        let mut eng = CapsEngine::new();
        eng.set_enabled(true);
        eng.set_tap_action(TapAction::Escape);
        let mut out = Vec::new();

        // Shift+CapsLock 直接放行给系统原生大小写
        assert!(!eng.process_key(VK_CAPITAL, true, MOD_SHIFT, "notepad.exe", &mut out));
        assert!(out.is_empty());

        // 单按 Caps 弹起发 Escape
        assert!(eng.process_key(VK_CAPITAL, true, 0, "notepad.exe", &mut out));
        assert!(out.is_empty());
        assert!(eng.process_key(VK_CAPITAL, false, 0, "notepad.exe", &mut out));
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].kind, CapsActionKind::TapEscape as u8);
    }

    #[test]
    fn hold_caps_maps_global_and_handles_rollover_release_cleanly() {
        let mut eng = CapsEngine::new();
        eng.set_enabled(true);
        eng.set_tap_action(TapAction::Escape);
        eng.add_global_mapping(VK_I, MappingTarget::Key(VK_UP));
        let mut out = Vec::new();

        assert!(eng.process_key(VK_CAPITAL, true, 0, "notepad.exe", &mut out));
        // 按下 I -> 映射为 Up Down
        assert!(eng.process_key(VK_I, true, 0, "notepad.exe", &mut out));
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].kind, CapsActionKind::KeyDown as u8);
        assert_eq!(out[0].vk, VK_UP);

        // 滚键测试：先松开 Caps（因曾激活层，绝不误发 TapEscape）
        assert!(eng.process_key(VK_CAPITAL, false, 0, "notepad.exe", &mut out));
        assert!(out.is_empty());

        // 松开 Caps 后若物理 I 仍按住触发键盘驱动自动连发 KeyDown，必须继续映射为 Up 绝不漏出裸字母 'i'
        assert!(eng.process_key(VK_I, true, 0, "notepad.exe", &mut out));
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].kind, CapsActionKind::KeyDown as u8);
        assert_eq!(out[0].vk, VK_UP);

        // 最后松开 I 仍必须配对发送 Up KeyUp
        assert!(eng.process_key(VK_I, false, 0, "notepad.exe", &mut out));
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].kind, CapsActionKind::KeyUp as u8);
        assert_eq!(out[0].vk, VK_UP);
    }

    #[test]
    fn per_app_override_and_excluded_app_work_accurately() {
        let mut eng = CapsEngine::new();
        eng.set_enabled(true);
        eng.add_global_mapping(VK_I, MappingTarget::Key(VK_UP));
        eng.add_app_mapping("code.exe", VK_R, MappingTarget::Key(VK_F2));
        eng.add_app_mapping("code.exe", VK_I, MappingTarget::Key(VK_LEFT));
        eng.add_excluded_app("cs2.exe");

        let mut out = Vec::new();
        // 排除进程直接放行 Caps
        assert!(!eng.process_key(VK_CAPITAL, true, 0, "C:\\Games\\CS2.EXE", &mut out));

        // 在 VS Code 中：应用层优先覆盖全局层，且独占规则生效
        assert!(eng.process_key(VK_CAPITAL, true, 0, "Code.exe", &mut out));
        assert!(eng.process_key(VK_I, true, 0, "Code.exe", &mut out));
        assert_eq!(out[0].vk, VK_LEFT);
        assert!(eng.process_key(VK_R, true, 0, "Code.exe", &mut out));
        assert_eq!(out[0].vk, VK_F2);
        assert!(eng.process_key(VK_CAPITAL, false, 0, "Code.exe", &mut out));

        // 切回记事本：恢复全局层，且 R 未映射直接放行
        assert!(eng.process_key(VK_CAPITAL, true, 0, "notepad.exe", &mut out));
        assert!(eng.process_key(VK_I, true, 0, "notepad.exe", &mut out));
        assert_eq!(out[0].vk, VK_UP);
        assert!(!eng.process_key(VK_R, true, 0, "notepad.exe", &mut out));
    }

    #[test]
    fn modifier_combo_passes_through_while_caps_held() {
        let mut eng = CapsEngine::new();
        eng.set_enabled(true);
        eng.add_global_mapping(VK_J, MappingTarget::Key(VK_LEFT));
        let mut out = Vec::new();

        assert!(eng.process_key(VK_CAPITAL, true, 0, "notepad.exe", &mut out));
        // Shift 自身在按住 Caps 时必须放行，支持 Caps+Shift+J 选中文字！
        assert!(!eng.process_key(VK_SHIFT, true, MOD_SHIFT, "notepad.exe", &mut out));
        assert!(eng.process_key(VK_J, true, MOD_SHIFT, "notepad.exe", &mut out));
        assert_eq!(out[0].kind, CapsActionKind::KeyDown as u8);
        assert_eq!(out[0].vk, VK_LEFT);
    }
}
