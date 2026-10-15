/**
 * @file led_controller.h
 * @brief 外接 LED 控制器，语音“开灯 / 关灯”命令的最终执行对象。
 */

#pragma once

extern "C"
{
#include "driver/gpio.h"
#include "esp_err.h"
}

class LightController
{
public:
    // pin：LED 所在引脚
    explicit LightController(gpio_num_t pin = GPIO_NUM_21);

    esp_err_t init();

    void turnOn();
    void turnOff();
    void toggle();

    // 闪烁 times 次后恢复闪烁前的状态，用于“已唤醒”的视觉反馈而不改变开灯 / 关灯语义。
    // half_period_ms：亮、灭各持续多少毫秒。
    void blink(int times, int half_period_ms = 120);

    bool isOn() const { return state_; }
    gpio_num_t pin() const { return pin_; }

private:
    void apply(bool on);

    gpio_num_t pin_;
    bool state_;
    bool initialized_;
};
