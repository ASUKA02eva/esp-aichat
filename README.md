# ESP-AI 智能语音助手（复刻小智 AI）

把原来 `examples/` 下 **5 个互相独立的示例**合并成的**一个完整固件**：

> 唤醒词检测 → 本地命令词（LED / 舵机）→ 大模型多轮对话，全部串在一条状态机里。

原项目参考：https://github.com/78/xiaozhi-esp32

## 📁 目录结构

```text
esp-aichat/
├── CMakeLists.txt              # 顶层工程（唯一的构建入口）
├── partitions.csv              # 分区表：app 3MB + 语音模型 6MB
├── sdkconfig.defaults          # 通用默认配置
├── sdkconfig.defaults.esp32s3  # ESP32-S3 专用默认配置
├── main/                       # 应用层
│   ├── main.cc                 #   统一状态机（唤醒 / 听写 / 等回复）
│   ├── voice_engine.{h,cc}     #   唤醒词 + 命令词 + 降噪（封装 ESP-SR）
│   ├── app_config.h            #   引脚与参数
│   └── Kconfig.projbuild       #   menuconfig 配置项（WiFi / 服务端地址 / 引脚）
├── components/                 # 可复用组件
│   ├── bsp_board/              #   INMP441 麦克风 + MAX98357A 功放（I2S）
│   ├── audio_manager/          #   录音缓冲 / 播放 / 流式播放
│   ├── wifi_manager/           #   WiFi 连接
│   ├── ws_client/              #   WebSocket 客户端
│   ├── led_control/            #   外接 LED
│   ├── button_control/         #   外接按键（按键唤醒）
│   ├── servo_control/          #   SG90 舵机（LEDC PWM）
│   └── voice_assets/           #   提示音头文件（由 assets/ 里的 mp3 生成）
├── assets/mock_voices/         # 提示音源文件（mp3）
├── server/                     # 电脑端 Python 大模型服务端
├── tools/                      # 音频转头文件工具、命令词拼音工具
└── docs/                       # 架构说明、舵机 PWM 波形图
```

## 🔧 硬件清单

| 部件   | 推荐型号           | 备注                       |
| ------ | ------------------ | -------------------------- |
| 开发板 | ESP32-S3-DevKitC-1 | 必须 S3 版本，需 16MB Flash + PSRAM |
| 麦克风 | INMP441            | 约 5 元/个                 |
| 功放   | MAX98357A          | 约 8 元/个                 |
| 喇叭   | 4Ω 3W 小喇叭       | 约 3 元/个                 |
| LED 灯 | 任意颜色           | 建议串 220Ω 限流电阻        |
| 按键   | 轻触按键           | 可选，用于按键唤醒          |
| 舵机   | SG90 180 度        | 可选，做开关灯的物理反馈    |

### 接线

```text
麦克风(INMP441) → ESP32-S3
---------------------------
VDD → 3.3V      GND → GND
SD  → GPIO6     WS  → GPIO4     SCK → GPIO5

功放(MAX98357A) → ESP32-S3
---------------------------
VIN → 3.3V      GND → GND
DIN → GPIO7     BCLK → GPIO15   LRC → GPIO16

LED      : 长脚 → GPIO21, 短脚 → GND（串限流电阻）
按键     : 一端 → 3V3,  另一端 → GPIO41（按下为高电平）
SG90 舵机: 红线 → 5V,   棕线 → GND, 橙线 → GPIO18
喇叭     : 正极 → 功放 +, 负极 → 功放 -
```

> 所有引脚都能在 `idf.py menuconfig` → **ESP-AI 智能语音助手配置** 里改。

## 🚀 快速开始

### 1. 编译烧录（ESP-IDF 5.4 及以上）

```bash
idf.py set-target esp32s3
idf.py menuconfig        # 见下方“必须配置的两项”
idf.py build
idf.py -p COMx flash monitor
```

**必须配置的两项**（menuconfig → ESP-AI 智能语音助手配置）：

| 配置项 | 说明 |
| --- | --- |
| `WiFi 名称 (SSID)` / `WiFi 密码` | 留空 = 纯离线模式（只用唤醒词 + 命令词） |
| `大模型服务端地址 (WebSocket)` | 例如 `ws://192.168.1.100:8888`（填电脑的局域网 IP） |

唤醒词默认是「你好小智」，由 `sdkconfig.defaults` 指定；想换唤醒词请到
`menuconfig` → `ESP Speech Recognition` → `Load Multiple Wake Words`。

看到串口打印 `✅ 系统就绪，请说“你好小智”或按按键开始对话` 就成功了。

### 2. 启动大模型服务端（想聊天才需要）

