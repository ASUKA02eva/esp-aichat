/**
 * @file main.cc
 * @brief ESP-AI 智能语音助手 —— 统一固件
 *
 * 这个文件把原来 examples/ 下 5 个互相独立的示例合并成一条完整的交互链路：
 *
 *   ┌ 待唤醒 ──────────── 说“你好小智” 或 按按键 ─────────────┐
 *   │                                                        │
 *   │   ▼                                                    │
 *   │ ① 播放欢迎音，通知服务端“已唤醒”                        │
 *   │ ② 进入听写状态：本地命令词识别 + 边说边传给服务端        │
 *   │      · 命中 “帮我开灯 / 帮我关灯” → LED(+舵机动作) + 确认音
 *   │      · 命中 “拜拜”                → 退出对话
 *   │      · 命中自定义命令             → 播放自定义音频
 *   │ ③ VAD 检测到“说完了” → 等大模型回复（流式播放）          │
 *   │ ④ 回复播完 → 回到 ② 连续对话（不用再说唤醒词）           │
 *   │ ⑤ 说“拜拜”或超时无语音 → 回到待唤醒                      │
 *   └────────────────────────────────────────────────────────┘
 *
 * 硬件（沿用原示例的“小智 AI 标准接线”）：
 *   INMP441 麦克风 : VDD→3V3, GND→GND, SD→GPIO6, WS→GPIO4, SCK→GPIO5
 *   MAX98357A 功放 : VIN→3V3, GND→GND, DIN→GPIO7, BCLK→GPIO15, LRC→GPIO16
 *   LED            : 长脚→GPIO21, 短脚→GND（串限流电阻）
 *   按键           : 一端→3V3, 另一端→GPIO41（按下为高电平）
 *   SG90 舵机      : 红线→5V, 棕线→GND, 橙线→GPIO18
 *
 * 引脚都可在 idf.py menuconfig → ESP-AI 智能语音助手配置 中修改。
 */

extern "C"
{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_vad.h"

#include "bsp_board.h" // 麦克风 / 功放

#include "mock_voices/welcome.h"
#include "mock_voices/light_on.h"
#include "mock_voices/light_off.h"
#include "mock_voices/byebye.h"
#include "mock_voices/custom.h"
}

#include "app_config.h"
#include "voice_engine.h"
#include "audio_manager.h"
#include "led_controller.h"

#if CONFIG_APP_ENABLE_BUTTON
#include "button_controller.h"
#endif

#if CONFIG_APP_ENABLE_SERVO
#include "servo_controller.h"
#endif

#if CONFIG_APP_LLM_ENABLE
#include "wifi_manager.h"
#include "websocket_client.h"
#endif

static const char *TAG = "aichat";

// ============================================================================
// 全局对象
// ============================================================================
static VoiceEngine s_voice;
static AudioManager *s_audio = nullptr;
static LightController s_led(APP_LED_GPIO);

#if CONFIG_APP_ENABLE_SERVO
static ServoController s_servo(APP_SERVO_GPIO);
#endif

#if CONFIG_APP_ENABLE_BUTTON
static ButtonController s_button(APP_BUTTON_GPIO);
#endif

#if CONFIG_APP_LLM_ENABLE
static WiFiManager *s_wifi = nullptr;
static WebSocketClient *s_ws = nullptr;
#endif

static vad_handle_t s_vad = nullptr;

// ============================================================================
// 运行状态
// ============================================================================
enum AppState
{
    STATE_WAITING_WAKEUP = 0, // 待唤醒：听唤醒词 / 等按键
    STATE_RECORDING,          // 听写中：识别本地命令词 + 把音频传给大模型
    STATE_WAITING_RESPONSE,   // 等大模型回复
};

static volatile AppState s_state = STATE_WAITING_WAKEUP;
static bool s_llm_enabled = false;   // 是否真的连上了大模型服务端
static bool s_user_spoke = false;    // 本轮是否已检测到用户说话
static bool s_streaming_up = false;  // 是否正在把音频实时上传
static int s_silence_frames = 0;     // 连续静音帧计数
static TickType_t s_idle_start = 0;  // “等用户说话”的计时起点（0 表示用户已在说话）
static TickType_t s_response_start = 0; // “等大模型回复”的计时起点（兜底防卡死）

