/**
 * ESP32-S3-DevKitC-1 + INMP441 麦克风的板级音频实现。
 *
 * 录音走 I2S0、播放走 I2S1，两个端口时钟相互独立；功放默认关断，
 * 仅在实际需要出声时使能，以免静默期底噪被放大。
 *
 * @copyright Copyright 2021 Espressif Systems (Shanghai) Co. Ltd.
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *               http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#include <string.h>
#include "bsp_board.h"
#include "driver/i2s_std.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// INMP441 为数字 MEMS 麦克风，只用 BCLK/WS/DATA 三线，无主时钟
#define I2S_WS_PIN GPIO_NUM_4  // 左右声道选择，麦克风固定输出左声道
#define I2S_SCK_PIN GPIO_NUM_5 // 位时钟
#define I2S_SD_PIN GPIO_NUM_6  // 串行数据

// MAX98357A 为数字功放，SD 引脚控制其输出使能
#define I2S_OUT_BCLK_PIN GPIO_NUM_15 // 位时钟
#define I2S_OUT_LRC_PIN GPIO_NUM_16  // 左右声道时钟
#define I2S_OUT_DIN_PIN GPIO_NUM_7   // 串行数据
#define I2S_OUT_SD_PIN GPIO_NUM_8    // 高电平使能功放，低电平关断

// I2S 配置参数
#define I2S_PORT_RX I2S_NUM_0 // 录音端口
#define I2S_PORT_TX I2S_NUM_1 // 播放端口
#define SAMPLE_RATE 16000     // 16kHz 满足语音识别模型的输入要求
#define BITS_PER_SAMPLE 16    // 每个采样点 16 位
#define CHANNELS 1            // 单声道

static const char *TAG = "bsp_board";

// 未初始化时保持为 nullptr
static i2s_chan_handle_t rx_handle = nullptr;
static i2s_chan_handle_t tx_handle = nullptr;
// 发送通道当前是否已使能，决定播放时是否需要重新给功放上电
static bool tx_channel_enabled = false;

/**
 * 初始化录音 I2S。INMP441 在 32 位帧内左对齐输出 24 位数据，只取左声道。
 *
 * @param sample_rate 采样率（Hz）
 * @param channel_format 当前未使用，录音固定为单声道
 * @param bits_per_chan 每个采样点的位数，32 以外的值按 16 位处理
 * @return ESP_OK 成功；其余为 I2S 驱动返回的错误码
 */
static esp_err_t bsp_i2s_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    esp_err_t ret = ESP_OK;

    // 麦克风只能作从设备，时钟由 ESP32-S3 提供
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_RX, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&chan_cfg, nullptr, &rx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "创建 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_data_bit_width_t bit_width = (bits_per_chan == 32) ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256},
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // INMP441 无主时钟引脚
            .bclk = I2S_SCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED, // 仅录音，不使用发送引脚
            .din = I2S_SD_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    // slot_mask 必须选在麦克风数据所在的槽位上
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    ret = i2s_channel_init_std_mode(rx_handle, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 I2S 标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启用 I2S 通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    // 上电初期缓冲区里是噪声样本，先丢弃若干帧，避免干扰唤醒词检测
    const size_t discard_samples = 8192; // 丢弃前 8KB
    uint8_t *discard_buffer = (uint8_t *)malloc(discard_samples);
    if (discard_buffer) {
        size_t bytes_read;
        for (int i = 0; i < 3; i++) {
            i2s_channel_read(rx_handle, discard_buffer, discard_samples, &bytes_read, pdMS_TO_TICKS(100));
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        free(discard_buffer);
        ESP_LOGD(TAG, "已清理 I2S 输入缓冲区初始数据");
    }

    ESP_LOGI(TAG, "I2S 初始化成功");
    return ESP_OK;
}

/**
 * 初始化录音通路，使用麦克风前必须调用。
 *
 * @param sample_rate 采样率（Hz），语音识别取 16000
 * @param channel_format 当前未使用，录音固定为单声道
 * @param bits_per_chan 每个采样点的位数，32 以外的值按 16 位处理
 * @return ESP_OK 成功；其余为 I2S 驱动返回的错误码
 */
esp_err_t bsp_board_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    ESP_LOGI(TAG, "初始化 ESP32-S3-DevKitC-1 + INMP441");
    ESP_LOGI(TAG, "音频参数: 采样率=%ldHz, 声道数=%d, 位深=%d位",
             sample_rate, channel_format, bits_per_chan);

    return bsp_i2s_init(sample_rate, channel_format, bits_per_chan);
}

