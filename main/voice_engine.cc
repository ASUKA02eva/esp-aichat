/**
 * @file voice_engine.cc
 * @brief 语音识别引擎实现（基于 ESP-SR）
 */

#include "voice_engine.h"

#include <stdlib.h>
#include <string.h>

extern "C"
{
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wn_iface.h"           // 唤醒词检测接口
#include "esp_wn_models.h"          // 唤醒词模型管理
#include "esp_mn_iface.h"           // 命令词识别接口
#include "esp_mn_models.h"          // 命令词模型管理
#include "esp_mn_speech_commands.h" // 命令词配置
#include "esp_process_sdkconfig.h"  // 从 sdkconfig 加载命令词
#include "esp_nsn_iface.h"          // 噪声抑制接口
#include "esp_nsn_models.h"         // 噪声抑制模型管理
#include "model_path.h"             // 模型路径
}

static const char *TAG = "VoiceEngine";

namespace
{
/** 本地命令词表：拼音 + 中文说明，命令 ID 见 VoiceCommandId */
const VoiceEngine::Command kCommands[] = {
    {VOICE_CMD_TURN_ON_LIGHT, "bang wo kai deng", "帮我开灯"},
    {VOICE_CMD_TURN_OFF_LIGHT, "bang wo guan deng", "帮我关灯"},
    {VOICE_CMD_BYE_BYE, "bai bai", "拜拜"},
    {VOICE_CMD_CUSTOM, "xian zai an quan wu qing kuang ru he", "现在安全屋情况如何"},
};
const int kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);
} // namespace

VoiceEngine::VoiceEngine()
    : models_(nullptr), wakenet_(nullptr), wake_data_(nullptr),
      multinet_(nullptr), mn_data_(nullptr), nsn_(nullptr), nsn_data_(nullptr),
      nsn_out_(nullptr), nsn_chunk_(0), frame_bytes_(0),
      wake_model_(nullptr), command_model_(nullptr)
{
}

VoiceEngine::~VoiceEngine()
{
    if (nsn_out_ != nullptr)
    {
        free(nsn_out_);
        nsn_out_ = nullptr;
    }
}