// 等待大模型回复的最长时间（兜底：服务端异常时不至于永远卡住）
#define APP_RESPONSE_TIMEOUT_MS (45 * 1000)

// ============================================================================
// 与大模型服务端通信（未启用 / 未连上时这些函数是空操作）
// ============================================================================
static void llm_send(const char *json)
{
#if CONFIG_APP_LLM_ENABLE
    if (s_llm_enabled && s_ws != nullptr && s_ws->isConnected())
    {
        s_ws->sendText(json);
    }
#else
    (void)json;
#endif
}

static void llm_send_audio(const int16_t *pcm, size_t samples)
{
#if CONFIG_APP_LLM_ENABLE
    if (s_llm_enabled && s_streaming_up && s_ws != nullptr && s_ws->isConnected())
    {
        s_ws->sendBinary((const uint8_t *)pcm, samples * sizeof(int16_t));
    }
#else
    (void)pcm;
    (void)samples;
#endif
}

#if CONFIG_APP_LLM_ENABLE
/**
 * @brief 服务端消息回调（运行在 WebSocket 任务里）
 *
 * 约定的协议（与 server/server.py 一致）：
 *   服务端 → 设备：二进制帧 = 一段 AI 语音（16bit PCM）；ping 帧 = 本轮回话结束
 */
static void on_websocket_event(const WebSocketClient::EventData &event)
{
    switch (event.type)
    {
    case WebSocketClient::EventType::CONNECTED:
        ESP_LOGI(TAG, "🔗 服务端已连接");
        break;

    case WebSocketClient::EventType::DISCONNECTED:
        ESP_LOGW(TAG, "🔌 服务端已断开");
        break;

    case WebSocketClient::EventType::DATA_BINARY:
        if (s_audio != nullptr && event.data_len > 0 && s_state == STATE_WAITING_RESPONSE)
        {
            if (!s_audio->isStreamingActive())
            {
                s_audio->startStreamingPlayback();
            }
            if (!s_audio->addStreamingAudioChunk(event.data, event.data_len))
            {
                ESP_LOGW(TAG, "流式播放缓冲区满，丢弃 %zu 字节", event.data_len);
            }
        }
        break;

    case WebSocketClient::EventType::PING:
        // 用 ping 作为“这一轮回复结束”的标志
        if (s_audio != nullptr)
        {
            if (s_audio->isStreamingActive())
            {
                s_audio->finishStreamingPlayback();
            }
            if (s_state == STATE_WAITING_RESPONSE)
            {
                s_audio->setStreamingComplete();
            }
        }
        break;

    case WebSocketClient::EventType::DATA_TEXT:
        if (event.data != nullptr && event.data_len > 0)
        {
            char *text = (char *)malloc(event.data_len + 1);
            if (text != nullptr)
            {
                memcpy(text, event.data, event.data_len);
                text[event.data_len] = '\0';
                ESP_LOGI(TAG, "服务端文本消息: %s", text);
                free(text);
            }
        }
        break;

    case WebSocketClient::EventType::ERROR:
        ESP_LOGW(TAG, "❌ 服务端连接出错");
        break;

    default:
        break;
    }
}
#endif // CONFIG_APP_LLM_ENABLE

// ============================================================================
// 局部动作：LED / 舵机
// ============================================================================
#if CONFIG_APP_ENABLE_SERVO
/**
 * @brief 舵机“点一下头”：转出去再转回来，作为命令已执行的物理反馈
 *
 * @param clockwise true=顺时针，false=逆时针
 */
static void servo_gesture(bool clockwise)
{
    if (APP_CMD_ROTATE_DEG <= 0)
    {
        return;
    }
    int deg = clockwise ? APP_CMD_ROTATE_DEG : -APP_CMD_ROTATE_DEG;
    s_servo.rotate(deg);
    vTaskDelay(pdMS_TO_TICKS(800));
    s_servo.rotate(-deg);
}
#endif