/**
 * 阻塞读取一帧音频到 buffer，直到读满 buffer_len 字节。
 *
 * @param is_get_raw_channel true 时跳过电平处理，直接返回 I2S 原始数据
 * @param buffer 输出缓冲，按 int16 采样存放
 * @param buffer_len 期望读取的字节数
 * @return ESP_OK 成功；其余为 I2S 驱动返回的错误码
 */
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_read = 0;

    ret = i2s_channel_read(rx_handle, buffer, buffer_len, &bytes_read, portMAX_DELAY);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "读取 I2S 数据失败: %s", esp_err_to_name(ret));
        return ret;
    }

    if (bytes_read != buffer_len)
    {
        ESP_LOGW(TAG, "读取长度不符: 预期 %d 字节，实际 %d 字节", buffer_len, bytes_read);
    }

    // INMP441 在 32 位帧内左对齐输出 24 位数据，取高 16 位供语音识别使用
    if (!is_get_raw_channel)
    {
        int samples = buffer_len / sizeof(int16_t);

        // 不加增益：实测原始电平已能满足唤醒词检测；限幅用于将来开启增益后防止溢出
        for (int i = 0; i < samples; i++)
        {
            int32_t sample = static_cast<int32_t>(buffer[i]);

            // 灵敏度不足时可启用 2 倍增益，开启后依赖下方限幅避免削顶
            // sample = sample * 2;

            if (sample > 32767)
            {
                sample = 32767;
            }
            if (sample < -32768)
            {
                sample = -32768;
            }

            buffer[i] = static_cast<int16_t>(sample);
        }
    }

    return ESP_OK;
}

/**
 * 麦克风按单声道采集，与 I2S 槽位配置保持一致。
 *
 * @return 固定为 1
 */
int bsp_get_feed_channel(void)
{
    return CHANNELS;
}

/**
 * 初始化播放通路：配置功放使能引脚并建立 I2S 发送通道。
 *
 * @param sample_rate 采样率（Hz），须与待播放数据一致
 * @param channel_format 声道数，1 为单声道
 * @param bits_per_chan 每个采样点的位数，32 以外的值按 16 位处理
 * @return ESP_OK 成功；其余为 GPIO 或 I2S 驱动返回的错误码
 */
esp_err_t bsp_audio_init(uint32_t sample_rate, int channel_format, int bits_per_chan)
{
    esp_err_t ret = ESP_OK;

    // SD 为高时功放工作，为低时关断输出
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << I2S_OUT_SD_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(I2S_OUT_SD_PIN, 1); // 高电平启用功放
    ESP_LOGI(TAG, "MAX98357A SD 引脚已初始化（GPIO%d）", I2S_OUT_SD_PIN);

    // 功放作从设备，时钟由 ESP32-S3 提供
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT_TX, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&chan_cfg, &tx_handle, nullptr);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "创建 I2S 发送通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    i2s_data_bit_width_t bit_width = (bits_per_chan == 32) ? I2S_DATA_BIT_WIDTH_32BIT : I2S_DATA_BIT_WIDTH_16BIT;

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, (channel_format == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,   // MAX98357A 无主时钟引脚
            .bclk = I2S_OUT_BCLK_PIN,
            .ws = I2S_OUT_LRC_PIN,
            .dout = I2S_OUT_DIN_PIN,
            .din = I2S_GPIO_UNUSED,    // 仅播放，不使用接收引脚
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "初始化 I2S 发送标准模式失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "启用 I2S 发送通道失败: %s", esp_err_to_name(ret));
        return ret;
    }

    tx_channel_enabled = true;

    ESP_LOGI(TAG, "I2S 音频播放初始化成功");
    return ESP_OK;
}

