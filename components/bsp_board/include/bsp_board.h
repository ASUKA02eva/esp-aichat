/**
 * @file bsp_board.h
 * @brief 音频输入输出的板级接口（ESP32-S3-DevKitC-1 + INMP441 + MAX98357A）
 *
 * 录音固定单声道；播放不校验采样率，需调用方保证与写入数据一致。
 *
 * @copyright Copyright 2021 Espressif Systems (Shanghai) Co. Ltd.
 *      Licensed under the Apache License, Version 2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化录音通路，使用麦克风前必须调用。
 *
 * @param sample_rate 采样率（Hz），语音识别取 16000
 * @param channel_format 当前未使用，录音固定为单声道
 * @param bits_per_chan 每个采样点的位数，32 以外的值按 16 位处理
 * @return ESP_OK 成功；其余为 I2S 驱动返回的错误码
 */
esp_err_t bsp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan);

/**
 * 阻塞读取一帧音频，直到读满 buffer_len 字节。
 *
 * @param is_get_raw_channel true 时跳过电平处理，直接返回 I2S 原始数据
 * @param buffer 输出缓冲，按 int16 采样存放
 * @param buffer_len 期望读取的字节数
 * @return ESP_OK 成功；其余为 I2S 驱动返回的错误码
 */
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len);

/**
 * 麦克风按单声道采集，与 I2S 槽位配置保持一致。
 *
 * @return 固定为 1
 */
int bsp_get_feed_channel(void);

/**
 * 初始化播放通路，使用扬声器前必须调用。
 *
 * @param sample_rate 采样率（Hz），须与待播放数据一致
 * @param channel_format 声道数，1 为单声道
 * @param bits_per_chan 每个采样点的位数，32 以外的值按 16 位处理
 * @return ESP_OK 成功；其余为 GPIO 或 I2S 驱动返回的错误码
 */
esp_err_t bsp_audio_init(uint32_t sample_rate, int channel_format, int bits_per_chan);

/**
 * 播放一段完整音频，结束后关断功放以消除底噪。
 *
 * @param audio_data PCM 数据
 * @param data_len 字节数
 * @return ESP_OK 写入完成；ESP_FAIL 写入不完整；其余为参数或状态错误
 */
esp_err_t bsp_play_audio(const uint8_t *audio_data, size_t data_len);

/**
 * 播放音频且结束后不关闭 I2S，供连续播放多个音频块使用。
 *
 * @param audio_data PCM 数据
 * @param data_len 字节数
 * @return ESP_OK 写入完成；ESP_FAIL 写入不完整；其余为参数或状态错误
 */
esp_err_t bsp_play_audio_stream(const uint8_t *audio_data, size_t data_len);

/**
 * 停止播放并关断功放。
 *
 * @return ESP_OK 已停止或本就处于停止状态；其余为禁用通道失败的错误码
 */
esp_err_t bsp_audio_stop(void);

#ifdef __cplusplus
}
#endif
