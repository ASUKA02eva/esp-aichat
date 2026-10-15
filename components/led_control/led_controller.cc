/**
 * @file led_controller.cc
 * @brief 外接 LED 控制器实现
 */

#include "led_controller.h"

extern "C"
{
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

static const char *TAG = "LED";

LightController::LightController(gpio_num_t pin)
    : pin_(pin), state_(false), initialized_(false)
{
}

esp_err_t LightController::init()
{
    ESP_LOGI(TAG, "初始化外接 LED (GPIO%d)...", pin_);

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin_);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "LED GPIO 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    initialized_ = true;
    apply(false); // 初始熄灭
    ESP_LOGI(TAG, "✓ LED 初始化成功 (GPIO%d)，初始状态：熄灭", pin_);
    return ESP_OK;
}

void LightController::apply(bool on)
{
    if (!initialized_)
    {
        return;
    }
    gpio_set_level(pin_, on ? 1 : 0);
    state_ = on;
}

void LightController::turnOn()
{
    apply(true);
    ESP_LOGI(TAG, "💡 LED 点亮");
}

void LightController::turnOff()
{
    apply(false);
    ESP_LOGI(TAG, "🌑 LED 熄灭");
}

void LightController::toggle()
{
    apply(!state_);
}

void LightController::blink(int times, int half_period_ms)
{
    if (!initialized_ || times <= 0)
    {
        return;
    }

    const bool prev = state_;
    for (int i = 0; i < times; i++)
    {
        gpio_set_level(pin_, 1);
        vTaskDelay(pdMS_TO_TICKS(half_period_ms));
        gpio_set_level(pin_, 0);
        vTaskDelay(pdMS_TO_TICKS(half_period_ms));
    }
    gpio_set_level(pin_, prev ? 1 : 0); // 恢复闪烁前的状态
}
