/**
 * @file audio_manager.cc
 * @brief AudioManager 实现：录音缓冲、整段播放与流式播放
 *
 * 缓冲区尺寸由构造函数按采样率算出，init() 一次性分配，运行期不再扩容。
 */

extern "C" {
#include <string.h>
#include "esp_log.h"
#include "bsp_board.h"
}

#include "audio_manager.h"

const char* AudioManager::TAG = "AudioManager";

AudioManager::AudioManager(uint32_t sample_rate, uint32_t recording_duration_sec, uint32_t response_duration_sec)
    : sample_rate(sample_rate)
    , recording_duration_sec(recording_duration_sec)
    , response_duration_sec(response_duration_sec)
    , recording_buffer(nullptr)
    , recording_buffer_size(0)
    , recording_length(0)
    , is_recording(false)
    , response_buffer(nullptr)
    , response_buffer_size(0)
    , response_length(0)
    , response_played(false)
    , is_streaming(false)
    , streaming_buffer(nullptr)
    , streaming_buffer_size(STREAMING_BUFFER_SIZE)
    , streaming_write_pos(0)
    , streaming_read_pos(0)
{
    // 两个缓冲区单位不同：录音按样本数，响应按字节数
    recording_buffer_size = sample_rate * recording_duration_sec;  // 录音缓冲区（样本数）
    response_buffer_size = sample_rate * response_duration_sec * sizeof(int16_t);  // 响应缓冲区（字节数）
}

AudioManager::~AudioManager() {
    deinit();
}

esp_err_t AudioManager::init() {
    ESP_LOGI(TAG, "初始化音频管理器");
    
    recording_buffer = (int16_t*)malloc(recording_buffer_size * sizeof(int16_t));
    if (recording_buffer == nullptr) {
        ESP_LOGE(TAG, "录音缓冲区分配失败，需要 %zu 字节", 
                 recording_buffer_size * sizeof(int16_t));
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "录音缓冲区分配成功: %zu 字节 (%lu 秒)", 
             recording_buffer_size * sizeof(int16_t), (unsigned long)recording_duration_sec);
    
    response_buffer = (int16_t*)calloc(response_buffer_size / sizeof(int16_t), sizeof(int16_t));
    if (response_buffer == nullptr) {
        ESP_LOGE(TAG, "响应缓冲区分配失败，需要 %zu 字节", response_buffer_size);
        free(recording_buffer);
        recording_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "响应缓冲区分配成功: %zu 字节 (%lu 秒)", 
             response_buffer_size, (unsigned long)response_duration_sec);
    
    streaming_buffer = (uint8_t*)malloc(streaming_buffer_size);
    if (streaming_buffer == nullptr) {
        ESP_LOGE(TAG, "流式播放缓冲区分配失败，需要 %zu 字节", streaming_buffer_size);
        free(recording_buffer);
        free(response_buffer);
        recording_buffer = nullptr;
        response_buffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "流式播放缓冲区分配成功: %zu 字节", streaming_buffer_size);
    
    return ESP_OK;
}

void AudioManager::deinit() {
    if (recording_buffer != nullptr) {
        free(recording_buffer);
        recording_buffer = nullptr;
    }
    
    if (response_buffer != nullptr) {
        free(response_buffer);
        response_buffer = nullptr;
    }
    
    
    if (streaming_buffer != nullptr) {
        free(streaming_buffer);
        streaming_buffer = nullptr;
    }
}

void AudioManager::startRecording() {
    is_recording = true;
    recording_length = 0;
    ESP_LOGI(TAG, "开始录音");
}

void AudioManager::stopRecording() {
    is_recording = false;
    ESP_LOGI(TAG, "停止录音，当前长度: %zu 样本 (%.2f 秒)", 
             recording_length, getRecordingDuration());
}