```bash
cd server
pip install -r requirements.txt
# 配置阿里云百炼（DashScope）API Key
set DASHSCOPE_API_KEY=sk-xxxxxxxx        # Windows CMD
# $env:DASHSCOPE_API_KEY="sk-xxxxxxxx"   # PowerShell
python server.py
```

启动后终端会打印 `ws://<电脑IP>:8888`，把这个地址填进上面 menuconfig 的
「大模型服务端地址」，重新烧录即可。

## 🎯 怎么用

| 你说的话 | 会发生什么 |
| --- | --- |
| 「你好小智」**或按一下按键** | 唤醒：LED 闪一下 + 播放欢迎音，进入听写 |
| 「帮我开灯」 | LED 点亮 + 舵机点头（转出再复位）+ 播放开灯确认音 |
| 「帮我关灯」 | LED 熄灭 + 舵机反向点头 + 播放关灯确认音 |
| 「现在安全屋情况如何」 | 播放自定义音频（演示自定义命令词） |
| 「拜拜」 | 播放再见音，回到待唤醒 |
| **其他任何话** | 通过 WebSocket 交给大模型，多轮对话（不用再说唤醒词） |
| 连续 10 秒不说话 | 自动播放再见音并退出对话 |

- **本地命令词零延迟**：开灯/关灯/拜拜在设备端识别，不走网络。
- **连续对话**：AI 回复播完后会自动继续听，可以接着提问。
- **离线可用**：不填 WiFi 也能跑，只是没有大模型对话能力。

## ⚙️ 配置项一览

`idf.py menuconfig` → **ESP-AI 智能语音助手配置**：

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `APP_LLM_ENABLE` | y | 是否启用大模型对话（关闭 = 纯离线） |
| `APP_WIFI_SSID` / `APP_WIFI_PASSWORD` | 空 | WiFi 账号密码（留空 = 离线模式） |
| `APP_WS_URI` | `ws://192.168.1.100:8888` | 服务端地址 |
| `APP_LED_GPIO` | 21 | LED 引脚 |
| `APP_ENABLE_BUTTON` / `APP_BUTTON_GPIO` | y / 41 | 按键唤醒 |
| `APP_ENABLE_SERVO` / `APP_SERVO_GPIO` | y / 18 | 舵机 |
| `APP_IDLE_TIMEOUT_MS` | 10000 | 等说话 / 等命令的超时 |
| `APP_CMD_CONFIRM_ROTATE_DEG` | 45 | 开关灯时舵机转动角度 |
| `APP_MAX_RECORDING_SEC` | 10 | 单次最长录音时长 |
| `APP_VAD_SILENCE_MS` | 600 | 静音多久算“说完了” |

## 🎵 更换提示音

1. 把 mp3 放进 `assets/mock_voices/`（文件名 = 数组名，如 `welcome.mp3`）；
2. 运行转换脚本（需要 ffmpeg）：

```bash
cd tools
pip install -r requirements.txt
python convert_audio.py
```

脚本会把 mp3 转成 16kHz/单声道/16bit 的 C 头文件，输出到
`components/voice_assets/include/mock_voices/`，然后在 `main.cc` 里
`#include "mock_voices/xxx.h"` 即可使用。

## ❓ 常见问题

- **没有声音**：检查 MAX98357A 接线与 3.3V 供电；确认喇叭接在功放 `+/-` 上。
- **识别不准**：安静环境、距麦克风 20~50cm；可在
  `main/voice_engine.cc` 里把 `DET_MODE_90` 改成 `DET_MODE_95`（更严格）。
- **连不上服务端**：确认电脑防火墙放行 8888 端口，且 `APP_WS_URI` 填的是电脑 IP。
  连不上也没关系，固件会自动降级为离线模式。
- **编译提示找不到 esp-sr**：首次编译需要联网，组件管理器会从 Espressif 组件仓下载
  `espressif/esp-sr` 与 `espressif/esp_websocket_client`。这两个依赖的版本已在
  `main/idf_component.yml` 和 `components/ws_client/idf_component.yml` 里**锁定**：
  `esp-sr` 从 2.2 起依赖 `esp-dl`，而 `esp-dl` 需要 ESP-IDF ≥ 5.5，与本工程的 5.4 不兼容
  （会报 `'MALLOC_CAP_SIMD' was not declared`）。升级到 ESP-IDF 5.5+ 后可放宽该约束。
- **内存不足**：确认开发板带 PSRAM，且 `sdkconfig.defaults.esp32s3` 已生效
  （`idf.py set-target esp32s3` 会读取它）。

## 📜 开源协议

Apache 2.0 —— 可自由用于个人/商业项目，注明原作者即可。

> 本项目由原 `xiaozhi-replica` 的 5 个示例合并重构而来，
> 各模块来源与设计细节见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。