// ============================================================================
// 状态切换
// ============================================================================

/** 回到待唤醒状态 */
static void exit_dialog()
{
    ESP_LOGI(TAG, "👋 结束对话，回到待唤醒状态");
    s_audio->playAudio(byebye, byebye_len, "再见音频");

#if CONFIG_APP_LLM_ENABLE
    if (s_llm_enabled && s_ws != nullptr)
    {
        s_ws->disconnect();
    }
#endif

    s_audio->stopRecording();
    s_audio->clearRecordingBuffer();

    s_state = STATE_WAITING_WAKEUP;
    s_user_spoke = false;
    s_streaming_up = false;
    s_silence_frames = 0;
    s_idle_start = 0;
    s_response_start = 0;
}

/** 开始一轮听写（continuous=true 表示连续对话，不用再说唤醒词） */
static void start_listening(bool continuous)
{
    s_state = STATE_RECORDING;
    s_user_spoke = false;
    s_streaming_up = false;
    s_silence_frames = 0;
    s_idle_start = xTaskGetTickCount();

    s_audio->clearRecordingBuffer();
    s_audio->startRecording();
    s_voice.reset_command();
    if (s_vad != nullptr)
    {
        vad_reset_trigger(s_vad);
    }

    if (continuous)
    {
        ESP_LOGI(TAG, "🎙️ 继续听写中（连续对话，可直接说话）");
    }
}

