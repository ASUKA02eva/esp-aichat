/**
 * @file app_config.h
 * @brief 配置入口：可调参数在 Kconfig，本文件放与硬件、语音绑定的常量。
 */

#pragma once

#include "sdkconfig.h"

extern "C"
{
#include "driver/gpio.h"
}

// 硬件引脚
#define APP_LED_GPIO ((gpio_num_t)CONFIG_APP_LED_GPIO)       // 外接 LED：长脚接该引脚，短脚接 GND
#define APP_BUTTON_GPIO ((gpio_num_t)CONFIG_APP_BUTTON_GPIO) // 按键：接 3V3 与该引脚（按下为高电平）
#define APP_SERVO_GPIO ((gpio_num_t)CONFIG_APP_SERVO_GPIO)   // 舵机 PWM 信号线

// 音频参数
#define APP_SAMPLE_RATE 16000      // 采样率 16kHz，语音识别模型要求
#define APP_VAD_FRAME_MS 30        // VAD 帧长 30ms
#define APP_VAD_MIN_SPEECH_MS 200  // 最短语音时长，低于该值不算有效语音
#define APP_VAD_MIN_NOISE_MS 1000  // 最短静音时长

// 行为参数
#define APP_IDLE_TIMEOUT_MS CONFIG_APP_IDLE_TIMEOUT_MS
#define APP_MAX_RECORDING_SEC CONFIG_APP_MAX_RECORDING_SEC
#define APP_CMD_ROTATE_DEG CONFIG_APP_CMD_CONFIRM_ROTATE_DEG

// 连续静音多少帧认为“用户说完了”（帧长为 APP_VAD_FRAME_MS）
#define APP_VAD_SILENCE_FRAMES (CONFIG_APP_VAD_SILENCE_MS / APP_VAD_FRAME_MS)

// 有效语音的最小样本数，0.25 秒以下的音频不作为大模型输入
#define APP_MIN_VOICE_SAMPLES (APP_SAMPLE_RATE / 4)
