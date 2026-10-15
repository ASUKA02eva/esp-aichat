/**
 * @file button_controller.cc
 * @brief 外接按键控制器实现
 */

#include "button_controller.h"

extern "C"
{
#include "esp_log.h"
#include "esp_timer.h"
}

static const char *TAG = "Button";

// 两次按下之间的最小间隔，用于软件去抖（微秒）
static const int64_t PRESS_DEBOUNCE_US = 50000; // 50ms

ButtonController::ButtonController(gpio_num_t pin, bool active_high)
    : pin_(pin), active_high_(active_high), last_pressed_(false),
      last_press_us_(0), initialized_(false)
{
}

esp_err_t ButtonController::init()
{
    ESP_LOGI(TAG, "初始化按键 (GPIO%d, 按下为%s电平)...",
             pin_, active_high_ ? "高" : "低");

    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin_);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = active_high_ ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = active_high_ ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "按键 GPIO 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }

    initialized_ = true;
    ESP_LOGI(TAG, "✓ 按键初始化成功 (GPIO%d)", pin_);
    return ESP_OK;
}

bool ButtonController::isPressed() const
{
    if (!initialized_)
    {
        return false;
    }
    bool level_high = (gpio_get_level(pin_) != 0);
    return active_high_ ? level_high : !level_high;
}

bool ButtonController::consume_press()
{
    if (!initialized_)
    {
        return false;
    }

    const bool pressed = isPressed();
    const int64_t now = esp_timer_get_time();

    bool triggered = false;
    if (pressed && !last_pressed_ && (now - last_press_us_) > PRESS_DEBOUNCE_US)
    {
        triggered = true;
        last_press_us_ = now;
    }
    last_pressed_ = pressed;

    if (triggered)
    {
        ESP_LOGI(TAG, "🖲️ 检测到按键按下");
    }
    return triggered;
}