/** 唤醒（唤醒词或按键触发） */
static void on_wake_detected()
{
    ESP_LOGI(TAG, "🎉 已唤醒");
    s_led.blink(1); // 视觉反馈，且不改变 LED 原来的亮灭状态

#if CONFIG_APP_LLM_ENABLE
    if (s_llm_enabled)
    {
        if (s_ws != nullptr && !s_ws->isConnected())
        {
            ESP_LOGI(TAG, "服务端未连接，尝试重连...");
            s_ws->connect();
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        char msg[192];
        snprintf(msg, sizeof(msg),
                 "{\"event\":\"wake_word_detected\",\"model\":\"%s\",\"timestamp\":%lld}",
                 s_voice.wake_model() != nullptr ? s_voice.wake_model() : "unknown",
                 (long long)(esp_timer_get_time() / 1000));
        llm_send(msg);
    }
#endif

    s_audio->playAudio(welcome, welcome_len, "欢迎音频");
    llm_send("{\"event\":\"recording_started\"}");

    start_listening(false);
}

/**
 * @brief 执行本地命令词（离线响应，不需要联网）
 *
 * 开关灯同时驱动 LED 和舵机：LED 表示“灯”的状态，舵机给出物理动作反馈。
 */
static void run_local_command(int command_id)
{
    // 命令是在“边说边传”的过程中命中的，先把这一轮上传作废，避免服务端收到半句话
    if (s_streaming_up)
    {
        s_streaming_up = false;
        llm_send("{\"event\":\"recording_cancelled\"}");
    }

    switch (command_id)
    {
    case VOICE_CMD_TURN_ON_LIGHT:
        ESP_LOGI(TAG, "💡 执行【开灯】");
        s_led.turnOn();
#if CONFIG_APP_ENABLE_SERVO
        servo_gesture(true);
#endif
        s_audio->playAudio(light_on, light_on_len, "开灯确认音频");
        break;

    case VOICE_CMD_TURN_OFF_LIGHT:
        ESP_LOGI(TAG, "🌑 执行【关灯】");
        s_led.turnOff();
#if CONFIG_APP_ENABLE_SERVO
        servo_gesture(false);
#endif
        s_audio->playAudio(light_off, light_off_len, "关灯确认音频");
        break;

    case VOICE_CMD_CUSTOM:
        ESP_LOGI(TAG, "🧩 执行【自定义命令】");
        s_audio->playAudio(custom, custom_len, "自定义命令音频");
        break;

    case VOICE_CMD_BYE_BYE:
        exit_dialog();
        return;

    default:
        ESP_LOGW(TAG, "未知命令 ID: %d", command_id);
        return;
    }

    // 命令执行完，继续留在对话里听下一句
    if (s_state == STATE_RECORDING)
    {
        s_user_spoke = false;
        s_silence_frames = 0;
        s_idle_start = xTaskGetTickCount();
        s_audio->clearRecordingBuffer();
        s_audio->startRecording();
        s_voice.reset_command();
        if (s_vad != nullptr)
        {
            vad_reset_trigger(s_vad);
        }
    }
}

/** 本轮录音结束：要么把音频交给大模型，要么继续待命 */
static void finish_recording()
{
    s_audio->stopRecording();
    s_streaming_up = false;

    size_t recorded = 0;
    s_audio->getRecordingBuffer(recorded);

    // 没说话 / 音频太短：当作误唤醒，直接重新开始听（连续对话模式下）
    if (!s_user_spoke || recorded < (size_t)APP_MIN_VOICE_SAMPLES)
    {
        ESP_LOGI(TAG, "本轮没有有效语音，继续待命");
        llm_send("{\"event\":\"recording_cancelled\"}");
        start_listening(true);
        return;
    }

    llm_send("{\"event\":\"recording_ended\"}");

#if CONFIG_APP_LLM_ENABLE
    if (s_llm_enabled)
    {
        s_state = STATE_WAITING_RESPONSE;
        s_response_start = xTaskGetTickCount();
        s_audio->resetResponsePlayedFlag();
        ESP_LOGI(TAG, "⏳ 已发送 %.2f 秒音频，等待大模型回复...", s_audio->getRecordingDuration());
        return;
    }
#endif

    // 离线模式：没有服务端，直接继续听下一句
    ESP_LOGI(TAG, "离线模式：跳过等待回复，继续待命");
    start_listening(true);
}

/** 听写状态下的每帧处理 */
static void handle_recording(const int16_t *audio, int samples)
{
    // ① 本地命令词优先（离线、零延迟）
    int command_id = s_voice.detect_command(audio);
    if (command_id != VOICE_CMD_NONE)
    {
        run_local_command(command_id);
        return;
    }

    // ② 录音缓冲 + 边说边传给服务端
    if (s_audio->isRecording() && !s_audio->isRecordingBufferFull())
    {
        s_audio->addRecordingData(audio, samples);
        llm_send_audio(audio, samples);
    }

    // ③ VAD 判断用户是否在说话
    vad_state_t vad = vad_process(s_vad, (int16_t *)audio, APP_SAMPLE_RATE, APP_VAD_FRAME_MS);
    if (vad == VAD_SPEECH)
    {
        if (!s_user_spoke)
        {
            ESP_LOGI(TAG, "🗣️ 检测到说话，开始录音/传输");
        }
        s_user_spoke = true;
        s_silence_frames = 0;
        s_idle_start = 0; // 用户已开口，取消“等说话”超时
        if (!s_streaming_up)
        {
            s_streaming_up = true;
        }
        return;
    }

    if (vad == VAD_SILENCE && s_user_spoke)
    {
        s_silence_frames++;
        if (s_silence_frames >= APP_VAD_SILENCE_FRAMES)
        {
            ESP_LOGI(TAG, "🤐 检测到说话结束（录音 %.2f 秒）", s_audio->getRecordingDuration());
            finish_recording();
            return;
        }
    }

    // ④ 录音缓冲满 → 本轮结束
    if (s_audio->isRecordingBufferFull())
    {
        ESP_LOGW(TAG, "⏹️ 达到单次最长录音时长（%d 秒）", APP_MAX_RECORDING_SEC);
        finish_recording();
        return;
    }

    // ⑤ 一直没说话 → 超时退出
    if (!s_user_spoke && s_idle_start != 0)
    {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_idle_start) > pdMS_TO_TICKS(APP_IDLE_TIMEOUT_MS))
        {
            ESP_LOGW(TAG, "⏰ %d 秒没有听到说话，结束对话", APP_IDLE_TIMEOUT_MS / 1000);
            exit_dialog();
        }
    }
}

