/**
 * @file voice_engine.h
 * @brief 语音识别引擎：把 ESP-SR 的「唤醒词检测 / 命令词识别 / 噪声抑制」封装成一个类
 *
 * 原来这三个能力散落在各个示例的 main.cc 里，现在统一收口在这里，
 * main.cc 只需要关心业务状态机。
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

/** 本地命令词 ID（与 MultiNet 命令表一致，沿用原示例的编号） */
enum VoiceCommandId
{
    VOICE_CMD_NONE = -1,
    VOICE_CMD_TURN_OFF_LIGHT = 308, // "帮我关灯"
    VOICE_CMD_TURN_ON_LIGHT = 309,  // "帮我开灯"
    VOICE_CMD_BYE_BYE = 314,        // "拜拜"
    VOICE_CMD_CUSTOM = 315,         // "现在安全屋情况如何"（演示自定义命令）
};

class VoiceEngine
{
public:
    /** 一条命令词的定义 */
    struct Command
    {
        int id;
        const char *pinyin;      // 拼音（MultiNet 用拼音匹配）
        const char *description; // 中文说明（只用于打印）
    };

    VoiceEngine();
    ~VoiceEngine();

    /**
     * @brief 加载唤醒词、命令词（可选：噪声抑制）模型
     */
    esp_err_t init();

    /** 每帧需要喂给模型的音频字节数 */
    int frame_bytes() const { return frame_bytes_; }
    const char *wake_model() const { return wake_model_; }
    const char *command_model() const { return command_model_; }
    bool noise_suppression_enabled() const { return nsn_data_ != nullptr; }

    /**
     * @brief 对一帧音频做噪声抑制
     *
     * @param pcm     输入（16bit 单声道）
     * @param samples 样本数
     * @return 处理后的一帧数据；没有启用降噪时直接返回入参
     */
    const int16_t *denoise(const int16_t *pcm, int samples);

    /** 是否检测到唤醒词 */
    bool detect_wake(const int16_t *pcm);

    /**
     * @brief 命令词识别
     *
     * @return 命中的命令 ID（VoiceCommandId）；没有命中返回 VOICE_CMD_NONE
     */
    int detect_command(const int16_t *pcm);

    /** 清空命令词识别缓冲（每次开始新一轮听写前调用） */
    void reset_command();

    int command_count() const;
    const Command *commands() const;

    /** 命令 ID → 中文说明 */
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
