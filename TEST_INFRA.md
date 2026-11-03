# E2E Test Infra: Tools3000 Gesture & DirectComposition

## Test Philosophy
- Opaque-box, requirement-driven. Derived strictly from user specifications in ORIGINAL_REQUEST.md.
- Methodology: Category-Partition + Boundary Value Analysis (BVA) + Pairwise Combinatorial + Real-World Workload Stress Testing.

## Feature Inventory
| # | Feature | Source (requirement) | Tier 1 | Tier 2 | Tier 3 |
|---|---------|---------------------|:------:|:------:|:------:|
| 1 | 1000Hz 鼠标输入采样 | ORIGINAL_REQUEST §R1 | 5 | 5 | ✓ |
| 2 | 无锁环形缓冲区分发 | ORIGINAL_REQUEST §R1 | 5 | 5 | ✓ |
| 3 | 0 锁争用与 0 阻塞判定 | ORIGINAL_REQUEST §R1 | 5 | 5 | ✓ |
| 4 | 预分配硬件表面 | ORIGINAL_REQUEST §R2 | 5 | 5 | ✓ |
| 5 | 0 SetWindowPos 窗口重排 | ORIGINAL_REQUEST §R2 | 5 | 5 | ✓ |
| 6 | 0 运行时 CreateSurface | ORIGINAL_REQUEST §R2 | 5 | 5 | ✓ |
| 7 | GPU 增量局部绘制 | ORIGINAL_REQUEST §R2 | 5 | 5 | ✓ |
| 8 | 物理刷新率 VBlank 对齐 | ORIGINAL_REQUEST §R3 | 5 | 5 | ✓ |
| 9 | QPC 高精度单调节拍器 | ORIGINAL_REQUEST §R3 | 5 | 5 | ✓ |
| 10 | 尖端采样相位差 < 4ms | ORIGINAL_REQUEST §R3 | 5 | 5 | ✓ |
| 11 | 单笔画即时高亮与 HUD | ORIGINAL_REQUEST §AC | 5 | 5 | ✓ |
| 12 | 短促手势与淡出鲁棒性 | ORIGINAL_REQUEST §AC | 5 | 5 | ✓ |

## Test Architecture
- Test Runner: PowerShell / C++ test executable (`scripts/test_gesture_e2e.ps1`, `Tools3000Tests.exe`)
- Format: Pass/Fail assertion with exit code 0 and structured error output
- Directory layout: `tests/e2e/`, `scripts/`

## Real-World Application Scenarios (Tier 4)
| # | Scenario | Features Exercised | Complexity |
|---|----------|--------------------|------------|
| 1 | 1000Hz 报告率 + CPU 满载高速连续画圆与折笔 | F1, F2, F3, F7, F8, F9, F10 | High |
| 2 | < 50ms 极速短促单笔画手势 (Back/Forward) | F1, F5, F11, F12 | Medium |
| 3 | 4K 多屏幕跨屏连续大行程手势 | F4, F5, F6, F7 | High |
| 4 | 复杂多笔画连续手势带 HUD 即时状态反馈 | F5, F11, F12 | Medium |
| 5 | 左键中断与前台窗口无响应自愈测试 | F1, F3, F11 | High |

## Coverage Thresholds
- Tier 1: ≥5 per feature (60 tests minimum)
- Tier 2: ≥5 per feature (60 tests minimum)
- Tier 3: Pairwise coverage of major feature interactions (≥12 tests)
- Tier 4: ≥5 realistic application scenarios (5 tests)
- Total minimum: ~137 test assertions across automated scripts and unit test harnesses.