// ============================================================================
// 主入口
// ============================================================================
extern "C" void app_main(void)
{
    // ---------- ① NVS（WiFi 需要） ----------
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "=========== ESP-AI 智能语音助手启动 ===========");

    // ---------- ② 本地外设 ----------
    s_led.init();

#if CONFIG_APP_ENABLE_SERVO
    if (s_servo.init() != ESP_OK)
    {
        ESP_LOGW(TAG, "舵机初始化失败（可能没接舵机），继续运行");
    }
#endif

#if CONFIG_APP_ENABLE_BUTTON
    if (s_button.init() != ESP_OK)
    {
        ESP_LOGW(TAG, "按键初始化失败，仅支持语音唤醒");
    }
#endif

    // ---------- ③ 网络与大模型服务端（可选） ----------
#if CONFIG_APP_LLM_ENABLE
    s_llm_enabled = (strlen(CONFIG_APP_WIFI_SSID) > 0);
    if (!s_llm_enabled)
    {
        ESP_LOGW(TAG, "未配置 WiFi（menuconfig → ESP-AI 智能语音助手配置）→ 离线模式");
    }
    else
    {
        ESP_LOGI(TAG, "正在连接 WiFi: %s", CONFIG_APP_WIFI_SSID);
        s_wifi = new WiFiManager(CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_PASSWORD);
        if (s_wifi->connect() != ESP_OK)
        {
            ESP_LOGE(TAG, "WiFi 连接失败 → 降级为离线模式");
            delete s_wifi;
            s_wifi = nullptr;
            s_llm_enabled = false;
        }
    }

    if (s_llm_enabled)
    {
        ESP_LOGI(TAG, "正在连接大模型服务端: %s", CONFIG_APP_WS_URI);
        s_ws = new WebSocketClient(CONFIG_APP_WS_URI, true, 5000);
        s_ws->setEventCallback(on_websocket_event);
        if (s_ws->connect() != ESP_OK)
        {
            ESP_LOGE(TAG, "服务端连接失败（确认电脑上已运行 server/server.py）→ 降级为离线模式");
            delete s_ws;
            s_ws = nullptr;
            s_llm_enabled = false;
        }
    }

    ESP_LOGI(TAG, "大模型对话: %s", s_llm_enabled ? "已启用" : "未启用（离线模式）");
#else
    ESP_LOGI(TAG, "大模型对话: 已在 menuconfig 中关闭（离线模式）");
#endif

    // ---------- ④ 音频硬件 ----------
    ESP_LOGI(TAG, "初始化麦克风 INMP441（16kHz / 单声道 / 16bit）...");
    ret = bsp_board_init(APP_SAMPLE_RATE, 1, 16);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "麦克风初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查接线: VDD→3.3V, GND→GND, SD→GPIO6, WS→GPIO4, SCK→GPIO5");
        return;
    }

    ESP_LOGI(TAG, "初始化功放 MAX98357A（16kHz / 单声道 / 16bit）...");
    ret = bsp_audio_init(APP_SAMPLE_RATE, 1, 16);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "功放初始化失败: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "请检查接线: DIN→GPIO7, BCLK→GPIO15, LRC→GPIO16, VIN→3.3V");
        return;
    }

    // ---------- ⑤ VAD（判断“开始说话 / 说完了”） ----------
    s_vad = vad_create_with_param(VAD_MODE_1, APP_SAMPLE_RATE, APP_VAD_FRAME_MS,
                                  APP_VAD_MIN_SPEECH_MS, APP_VAD_MIN_NOISE_MS);
    if (s_vad == NULL)
    {
        ESP_LOGE(TAG, "VAD 初始化失败");
        return;
    }

    // ---------- ⑥ 语音引擎（唤醒词 + 命令词 + 降噪） ----------
    if (s_voice.init() != ESP_OK)
    {
        ESP_LOGE(TAG, "语音引擎初始化失败，请检查模型是否已烧录");
        return;
    }

    // ---------- ⑦ 音频管理器（录音缓冲 / 播放 / 流式播放） ----------
    s_audio = new AudioManager(APP_SAMPLE_RATE, APP_MAX_RECORDING_SEC, 32);
    if (s_audio->init() != ESP_OK)
    {
        ESP_LOGE(TAG, "音频管理器初始化失败（内存不足？）");
        delete s_audio;
        s_audio = nullptr;
        return;
    }

    int frame_bytes = s_voice.frame_bytes();
    int16_t *frame = (int16_t *)malloc(frame_bytes);
    if (frame == nullptr)
    {
        ESP_LOGE(TAG, "音频帧缓冲分配失败（需要 %d 字节）", frame_bytes);
        return;
    }

    // ---------- ⑧ 打印配置 & 支持的指令 ----------
    ESP_LOGI(TAG, "--------------- 配置汇总 ---------------");
    ESP_LOGI(TAG, "唤醒词模型 : %s", s_voice.wake_model());
    ESP_LOGI(TAG, "命令词模型 : %s", s_voice.command_model());
    ESP_LOGI(TAG, "噪声抑制   : %s", s_voice.noise_suppression_enabled() ? "开" : "关");
    ESP_LOGI(TAG, "每帧音频   : %d 字节", frame_bytes);
    ESP_LOGI(TAG, "LED        : GPIO%d", CONFIG_APP_LED_GPIO);