/**
 * 播放一段完整音频，写完后调用 bsp_audio_stop() 关断功放，避免尾部噪声。
 *
 * @param audio_data PCM 数据指针
 * @param data_len 字节数
 * @return ESP_OK 全部写入完成；写入不完整返回 ESP_FAIL；参数或状态非法时返回对应错误码
 */
esp_err_t bsp_play_audio(const uint8_t *audio_data, size_t data_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_written = 0;
    size_t total_written = 0;

    if (tx_handle == nullptr)
    {
        ESP_LOGE(TAG, "I2S 发送通道未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_data == nullptr || data_len == 0)
    {
        ESP_LOGE(TAG, "无效的音频数据");
        return ESP_ERR_INVALID_ARG;
    }

    // 通道可能已被 bsp_audio_stop() 关过，重新使能前要先给功放上电
    if (!tx_channel_enabled)
    {
        gpio_set_level(I2S_OUT_SD_PIN, 1); // 高电平启用功放
        vTaskDelay(pdMS_TO_TICKS(10)); // 功放上电到输出稳定需要一定时间
        ESP_LOGD(TAG, "MAX98357A 功放已启用");
        
        ret = i2s_channel_enable(tx_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "启用 I2S 发送通道失败: %s", esp_err_to_name(ret));
            return ret;
        }
        tx_channel_enabled = true;
        ESP_LOGD(TAG, "I2S 发送通道已重新启用");
        
        // 先写一小段静音填充 DMA 缓冲，避免欠载时输出上电瞬间的杂音
        const size_t init_silence_size = 256; // 仅用于填充，无需更大
        static uint8_t init_silence[256] = {0}; // 静态分配，避免每次播放都申请内存
        size_t silence_written = 0;
        i2s_channel_write(tx_handle, init_silence, init_silence_size, &silence_written, pdMS_TO_TICKS(10));
    }

    // i2s_channel_write 可能短写，循环补齐剩余数据
    while (total_written < data_len)
    {
        size_t bytes_to_write = data_len - total_written;
        
        ret = i2s_channel_write(tx_handle, audio_data + total_written, bytes_to_write, &bytes_written, portMAX_DELAY);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "写入 I2S 音频数据失败: %s", esp_err_to_name(ret));
            break;
        }

        total_written += bytes_written;

        // 每 10KB 输出一次进度
        if ((total_written % 10240) < bytes_written)
        {
            ESP_LOGD(TAG, "音频播放进度: %zu/%zu 字节 (%.1f%%)",
                     total_written, data_len, (float)total_written * 100.0f / data_len);
        }
    }

    if (total_written != data_len)
    {
        ESP_LOGW(TAG, "音频数据写入不完整: 预期 %zu 字节，实际写入 %zu 字节", data_len, total_written);
        return ESP_FAIL;
    }

    // 停掉输出，否则功放会一直放大静默期的底噪
    esp_err_t stop_ret = bsp_audio_stop();
    if (stop_ret != ESP_OK)
    {
        ESP_LOGW(TAG, "停止音频输出时出现警告: %s", esp_err_to_name(stop_ret));
    }

    ESP_LOGI(TAG, "音频播放完成，播放了 %zu 字节", total_written);
    return ESP_OK;
}

/**
 * 与 bsp_play_audio 相同，但写完不停止 I2S，供连续播放多个音频块使用。
 *
 * @param audio_data PCM 数据指针
 * @param data_len 字节数
 * @return ESP_OK 全部写入完成；写入不完整返回 ESP_FAIL；参数或状态非法时返回对应错误码
 */