bool AudioManager::addRecordingData(const int16_t* data, size_t samples) {
    if (!is_recording || recording_buffer == nullptr) {
        return false;
    }
    
    if (recording_length + samples > recording_buffer_size) {
        ESP_LOGW(TAG, "录音缓冲区已满，丢弃本次数据");
        return false;
    }
    
    memcpy(&recording_buffer[recording_length], data, samples * sizeof(int16_t));
    recording_length += samples;
    
    return true;
}

const int16_t* AudioManager::getRecordingBuffer(size_t& length) const {
    length = recording_length;
    return recording_buffer;
}

void AudioManager::clearRecordingBuffer() {
    recording_length = 0;
}

float AudioManager::getRecordingDuration() const {
    return (float)recording_length / sample_rate;
}

bool AudioManager::isRecordingBufferFull() const {
    return recording_length >= recording_buffer_size;
}

void AudioManager::startReceivingResponse() {
    response_length = 0;
    response_played = false;
}

bool AudioManager::addResponseData(const uint8_t* data, size_t size) {
    size_t samples = size / sizeof(int16_t);
    
    if (samples * sizeof(int16_t) > response_buffer_size) {
        ESP_LOGW(TAG, "响应数据过大，超过缓冲区限制");
        return false;
    }
    
    memcpy(response_buffer, data, size);
    response_length = samples;
    
    ESP_LOGI(TAG, "接收到完整音频数据: %zu 字节, %zu 样本", size, samples);
    return true;
}

esp_err_t AudioManager::finishResponseAndPlay() {
    if (response_length == 0) {
        ESP_LOGW(TAG, "没有响应音频数据可播放");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "播放响应音频: %zu 样本 (%.2f 秒)",
             response_length, (float)response_length / sample_rate);
    
    // 播放偶发失败时重试，最多 3 次
    int retry_count = 0;
    const int max_retries = 3;
    esp_err_t audio_ret = ESP_FAIL;
    
    while (retry_count < max_retries && audio_ret != ESP_OK) {
        audio_ret = bsp_play_audio((const uint8_t*)response_buffer, response_length * sizeof(int16_t));
        if (audio_ret == ESP_OK) {
            ESP_LOGI(TAG, "响应音频播放成功");
            response_played = true;
            break;
        } else {
            ESP_LOGE(TAG, "音频播放失败 (第%d次尝试): %s",
                     retry_count + 1, esp_err_to_name(audio_ret));
            retry_count++;
            if (retry_count < max_retries) {
                vTaskDelay(pdMS_TO_TICKS(100)); // 等100ms再试
            }
        }
    }
    
    return audio_ret;
}

esp_err_t AudioManager::playAudio(const uint8_t* audio_data, size_t data_len, const char* description) {
    ESP_LOGI(TAG, "播放%s", description);
    esp_err_t ret = bsp_play_audio(audio_data, data_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "%s播放成功", description);
    } else {
        ESP_LOGE(TAG, "%s播放失败: %s", description, esp_err_to_name(ret));
    }
    return ret;
}

void AudioManager::startStreamingPlayback() {
    ESP_LOGI(TAG, "开始流式音频播放");
    is_streaming = true;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
    
    if (streaming_buffer) {
        memset(streaming_buffer, 0, streaming_buffer_size);
    }
}