#if CONFIG_APP_ENABLE_BUTTON
    ESP_LOGI(TAG, "按键       : GPIO%d（已启用，按下即唤醒）", CONFIG_APP_BUTTON_GPIO);
#else
    ESP_LOGI(TAG, "按键       : 未启用");
#endif
#if CONFIG_APP_ENABLE_SERVO
    ESP_LOGI(TAG, "舵机       : GPIO%d（已启用，开关灯时转动 %d 度）",
             CONFIG_APP_SERVO_GPIO, APP_CMD_ROTATE_DEG);
#else
    ESP_LOGI(TAG, "舵机       : 未启用");
#endif
    ESP_LOGI(TAG, "支持的语音指令：");
    for (int i = 0; i < s_voice.command_count(); i++)
    {
        ESP_LOGI(TAG, "  · %s", s_voice.commands()[i].description);
    }
    ESP_LOGI(TAG, "----------------------------------------");
    ESP_LOGI(TAG, "✅ 系统就绪，请说“你好小智”或按按键开始对话");

    // ---------- ⑨ 主循环 ----------
    while (true)
    {
        ret = bsp_get_feed_data(false, frame, frame_bytes);
        if (ret != ESP_OK)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 可选降噪；没启用时原样返回
        const int16_t *audio = s_voice.denoise(frame, frame_bytes / sizeof(int16_t));

        // 待唤醒时，按键等同于唤醒词
#if CONFIG_APP_ENABLE_BUTTON
        if (s_state == STATE_WAITING_WAKEUP && s_button.consume_press())
        {
            on_wake_detected();
            continue;
        }
#endif

        switch (s_state)
        {
        case STATE_WAITING_WAKEUP:
            if (s_voice.detect_wake(audio))
            {
                on_wake_detected();
            }
            break;

        case STATE_RECORDING:
            handle_recording(audio, frame_bytes / sizeof(int16_t));
            break;

        case STATE_WAITING_RESPONSE:
            // 音频通过 WebSocket 流式接收并播放，这里只等它播完
            if (s_audio->isResponsePlayed())
            {
                llm_send("{\"event\":\"recording_started\"}");
                start_listening(true);
                ESP_LOGI(TAG, "💬 连续对话中：可以继续提问、下命令，或说“拜拜”结束");
            }
            else if ((xTaskGetTickCount() - s_response_start) > pdMS_TO_TICKS(APP_RESPONSE_TIMEOUT_MS))
            {
                ESP_LOGW(TAG, "⏰ 等待大模型回复超时，结束对话");
                exit_dialog();
            }
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
