/**
 * @file button_controller.h
 * @brief 外接按键控制器（由原来的 control-led 示例抽象而来）
 *
 * 本工程里按键的作用是“按键唤醒”，效果等同于说唤醒词。
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
     * @param pin         按键引脚（默认 GPIO41）
     * @param active_high true = 按下时为高电平（按键另一端接 3V3，使用内部下拉）
     *                    false = 按下时为低电平（按键另一端接 GND，使用内部上拉）
     */
    explicit ButtonController(gpio_num_t pin = GPIO_NUM_41, bool active_high = true);

    esp_err_t init();

    /** 当前是否处于按下状态 */
    bool isPressed() const;

    /**
     * @brief 是否“刚刚发生了一次按下”（带上电平和时间去抖）
     *
     * 主循环里每帧调用一次，返回 true 表示这一次是新的按下动作。
     */
    bool consume_press();

private:
    gpio_num_t pin_;
    bool active_high_;
    bool last_pressed_;
    int64_t last_press_us_;
    bool initialized_;
};