esp_err_t VoiceEngine::init()
{
    ESP_LOGI(TAG, "加载语音识别模型...");

    // 内存自检：语音模型加载至少需要 100KB 可用内存
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "可用内存: 总计 %zu KB / 内部 %zu KB / PSRAM %zu KB",
             free_heap / 1024,
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    if (free_heap < 100 * 1024)
    {
        ESP_LOGE(TAG, "可用内存不足（至少需要 100KB）");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = load_models();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = load_wake_model();
    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = load_command_model();
    if (ret != ESP_OK)
    {
        return ret;
    }

    load_noise_suppression(); // 可选能力，失败不影响主流程

    ESP_LOGI(TAG, "✓ 语音引擎就绪（唤醒词: %s / 命令词: %s / 降噪: %s）",
             wake_model_, command_model_,
             noise_suppression_enabled() ? "开" : "关");
    return ESP_OK;
}

esp_err_t VoiceEngine::load_models()
{
    // 模型分区里的模型列表，加载失败时重试
    for (int retry = 0; retry < 3 && models_ == nullptr; retry++)
    {
        if (retry > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        models_ = esp_srmodel_init("model");
        if (models_ == nullptr)
        {
            ESP_LOGW(TAG, "模型列表加载失败，第 %d 次重试...", retry + 1);
        }
    }

    if (models_ == nullptr)
    {
        ESP_LOGE(TAG, "模型加载失败：请确认语音模型已烧录到 model 分区");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t VoiceEngine::load_wake_model()
{
    srmodel_list_t *models = (srmodel_list_t *)models_;

    char *name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (name == nullptr)
    {
        ESP_LOGE(TAG, "没有找到唤醒词模型（menuconfig → ESP Speech Recognition → Load Multiple Wake Words）");
        return ESP_ERR_NOT_FOUND;
    }
    wake_model_ = name;

    esp_wn_iface_t *wakenet = (esp_wn_iface_t *)esp_wn_handle_from_name(name);
    if (wakenet == nullptr)
    {
        ESP_LOGE(TAG, "获取唤醒词接口失败: %s", name);
        return ESP_FAIL;
    }
    wakenet_ = wakenet;

    // DET_MODE_90：90% 置信度阈值，兼顾识别率与误触发
    model_iface_data_t *data = wakenet->create(name, DET_MODE_90);
    if (data == nullptr)
    {
        ESP_LOGE(TAG, "创建唤醒词实例失败: %s", name);
        return ESP_FAIL;
    }
    wake_data_ = data;

    int chunk_samples = wakenet->get_samp_chunksize(data);
    frame_bytes_ = chunk_samples * sizeof(int16_t);
    ESP_LOGI(TAG, "✓ 唤醒词模型: %s（每帧 %d 样本 / %d 字节）", name, chunk_samples, frame_bytes_);
    return ESP_OK;
}

esp_err_t VoiceEngine::load_command_model()
{
    srmodel_list_t *models = (srmodel_list_t *)models_;

    char *name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (name == nullptr)
    {
        ESP_LOGE(TAG, "没有找到中文命令词模型（MultiNet7）");
        return ESP_ERR_NOT_FOUND;
    }
    command_model_ = name;

    esp_mn_iface_t *mn = (esp_mn_iface_t *)esp_mn_handle_from_name(name);
    if (mn == nullptr)
    {
        ESP_LOGE(TAG, "获取命令词接口失败: %s", name);
        return ESP_FAIL;
    }
    multinet_ = mn;

    // 6000ms：MultiNet 单轮识别的内部时长
    model_iface_data_t *data = mn->create(name, 6000);
    if (data == nullptr)
    {
        ESP_LOGE(TAG, "创建命令词实例失败: %s", name);
        return ESP_FAIL;
    }
    mn_data_ = data;

    ESP_LOGI(TAG, "✓ 命令词模型: %s", name);
    return configure_commands();
}

esp_err_t VoiceEngine::configure_commands()
{
    esp_mn_iface_t *mn = (esp_mn_iface_t *)multinet_;
    model_iface_data_t *data = (model_iface_data_t *)mn_data_;

    // 先按 sdkconfig 里的默认命令表初始化，再清空并只注册本工程用到的命令
    esp_mn_commands_update_from_sdkconfig(mn, data);
    esp_mn_commands_clear();

    esp_err_t ret = esp_mn_commands_alloc(mn, data);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "命令词管理结构分配失败: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    int ok_count = 0;
    int fail_count = 0;
    for (int i = 0; i < kCommandCount; i++)
    {
        const VoiceEngine::Command *cmd = &kCommands[i];
        ret = esp_mn_commands_add(cmd->id, cmd->pinyin);
        if (ret == ESP_OK)
        {
            ok_count++;
            ESP_LOGI(TAG, "  + 命令 %d: %s (%s)", cmd->id, cmd->description, cmd->pinyin);
        }
        else
        {
            fail_count++;
            ESP_LOGE(TAG, "  ✗ 命令 %d 添加失败: %s", cmd->id, esp_err_to_name(ret));
        }
    }

    esp_mn_error_t *error_phrases = esp_mn_commands_update();
    if (error_phrases != nullptr && error_phrases->num > 0)
    {
        ESP_LOGW(TAG, "有 %d 条命令更新失败", error_phrases->num);
    }

    ESP_LOGI(TAG, "命令词注册完成：成功 %d 条 / 失败 %d 条", ok_count, fail_count);
    mn->print_active_speech_commands(data);

    return (fail_count == 0) ? ESP_OK : ESP_FAIL;
}

void VoiceEngine::load_noise_suppression()
{
    srmodel_list_t *models = (srmodel_list_t *)models_;

    char *name = esp_srmodel_filter(models, ESP_NSNET_PREFIX, NULL);
    if (name == nullptr)
    {
        ESP_LOGW(TAG, "没有噪声抑制模型，跳过降噪");
        return;
    }

    esp_nsn_iface_t *nsn = (esp_nsn_iface_t *)esp_nsnet_handle_from_name(name);
    if (nsn == nullptr)
    {
        ESP_LOGW(TAG, "获取噪声抑制接口失败: %s", name);
        return;
    }

    esp_nsn_data_t *data = nsn->create(name);
    if (data == nullptr)
    {
        ESP_LOGW(TAG, "创建噪声抑制实例失败: %s", name);
        return;
    }

    nsn_ = nsn;
    nsn_data_ = data;
    nsn_chunk_ = nsn->get_samp_chunksize(data);
    nsn_out_ = (int16_t *)calloc(nsn_chunk_, sizeof(int16_t));
    if (nsn_out_ == nullptr)
    {
        ESP_LOGW(TAG, "降噪输出缓冲分配失败，禁用降噪");
        nsn_ = nullptr;
        nsn_data_ = nullptr;
        return;
    }

    ESP_LOGI(TAG, "✓ 噪声抑制模型: %s（每帧 %d 样本）", name, nsn_chunk_);
}

const int16_t *VoiceEngine::denoise(const int16_t *pcm, int samples)
{
    if (nsn_ == nullptr || nsn_data_ == nullptr || nsn_out_ == nullptr)
    {
        return pcm;
    }

    // 降噪模型的帧长必须与语音模型一致，否则不做处理（避免缓冲区越界）
    if (samples != nsn_chunk_)
    {
        return pcm;
    }

    esp_nsn_iface_t *nsn = (esp_nsn_iface_t *)nsn_;
    esp_nsn_data_t *data = (esp_nsn_data_t *)nsn_data_;
    nsn->process(data, (int16_t *)pcm, nsn_out_);
    return nsn_out_;
}

bool VoiceEngine::detect_wake(const int16_t *pcm)
{
    if (wakenet_ == nullptr || wake_data_ == nullptr)
    {
        return false;
    }
    esp_wn_iface_t *wakenet = (esp_wn_iface_t *)wakenet_;
    model_iface_data_t *data = (model_iface_data_t *)wake_data_;
    return wakenet->detect(data, (int16_t *)pcm) == WAKENET_DETECTED;
}

int VoiceEngine::detect_command(const int16_t *pcm)
{
    if (multinet_ == nullptr || mn_data_ == nullptr)
    {
        return VOICE_CMD_NONE;
    }

    esp_mn_iface_t *mn = (esp_mn_iface_t *)multinet_;
    model_iface_data_t *data = (model_iface_data_t *)mn_data_;

    esp_mn_state_t state = mn->detect(data, (int16_t *)pcm);
    if (state == ESP_MN_STATE_TIMEOUT)
    {
        // MultiNet 自己的识别窗口到期（本工程用统一的等待超时控制，这里忽略）
        ESP_LOGD(TAG, "命令词识别窗口超时");
        return VOICE_CMD_NONE;
    }
    if (state != ESP_MN_STATE_DETECTED)
    {
        return VOICE_CMD_NONE;
    }

    esp_mn_results_t *result = mn->get_results(data);
    if (result == nullptr || result->num <= 0)
    {
        return VOICE_CMD_NONE;
    }

    int command_id = result->command_id[0];
    ESP_LOGI(TAG, "🎯 命中命令: id=%d 置信度=%.2f 文本=\"%s\" (%s)",
             command_id, result->prob[0], result->string, describe(command_id));
    return command_id;
}

void VoiceEngine::reset_command()
{
    if (multinet_ == nullptr || mn_data_ == nullptr)
    {
        return;
    }
    esp_mn_iface_t *mn = (esp_mn_iface_t *)multinet_;
    mn->clean((model_iface_data_t *)mn_data_);
}

int VoiceEngine::command_count() const
{
    return kCommandCount;
}

const VoiceEngine::Command *VoiceEngine::commands() const
{
    return kCommands;
}

const char *VoiceEngine::describe(int command_id) const
{
    for (int i = 0; i < kCommandCount; i++)
    {
        if (kCommands[i].id == command_id)
        {
            return kCommands[i].description;
        }
    }
    return "未知命令";
}
