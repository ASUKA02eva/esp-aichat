/**
 * @file voice_engine.h
 * @brief 语音识别引擎：封装 ESP-SR 的唤醒词检测、命令词识别与噪声抑制。
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

/** 本地命令词 ID（与 MultiNet 命令表一致） */
enum VoiceCommandId
{
    VOICE_CMD_NONE = -1,
    VOICE_CMD_TURN_OFF_LIGHT = 308, // "帮我关灯"
    VOICE_CMD_TURN_ON_LIGHT = 309,  // "帮我开灯"
    VOICE_CMD_BYE_BYE = 314,        // "拜拜"
    VOICE_CMD_CUSTOM = 315,         // "现在安全屋情况如何"
};

class VoiceEngine
{
public:
    struct Command
    {
        int id;
        const char *pinyin;      // 拼音（MultiNet 用拼音匹配）
        const char *description; // 中文说明（只用于打印）
    };

    VoiceEngine();
    ~VoiceEngine();

    // 加载唤醒词、命令词与可选的噪声抑制模型；任一必需模型缺失则返回错误
    esp_err_t init();

    /** 每帧需要喂给模型的音频字节数 */
    int frame_bytes() const { return frame_bytes_; }
    const char *wake_model() const { return wake_model_; }
    const char *command_model() const { return command_model_; }
    bool noise_suppression_enabled() const { return nsn_data_ != nullptr; }

    // 对一帧音频做噪声抑制，返回处理后的一帧数据。
    // 未启用降噪、或帧长与降噪模型不一致时，原样返回入参 pcm。
    const int16_t *denoise(const int16_t *pcm, int samples);

    bool detect_wake(const int16_t *pcm);

    // 命令词识别，返回命中的命令 ID；未命中返回 VOICE_CMD_NONE
    int detect_command(const int16_t *pcm);

    /** 清空命令词识别缓冲（每次开始新一轮听写前调用） */
    void reset_command();

    int command_count() const;
    const Command *commands() const;

    const char *describe(int command_id) const;

private:
    esp_err_t load_models();
    esp_err_t load_wake_model();
    esp_err_t load_command_model();
    esp_err_t configure_commands();
    void load_noise_suppression();

    void *models_;      // srmodel_list_t*
    void *wakenet_;     // esp_wn_iface_t*
    void *wake_data_;   // model_iface_data_t*（唤醒词）
    void *multinet_;    // esp_mn_iface_t*
    void *mn_data_;     // model_iface_data_t*（命令词）
    void *nsn_;         // esp_nsn_iface_t*
    void *nsn_data_;    // esp_nsn_data_t*
    int16_t *nsn_out_;  // 噪声抑制输出缓冲
    int nsn_chunk_;     // 噪声抑制每帧样本数

    int frame_bytes_;
    const char *wake_model_;
    const char *command_model_;
};