esp_err_t bsp_play_audio_stream(const uint8_t *audio_data, size_t data_len)
{
    esp_err_t ret = ESP_OK;
    size_t bytes_written = 0;
    size_t total_written = 0;

    if (tx_handle == nullptr)
    {
        ESP_LOGE(TAG, "I2S 发送通道未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    if (audio_data == nullptr || data_len == 0)
    {
        ESP_LOGE(TAG, "无效的音频数据");
        return ESP_ERR_INVALID_ARG;
    }

    // 通道可能已被 bsp_audio_stop() 关过，重新使能前要先给功放上电
    if (!tx_channel_enabled)
    {
        gpio_set_level(I2S_OUT_SD_PIN, 1); // 高电平启用功放
        vTaskDelay(pdMS_TO_TICKS(10)); // 功放上电到输出稳定需要一定时间
        ESP_LOGD(TAG, "MAX98357A 功放已启用");
        
        ret = i2s_channel_enable(tx_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "启用 I2S 发送通道失败: %s", esp_err_to_name(ret));
            return ret;
        }
        tx_channel_enabled = true;
        ESP_LOGD(TAG, "I2S 发送通道已重新启用");
        
        // 先写一小段静音填充 DMA 缓冲，避免欠载时输出上电瞬间的杂音
        const size_t init_silence_size = 256; // 仅用于填充，无需更大
        static uint8_t init_silence[256] = {0}; // 静态分配，避免每次播放都申请内存
        size_t silence_written = 0;
        i2s_channel_write(tx_handle, init_silence, init_silence_size, &silence_written, pdMS_TO_TICKS(10));
    }

    // i2s_channel_write 可能短写，循环补齐剩余数据
    while (total_written < data_len)
    {
        size_t bytes_to_write = data_len - total_written;
        
        ret = i2s_channel_write(tx_handle, audio_data + total_written, bytes_to_write, &bytes_written, portMAX_DELAY);

        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "写入 I2S 音频数据失败: %s", esp_err_to_name(ret));
            break;
        }

        total_written += bytes_written;

        // 每 10KB 输出一次进度
        if ((total_written % 10240) < bytes_written)
        {
            ESP_LOGD(TAG, "音频播放进度: %zu/%zu 字节 (%.1f%%)",
                     total_written, data_len, (float)total_written * 100.0f / data_len);
        }
    }

    if (total_written != data_len)
    {
        ESP_LOGW(TAG, "音频数据写入不完整: 预期 %zu 字节，实际写入 %zu 字节", data_len, total_written);
        return ESP_FAIL;
    }

    // 与 bsp_play_audio 的差别：不停止通道，由调用方决定何时结束流
    ESP_LOGD(TAG, "流式音频块播放完成，播放了 %zu 字节", total_written);
    return ESP_OK;
}

/**
 * 停止播放输出：清空缓冲区、关断功放并禁用 I2S 发送通道，消除底噪。
 *
 * @return ESP_OK 已停止或本就处于停止状态；禁用通道失败时返回驱动错误码
 */
esp_err_t bsp_audio_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (tx_handle == nullptr)
    {
        ESP_LOGW(TAG, "I2S 发送通道未初始化，无需停止");
        return ESP_OK;
    }

    if (tx_channel_enabled)
    {
        // 直接切断数据会在缓冲区留下残样产生爆音，先用静音填充
        const size_t silence_size = 4096; // 4KB 静音
        uint8_t *silence_buffer = (uint8_t *)calloc(silence_size, 1);
        if (silence_buffer) {
            size_t bytes_written = 0;
            i2s_channel_write(tx_handle, silence_buffer, silence_size, &bytes_written, pdMS_TO_TICKS(100));
            free(silence_buffer);
            ESP_LOGD(TAG, "已发送静音数据清空缓冲区");
        }
        
        // 等静音播完再关断功放
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // 先关功放再禁用通道，避免关断瞬间输出爆音
        gpio_set_level(I2S_OUT_SD_PIN, 0); // 低电平关闭功放
        ESP_LOGD(TAG, "MAX98357A 功放已关闭");
        vTaskDelay(pdMS_TO_TICKS(10)); // 等待功放完全关断
        
        ret = i2s_channel_disable(tx_handle);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "禁用 I2S 发送通道失败: %s", esp_err_to_name(ret));
            return ret;
        }
        tx_channel_enabled = false;
        ESP_LOGI(TAG, "I2S 音频输出已停止");
    }
    else
    {
        ESP_LOGD(TAG, "I2S 发送通道已处于禁用状态");
    }

    return ESP_OK;
}
