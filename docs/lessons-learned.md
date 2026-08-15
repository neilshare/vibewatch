# VibeWatch 开发与烧录经验教训 (Lessons Learned)

本文档总结了在开发、本地化改造以及固件烧录到 **M5Stack StopWatch**（ESP32-S3）过程中遇到的典型问题、排查思路与最佳实践。

---

## 1. 固件烧录后黑屏、绿灯闪烁且无法启动

### 现象
- 烧录固件后，StopWatch 屏幕无任何显示（黑屏），伴随设备指示灯（通常为绿色电源/状态灯）持续闪烁，系统无法进入正常工作状态。

### 根本原因
- **分区表与 Bootloader 缺失**：
  M5Stack StopWatch 搭载 **ESP32-S3** 芯片，配置为 **16MB Flash** 与 **8MB OPI PSRAM**（`qio_opi` 内存模式）。
  若仅将编译生成的单一应用程序 `firmware.bin` 写入 `0x10000`（或使用第三方工具烧录时遗漏了引导区），ESP32-S3 在硬件上电阶段无法找到匹配的二级引导程序（Bootloader）和 16MB 分区表定义（`default_16MB.csv`），导致 ROM Bootloader 报错并不停重启循环。

### 解决方案
必须完整烧录 ESP32-S3 运行所需的全部关键分区：
1. `0x00000000`：二级引导程序 `bootloader.bin`
2. `0x00008000`：16MB 分区表 `partitions.bin`
3. `0x0000e000`：OTA 数据区 `boot_app0.bin`
4. `0x00010000`：应用程序固件 `firmware.bin`

推荐直接使用 PlatformIO 一键烧录（会自动按正确偏移量完整刷入所有引导镜像）：
```bash
python3 -m platformio run -e m5stack-stopwatch --target upload
```

---

## 2. M5Unified / LovyanGFX 中英双语与 CJK 字体适配

### 现象与要点
- M5Stack StopWatch 采用 466×466 圆形显示屏，原生英文界面使用的是 `fonts::Orbitron_Light_24/32` 和 `fonts::DejaVu18` 等 GFX/TrueType 矢量字体。这些字体不包含中文 CJK 字形。

### 经验与解决方案
1. **中文字体选择**：
   - M5GFX / LovyanGFX 底层内置了 `lgfx::U8g2font` 格式的中文点阵字库：`&fonts::efontCN_16` 和 `&fonts::efontCN_24`。
   - 必须通过 `M5.Display.setFont(&fonts::efontCN_16)` 等方式加载中文字体，才能正确解析与绘制 UTF-8 编码的中文文本。
2. **多语言排版与字号差异**：
   - 英文单词（如 `BLUETOOTH DEVICE`）长度较长，适合使用较小字号或紧凑字体；中文词汇（如 `蓝牙设备槽位`）字符数较少但需保证清晰度。
   - 在双语切换渲染时，需要为每种语言分别设定最协调的 `setFont` 与 `setTextSize` 比例。
3. **设置项持久化（NVS / Preferences）**：
   - 语言偏好设置（`LANG_ZH` / `LANG_EN`）应通过 ESP32 NVS（`Preferences` 库）保存在 Flash 中，确保设备断电、低电量关机或重启后保持用户所选的语言。

---

## 3. macOS 下 USB-C 串口识别与连接避坑

1. **线缆选择**：
   - 必须使用具备**数据传输能力**的 USB-C 数据线，部分仅供充电的线缆无法在 Mac 上枚举出 `/dev/cu.usbmodem*` 串口设备。
2. **CDC 串口模式**：
   - ESP32-S3 原生 USB 模式下，设备在 Mac 上通常识别为 `/dev/cu.usbmodem1101`（或类似编号）。
   - 在固件崩溃或处于特定下载模式时，如果串口断开重连，PlatformIO 会自动重新探测可用端口。
