/**
 * @file audio_manager.h
 * @brief AudioManager：录音缓冲、整段播放与流式播放
 *
 * 三块缓冲区都在 init() 中一次性分配，失败返回 ESP_ERR_NO_MEM 并回滚已分配的内存。
 * addRecordingData() 的 samples 是样本数而非字节数。
 */

#ifndef AUDIO_MANAGER_H
#define AUDIO_MANAGER_H

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

class AudioManager {
public:
    // 采样率默认 16kHz；录音上限 10 秒，单次回复音频上限 32 秒
    AudioManager(uint32_t sample_rate = 16000, 
                 uint32_t recording_duration_sec = 10,
                 uint32_t response_duration_sec = 32);
    
    ~AudioManager();

    // 三块缓冲区一次性分配；内存不足返回 ESP_ERR_NO_MEM，且不留下已分配的缓冲区
    esp_err_t init();

    void deinit();

    void startRecording();

    void stopRecording();

    bool isRecording() const { return is_recording; }

    // samples 为样本数；缓冲区剩余空间不足时返回 false，本次数据整体丢弃
    bool addRecordingData(const int16_t* data, size_t samples);

    // length 返回当前录音的样本数
    const int16_t* getRecordingBuffer(size_t& length) const;

    void clearRecordingBuffer();

    // 返回秒数
    float getRecordingDuration() const;

    bool isRecordingBufferFull() const;

    // 整段播放：一次收完完整回复音频再播放
    void startReceivingResponse();

    // 覆盖式写入；size 超过 response_buffer_size 时返回 false
    bool addResponseData(const uint8_t* data, size_t size);

    esp_err_t finishResponseAndPlay();
    
    // 流式播放：按 32KB 环形缓冲累积，满一块（200ms）推一次 I2S
    
    void startStreamingPlayback();
    
    // 环形缓冲区空间不足时返回 false，调用方可稍后重试
    bool addStreamingAudioChunk(const uint8_t* data, size_t size);
    
    void finishStreamingPlayback();
    
    bool isStreamingActive() const { return is_streaming; }
    
    void setStreamingComplete() { response_played = true; }

    // description 仅用于日志
    esp_err_t playAudio(const uint8_t* audio_data, size_t data_len, const char* description);

    bool isResponsePlayed() const { return response_played; }

    void resetResponsePlayedFlag() { response_played = false; }


    uint32_t getSampleRate() const { return sample_rate; }

    // 单位是样本数
    size_t getRecordingBufferSize() const { return recording_buffer_size; }

    // 单位是字节数
    size_t getResponseBufferSize() const { return response_buffer_size; }

private:
    uint32_t sample_rate;               // 采样率（Hz）
    uint32_t recording_duration_sec;    // 最大录音时长（秒）
    uint32_t response_duration_sec;     // 最大回复时长（秒）

    // 录音
    int16_t* recording_buffer;          // 录音数据缓冲区
    size_t recording_buffer_size;       // 缓冲区大小（样本数）
    size_t recording_length;            // 已录制的样本数
    bool is_recording;                  // 是否正在录音

    // 响应音频
    int16_t* response_buffer;           // AI回复音频缓冲区
    size_t response_buffer_size;        // 缓冲区大小（字节数）
    size_t response_length;             // 已接收的样本数
    bool response_played;               // 是否已播放完成

    
    // 流式播放
    bool is_streaming;                  // 是否在流式播放中
    uint8_t* streaming_buffer;          // 环形缓冲区
    size_t streaming_buffer_size;       // 缓冲区大小
    size_t streaming_write_pos;         // 写入位置
    size_t streaming_read_pos;          // 读取位置
    static const size_t STREAMING_BUFFER_SIZE = 32768; // 32KB环形缓冲区
    static const size_t STREAMING_CHUNK_SIZE = 3200;   // 每次播放3200字节（200ms）

    static const char* TAG;
};

#endif // AUDIO_MANAGER_H