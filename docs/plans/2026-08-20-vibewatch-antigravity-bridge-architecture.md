# VibeWatch 与 Google Antigravity 深度双向交互技术方案

> **状态**：方案设计与待评审（Architecture Review & Feasibility Proposal）  
> **核心目标**：彻底解决当前 BLE HID 无法穿透后台窗口、无法主动唤醒 Antigravity、无法结构化交互的根本瓶颈，构建硬件级双向 Agent 交互通道。

---

## 一、 现状痛点与根本瓶颈剖析（Root-Cause Analysis）

```
【当前模式的致命缺陷】
[ VibeWatch 手表 ] ──(BLE HID 普通按键)──► [ macOS 焦点窗口 ]
                                                │
                                                ▼ (若 Antigravity 在后台)
                                     ❌ 按键被前台应用吞噬，无响应
                                     ❌ 无法主动拉起 Antigravity
                                     ❌ 无法传递结构化 Agent 槽位与提示词
                                     ❌ 语音听写只能打字，无法直达 Agent
```

1. **强依赖窗口焦点（Focus Dependency）**：
   - 当前固件仅充当“普通蓝牙键盘”，发送的快捷键（如 `Cmd+1` 或 `Cmd+Option+1`）必须在 Antigravity 处于前台最顶层窗口时才有效。
   - 用户在浏览器、终端或其它应用工作时，按键完全无法送达后台的 Antigravity。
2. **缺乏双向结构化通道（No Bidirectional Agent Protocol）**：
   - 键盘输入只能单向“打字”，无法实现“手表按 1 号键 -> Antigravity 自动切换至架构师 Agent 会话”或“手表按 PTT 语音 -> 语音转文字直达 Antigravity Prompt 提交”。
3. **配额与状态被动滞后**：
   - Antigravity 没有内置向手表推流的底层驱动，导致手表屏幕配额无法实时响应 Agent 每次工具调用的 Token 消耗。

---

## 二、 三种扩展方案对比与架构选型

| 方案类别 | 架构原理 | 优势 | 劣势与限制 | 推荐度 |
| :--- | :--- | :--- | :--- | :---: |
| **方案 1：系统级无感桥接（Native Accessibility Daemon）** | 编写后台常驻守护程序 `vibewatch-bridge`，监听手表 BLE Vendor 报文，通过 macOS Accessibility API / AppleScript 强行激活 Antigravity 窗口并注入事件。 | • 零侵入 Antigravity 源码<br>• 可立即在前台激活任何应用<br>• 支持全局呼出与多窗口控制 | • 依赖 macOS 辅助功能权限<br>• 仍属于 UI 层的宏自动化操作 | ⭐⭐⭐⭐<br>(最快落地) |
| **方案 2：原生 MCP 硬件服务（MCP-over-BLE Hardware Bridge）** | 为 Antigravity 挂载一个专用的 **`vibewatch-mcp` Server**。MCP 服务常驻监听手表 BLE 报文，同时作为 Antigravity 的原生工具/通道。 | • **官方最推荐标准架构**<br>• 真正的双向数据流（毫秒级状态同步）<br>• 支持硬件物理审批拦截与决策闭环<br>• 不受窗口焦点限制 | • 需在 Antigravity MCP 配置中注册该服务 | ⭐⭐⭐⭐⭐<br>(长期最佳) |
| **方案 3：本地 HTTP / IPC Webhook 触发器** | 在本地开启 `localhost:4321`，手表事件转换为 HTTP 请求，直接触发 Antigravity 的 headless 会话或脚本。 | • 解耦性强<br>• 易于测试与抓包调试 | • 需要 Antigravity 支持本地 Webhook 接收端点 | ⭐⭐⭐ |

---

## 三、 推荐融合架构：两阶段渐进式落地

为了兼顾“**立即见效的全局唤醒交互**”与“**长期标准的 MCP 双向 Agent 互联**”，设计采用 **两层融合架构（Hybrid Architecture）**：

```
                    ┌──────────────────────────────────────────────────┐
                    │               VibeWatch 硬件手表                  │
                    │   - 1~6 号 Agent 槽位触控                         │
                    │   - 实体 OK/NG 审批按键                           │
                    │   - PTT 语音按键 (ACT10/11)                      │
                    │   - AMOLED 全彩配额/状态渲染                      │
                    └─────────────────────────┬────────────────────────┘
                                              │ BLE (Vendor Report 6 & GATT)
                                              ▼
                    ┌──────────────────────────────────────────────────┐
                    │          VibeWatch Host Bridge (本地守护进程)     │
                    │   (Python / Swift 极轻量常驻, PID < 20MB)         │
                    └─────────────┬──────────────────────┬─────────────┘
                                  │                      │
                  ┌───────────────┴────────┐    ┌────────┴──────────────┐
                  ▼                        ▼    ▼                       ▼
       【模块 A：全局窗口唤醒与注入】         【模块 B：MCP 双向数据与审批中枢】
       • 捕获 `AG01~06` 槽位点击              • 实时向手表推送 Token/配额
       • 无论 Antigravity 是否在后台，        • 捕获 Agent 的高危操作（Shell/Write）
         一键将其拉至前台并聚焦 Prompt        • 触发手表物理震动与全彩弹窗
       • PTT 语音流直接填入 Prompt            • 手表按下 OK/NG 直接将决策返回给 Agent
```