bool AudioManager::addStreamingAudioChunk(const uint8_t* data, size_t size) {
    if (!is_streaming || !streaming_buffer || !data) {
        return false;
    }
    
    // 环形缓冲区保留 1 字节空位，用于区分“满”与“空”
    size_t available_space;
    if (streaming_write_pos >= streaming_read_pos) {
        available_space = streaming_buffer_size - (streaming_write_pos - streaming_read_pos) - 1;
    } else {
        available_space = streaming_read_pos - streaming_write_pos - 1;
    }
    
    if (size > available_space) {
        ESP_LOGW(TAG, "流式缓冲区不足: 需要 %zu 字节, 可用 %zu 字节", size, available_space);
        return false;
    }
    
    // 数据跨越缓冲区末尾时拆成两段写入
    size_t bytes_to_end = streaming_buffer_size - streaming_write_pos;
    if (size <= bytes_to_end) {
        memcpy(streaming_buffer + streaming_write_pos, data, size);
        streaming_write_pos += size;
    } else {
        memcpy(streaming_buffer + streaming_write_pos, data, bytes_to_end);
        memcpy(streaming_buffer, data + bytes_to_end, size - bytes_to_end);
        streaming_write_pos = size - bytes_to_end;
    }
    
    if (streaming_write_pos >= streaming_buffer_size) {
        streaming_write_pos = 0;
    }
    
    ESP_LOGD(TAG, "添加流式音频块: %zu 字节, 写位置: %zu, 读位置: %zu", 
             size, streaming_write_pos, streaming_read_pos);
    
    size_t available_data;
    if (streaming_write_pos >= streaming_read_pos) {
        available_data = streaming_write_pos - streaming_read_pos;
    } else {
        available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    // 累积到一块（200ms）再推给 I2S，避免频繁启停
    while (available_data >= STREAMING_CHUNK_SIZE) {
        uint8_t chunk[STREAMING_CHUNK_SIZE];
        
        size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
        if (STREAMING_CHUNK_SIZE <= bytes_to_end) {
            memcpy(chunk, streaming_buffer + streaming_read_pos, STREAMING_CHUNK_SIZE);
            streaming_read_pos += STREAMING_CHUNK_SIZE;
        } else {
            memcpy(chunk, streaming_buffer + streaming_read_pos, bytes_to_end);
            memcpy(chunk + bytes_to_end, streaming_buffer, STREAMING_CHUNK_SIZE - bytes_to_end);
            streaming_read_pos = STREAMING_CHUNK_SIZE - bytes_to_end;
        }
        
        if (streaming_read_pos >= streaming_buffer_size) {
            streaming_read_pos = 0;
        }
        
        // 流式接口，连续播放时不停止 I2S
        esp_err_t ret = bsp_play_audio_stream(chunk, STREAMING_CHUNK_SIZE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "流式音频播放失败: %s", esp_err_to_name(ret));
            break;
        }
        
        if (streaming_write_pos >= streaming_read_pos) {
            available_data = streaming_write_pos - streaming_read_pos;
        } else {
            available_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
        }
    }
    
    return true;
}

void AudioManager::finishStreamingPlayback() {
    if (!is_streaming) {
        return;
    }
    
    ESP_LOGI(TAG, "结束流式音频播放");
    
    // 收尾阶段：播放不足一块的剩余数据
    size_t remaining_data;
    if (streaming_write_pos >= streaming_read_pos) {
        remaining_data = streaming_write_pos - streaming_read_pos;
    } else {
        remaining_data = streaming_buffer_size - streaming_read_pos + streaming_write_pos;
    }
    
    if (remaining_data > 0) {
        uint8_t* remaining_buffer = (uint8_t*)malloc(remaining_data);
        if (remaining_buffer) {
            if (streaming_write_pos >= streaming_read_pos) {
                memcpy(remaining_buffer, streaming_buffer + streaming_read_pos, remaining_data);
            } else {
                size_t bytes_to_end = streaming_buffer_size - streaming_read_pos;
                memcpy(remaining_buffer, streaming_buffer + streaming_read_pos, bytes_to_end);
                memcpy(remaining_buffer + bytes_to_end, streaming_buffer, streaming_write_pos);
            }
            
            // 普通播放接口会停止 I2S，用于收尾
            esp_err_t ret = bsp_play_audio(remaining_buffer, remaining_data);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "播放剩余音频: %zu 字节", remaining_data);
            } else {
                ESP_LOGE(TAG, "播放剩余音频失败: %s", esp_err_to_name(ret));
            }
            
            free(remaining_buffer);
        }
    }
    
    is_streaming = false;
    streaming_write_pos = 0;
    streaming_read_pos = 0;
}