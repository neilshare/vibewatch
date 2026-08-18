# VibeWatch (随身 AI 编程智能体中枢)

[English](README.md) | **简体中文**

[![Firmware build](https://img.shields.io/badge/PlatformIO-ESP32--S3-orange.svg)](https://platformio.org/)
[![Firmware Version](https://img.shields.io/badge/Firmware-v1.01-green.svg)](docs/release-notes-v1.01.md)
[![Protocol](https://img.shields.io/badge/Companion%20Protocol-v2-blue.svg)](docs/COMPANION_PROTOCOL.md)
[![macOS Companion](https://img.shields.io/badge/macOS-Swift%205.10-blue.svg)](companion/README.md)
[![Tests](https://img.shields.io/badge/Tests-100%25%20Passing-brightgreen.svg)](tests/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

**专为 Vibe Coding 与多智能体协同编程打造的随身触控硬件中枢 — 基于 M5Stack StopWatch (ESP32-S3 AMOLED)。**

---

## 🌟 核心特性 (Key Features)

### 1. 三大桌面 AI 智能体全功能中枢 (Tri-Agent Dashboard)
通过横向滑动手势，在三大顶尖桌面编程智能体之间自由切换，每个智能体享有完全独立的数据模型、品牌主题色与遥测面板：

<table>
  <tr>
    <td width="33%" align="center">
      <img src="docs/images/card-codex.jpg" alt="OpenAI Codex 卡片"><br>
      <strong>🟢 1. OpenAI Codex</strong><br>
      薄荷青主题 · 360° 周度配额环 · 实时重置倒计时
    </td>
    <td width="33%" align="center">
      <img src="docs/images/card-workbuddy.jpg" alt="腾讯 Workbuddy 卡片"><br>
      <strong>🔵 2. 腾讯 Workbuddy</strong><br>
      极光蓝主题 · 专属 Credit 点数余额 · 额度总览
    </td>
    <td width="33%" align="center">
      <img src="docs/images/card-antigravity.jpg" alt="Google Antigravity 卡片"><br>
      <strong>🟣 3. Google Antigravity</strong><br>
      星系紫主题 · 月度配额与上下文监控
    </td>
  </tr>
</table>

- **手势无缝切页**：向左/向右滑动屏幕即可流畅切卡，伴随清脆的 **1150Hz 切页音效** 与 **触觉轻震**。
- **状态持久化 (NVS Memory)**：重启或断电后自动恢复停留在上次选定的智能体卡片。
- **严格任务与事件隔离**：三张卡片的 6 智能体状态环、HID 麦克风按键事件与审批事件各自携带上下文标识（如 `"c": "WORKBUDDY"`），杜绝跨系统串扰。

---

### 2. 事务性人工介入审批与防抢占保护 (Transactional Human-in-the-Loop Approval)

在 AI 智能体执行敏感操作（读取文件、运行终端命令、执行脚本）需要人工授权时，VibeWatch 会瞬间接管：

<p align="center">
  <img src="docs/images/approval-modal.jpg" alt="人工介入审批弹窗" width="400"><br>
  <em>当智能体需要确认时触发的醒目安全弹窗</em>
</p>

- **事务性协议 v2 (Transactional Protocol v2)**：每个审批请求与决策共享严格的 36 字节规范 UUID `request_id`，杜绝请求串号与重复处理。
- **安全信任边界**：固件审批特征值（`.03`）强制校验 BLE 加密连接与已绑定对端（Bonded Peer），确保只有授信的开发主机可推送确认请求。
- **强提醒与自动定向切页**：自动唤醒屏幕、定向切至对应智能体卡片，伴随双重震动警报与 1250Hz 和弦提示音。
- **操作类型与描述展示**：清晰显示 `[ EXEC ]`、`[ SCRIPT ]`、`[ WRITE ]` 及安全的 UTF-8 中文描述文本。
- **物理实体按键盲操**：
  - 👉 **右键 (BtnB) / 屏幕右半区**：一键 **OK (确认/执行)**，触发高音确认阶梯音效。
  - 👈 **左键 (BtnA) / 屏幕左半区**：一键 **NG (拒绝/取消)**，触发低音拒绝阶梯音效。
- **4.0 秒防抢占保护窗 (`Anti-Flapping`)**：用户在做决策期间，后台新消息不会暴力打断或跳页，保证决策连贯性。

---

### 3. 六子任务环形槽位 & 腕上语音控制 (6-Agent Ring & Push-to-Talk)
- **6 环形槽位**：围绕 AMOLED 表盘分布 6 个子任务节点，通过脉冲呼吸灯直观展示各子任务的运行/闲置/完成状态。
- **中心语音触发 (PTT HID Control)**：在任意界面长按中心麦克风区域（或长按右侧物理按键），立即向当前主机智能体发送 Push-to-Talk 拾音控制信号（固件本身不录制/流式传输音频，确保隐私安全）。

---

### 4. DVFS 动态调频与极致能耗优化 (Power Optimization)
- **活跃交互状态**：锁定 **`240MHz`**，确保 60FPS 丝滑动画与亚毫秒级蓝牙响应。
- **60 秒降亮度**：平滑降频至 **`160MHz`**，有效抑制发热。
- **180 秒息屏待机**：深度降频至 **`80MHz`**，**降低约 65% 的待机功耗**，轻触屏幕即刻无感恢复全速。

---

## 🔒 蓝牙伴侣协议 v2 架构 (Protocol v2 Specification)

VibeWatch 固件对外广播私有 GATT 遥测服务及标准 BLE HID 键盘服务：

| 通道角色 | 专属 UUID | 读写权限与安全策略 | 约束限制 |
| :--- | :--- | :--- | :--- |
| **专属服务** | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c01` | 服务发现 | — |
| **配额写入 (.02)** | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c02` | Write with response; 必须加密传输 | 最大 512 字节 UTF-8 JSON |
| **审批请求 (.03)** | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c03` | Write with response; 必须加密 + 强制绑定对端 (Bonded) | 最大 512 字节，包含 canonical `request_id` 与 `ttl_ms` |
| **审批结果 (.04)** | `7f0d4e66-2ac2-4a71-bfbe-4ef61a0e5c04` | Indicate; 必须加密传输 | 返回 `approved` / `rejected` / `expired` 与对应 `request_id` |

> 完整协议规范与错误码定义请参阅 [docs/COMPANION_PROTOCOL.md](docs/COMPANION_PROTOCOL.md)。

---

## 🛠️ 硬件与系统规格 (Hardware Specifications)

| 部件 | 规格 / 角色 |
| :--- | :--- |
| **主控芯片** | [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) (ESP32-S3 双核 240MHz, 16MB Flash, 8MB PSRAM) |
| **显示屏幕** | 1.43 英寸 466 × 466 圆形 AMOLED 电容式触控屏 |
| **交互输入** | 2 × 物理按键 (左 BtnA / 右 BtnB) + 实体振动马达 + 蜂鸣扬声器 |
| **无线通信** | Bluetooth Low Energy 5.0 (BLE HID Keyboard + 专属 GATT 事务通道) |
| **电源系统** | 内置锂电池 + AXP 电源管理 + DVFS 动态频率调节 |
| **主机支持** | macOS / Linux / Windows 智能体工作站 |

---

## 💻 快速上手与烧录 (Getting Started)

### 1. 固件编译与烧录

本项目使用 [PlatformIO](https://platformio.org/) 进行工程管理。将 M5Stack StopWatch 通过 USB-C 数据线连接至电脑：

```bash
# 1. 克隆代码仓库
git clone https://github.com/neilshare/vibewatch.git
cd vibewatch

# 2. 编译并烧录固件至手表
platformio run -e m5stack-stopwatch --target upload
```

---

### 2. 蓝牙配对与设备绑定

1. 点击表盘左下角的 **设置 (Settings)** 图标进入设置菜单。
2. 选择设备槽位（如 Slot 1），点击 **PAIR** 开启安全配对广播。
3. 打开电脑蓝牙设置，配对并连接名为 `Vibe Watch #1` 的蓝牙设备。
4. 记录当前主机的 CoreBluetooth UUID（在非 Demo 模式下通过 `--device-id` 严格绑定，保障通信安全）。

---

### 3. 主机端遥测与伴侣工具 (Companion & CLI)

#### 原生 macOS Swift 伴侣工具 (推荐)
已为 macOS 构建了基于原生 CoreBluetooth 的伴侣工具，即使已作为 HID 键盘连接也能实现即时 GATT 写入与审批监听：

```bash
# 编译 Swift 伴侣工具
swift build --package-path companion -c release

# 合成 Demo 模式：自动搜索周边手表，写入测试配额并输出当前设备 UUID
./companion/.build/release/codex-watch-companion --demo --verbose

# 向指定已绑定手表同步真实 Codex 配额
./companion/.build/release/codex-watch-companion --auto --card codex \
  --device-id YOUR_COREBLUETOOTH_UUID

# 向指定手表同步 Workbuddy 专属 Credit 点数
./companion/.build/release/codex-watch-companion \
  --remaining 83.3 --reset 0 --card workbuddy \
  --credits 1250 --total-credits 1500 \
  --device-id YOUR_COREBLUETOOTH_UUID

# 发送事务性协议 v2 人工审批请求
./companion/.build/release/codex-watch-companion --approval \
  --card workbuddy --type SCRIPT --summary "执行自动化测试脚本" \
  --device-id YOUR_COREBLUETOOTH_UUID
```

#### Python CLI 工具
```bash
# 安装 Python 依赖
pip install bleak pytest

# 自动同步真实本地 Codex 额度
python3 scripts/sync_watch.py --auto --device-id YOUR_COREBLUETOOTH_UUID

# 使用 Bleak 后端发送审批请求
python3 scripts/sync_watch.py --backend bleak --approval \
  --summary "执行自动化测试脚本" \
  --device-id YOUR_COREBLUETOOTH_UUID
```

---

## 🧪 自动化测试体系 (Testing & Quality Assurance)

项目拥有全栈自动化测试套件，涵盖固件、主机驱动与协议验证：

```bash
# 1. 固件 Native C++ 核心逻辑测试 (27 项全部通过)
platformio test -e native

# 2. Swift 伴侣核心独立行为验证 (37 项行为用例全部通过)
swift run --package-path companion VibeWatchCompanionCoreVerification

# 3. Python / Bleak 协议与 CLI 测试 (42 项用例 100% 覆盖通过)
pytest tests/
```

---

## 📚 延伸文档 (Documentation)

- [权威伴侣协议 v2 规范 (Protocol v2 Specification)](docs/COMPANION_PROTOCOL.md)
- [硬件与协议验收标准 (Hardware Acceptance v2)](docs/hardware-acceptance-v2.md)
- [Swift 伴侣工具指南 (Companion Guide)](companion/README.md)
- [版本发布说明 (Release Notes v1.01)](docs/release-notes-v1.01.md)
- [AI 代码审查报告 (Code Review Report)](docs/code_review_report.md)

---

## 📜 许可证 (License)

本项目采用 [MIT License](LICENSE) 开源许可证。
