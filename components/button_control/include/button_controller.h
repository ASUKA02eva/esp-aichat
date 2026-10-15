/**
 * @file button_controller.h
 * @brief 外接按键控制器，按键唤醒等同于说唤醒词。
 */

#pragma once

#include <stdint.h>

extern "C"
{
#include "driver/gpio.h"
#include "esp_err.h"
}

class ButtonController
{
public:
    /**
     * @param pin         按键引脚
     * @param active_high true = 按下为高电平（按键另一端接 3V3，使用内部下拉）
     *                    false = 按下为低电平（按键另一端接 GND，使用内部上拉）
     */
    explicit ButtonController(gpio_num_t pin = GPIO_NUM_41, bool active_high = true);

    esp_err_t init();

    bool isPressed() const;

    // 每帧调用一次；返回 true 表示发生了一次新的按下（含电平判定与时间去抖）
    bool consume_press();

private:
    gpio_num_t pin_;
    bool active_high_;
    bool last_pressed_;
    int64_t last_press_us_;
    bool initialized_;
};
