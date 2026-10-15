/**
 * @file led_controller.h
 * @brief 外接 LED 控制器（由原来的 control-led 示例抽象而来）
 *
 * 语音命令“帮我开灯 / 帮我关灯”最终作用在这个类上。
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
    /**
     * @param pin LED 所在引脚（默认 GPIO21）
     */
    explicit LightController(gpio_num_t pin = GPIO_NUM_21);

    /**
     * @brief 初始化 GPIO（输出模式，初始为熄灭）
     */
    esp_err_t init();

    void turnOn();
    void turnOff();
    void toggle();

    /**
     * @brief 闪烁若干次，结束后恢复闪烁前的状态
     *
     * 用它做“听到唤醒词了”的视觉反馈，又不会破坏“开灯/关灯”的语义。
     *
     * @param times          闪烁次数
     * @param half_period_ms 亮/灭各持续多久（毫秒）
     */
    void blink(int times, int half_period_ms = 120);

    bool isOn() const { return state_; }
    gpio_num_t pin() const { return pin_; }

private:
    void apply(bool on);

    gpio_num_t pin_;
    bool state_;
    bool initialized_;
};