---

## 四、 核心功能模块详细设计

### 3. 模块 C：多级智能 PTT 语音输入分发引擎（Tiered Voice Input Pipeline）

针对用户对于语音输入“直达目标 App 输入框”与“多级回落”的核心诉求，设计三级自适应路由管道：

```
 [ 手表按下 PTT (ACT10) ]
            │
            ▼
 ┌─────────────────────────────────────────────────────────┐
 │ 1. 焦点准备：无论目标应用在前台还是后台，                     │
 │    Accessibility API 自动将 Antigravity/Workbuddy 置顶， │
 │    并将光标精准锚定在 Prompt 内容输入框 (AXFocusedElement)   │
 └──────────────────────────┬──────────────────────────────┘
                            │
                            ▼
 ┌─────────────────────────────────────────────────────────┐
 │ 2. 三级自适应语音引擎分发 (Tiered Engine Selection)       │
 └──────────────────────────┬──────────────────────────────┘
                            │
       ┌────────────────────┼────────────────────┐
       ▼                    ▼                    ▼
 【Level 1: 豆包语音输入】 【Level 2: Agent 内置录音】 【Level 3: 系统听写/Whisper】
 • 检测到 `Doubao.app`  • 若当前在前台且支持   • 保底触发 macOS 原生听写
   常驻运行               原生语音输入 (WebRTC)   (F19) 或本地快速 Whisper
 • 发送豆包全局听写热键  • 触发内置麦克风录音   • 转写文字瞬时自动键入输入框
 • 文字直接流式打入输入框
```

#### 关键技术实现：
1. **焦点预置机制（Focus Pre-positioning）**：
   在按压 PTT 的瞬间（0~15ms 内），守护进程通过 `NSRunningApplication.activate(options: .activateIgnoringOtherApps)` 确保目标窗口就绪，并定位到主文本输入框，保证语音文字绝对不会打飞到其他无关窗口。
2. **豆包输入法联动**：
   检测系统进程列表中是否存在 `Doubao` / 豆包助手，若存在，通过守护进程精准触发豆包全局语音悬浮栏，松开 PTT 时自动提交转写文字。
3. **无缝回落保障（Zero-Failure Fallback）**：
   若未安装或未开启豆包，自动无感切换为 macOS 系统听写（`F19`）或后台轻量转写，确保任何设备环境下“按住就说，说完即填”的极致体验。

### 2. 模块 B：Antigravity 原生 MCP 硬件中枢（解决“双向通信与状态感知”）
- **实现方式**：
  在 Antigravity 中配置一个专用的 MCP Server：`vibewatch-mcp`。
- **暴露的核心功能与工具**：
  - `push_agent_state(agent_id, color, status, quota)`：
    当 Antigravity 在执行复杂任务（思考、写代码、跑测试）时，实时将各子 Agent 的状态色谱推送到手表外环；
  - `request_hardware_approval(request_id, summary, timeout_ms)`：
    当 Antigravity 准备执行 `run_command` 或高危代码修改时，主动向手表发起 Protocol v2 事务性审批，手表亮起琥珀色警告卡片，等待用户按手表物理 OK/NG 键后返回结果。

---

## 五、 分步实施规划（Implementation Roadmap）

```
Phase 1: 全局唤醒与 PTT 桥接 (Bridge Daemon)
  ├── 任务 1.1: 编写 `scripts/vibewatch_bridge.py` 监听 BLE Vendor Report 6
  ├── 任务 1.2: 实现 macOS 窗口强行置顶与 Prompt 焦点激活
  └── 任务 1.3: 实现 PTT 语音转文字直达输入框

Phase 2: Antigravity MCP Server 开发 (Native Integration)
  ├── 任务 2.1: 构建 `mcp-server-vibewatch`，实现双向 GATT Quota / Agent 状态推流
  ├── 任务 2.2: 实现 Antigravity 事务性硬件审批流（Hardware-in-the-loop）
  └── 任务 2.3: 注册并打通 Antigravity 配置文件

Phase 3: 固件与端侧体验优化
  └── 任务 3.1: 增强手表端针对 Antigravity 的双向心跳与在线反馈
```

---

## 六、 风险评估与应对措施

| 风险项 | 影响分析 | 缓解措施 |
| :--- | :--- | :--- |
| **macOS 辅助功能权限被拒** | 守护进程无法自动将后台窗口拉到前台 | 脚本启动时自动检测权限，并提供一键授权引导（`tccutil` / 系统设置直达）。 |
| **BLE 连接偶尔断开重连** | 手表睡眠唤醒时可能丢掉按键事件 | 守护进程内置自动重连与连接池保活机制，丢包时快速重试。 |
| **MCP 响应延迟** | 审批弹窗响应过慢影响敲代码流畅度 | 硬件端与 MCP 走本地 BLE 通道，实测端到端延时 < 35ms，保证秒级反馈。 |
