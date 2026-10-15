/**
 * @file servo_controller.h
 * @brief ServoController：舵机 LEDC PWM 控制接口
 *
 * 信号引脚由构造函数指定（默认 GPIO18）；PWM 50Hz、13 位分辨率，脉宽 0.5~2.5ms 对应 0~180 度。
 */

#pragma once

extern "C" {
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_err.h"
}

class ServoController {
public:
    // 舵机PWM配置常量（信号引脚由构造函数指定，默认 GPIO18）
    static constexpr ledc_timer_t SERVO_LEDC_TIMER = LEDC_TIMER_0;        // 使用LEDC定时器0
    static constexpr ledc_channel_t SERVO_LEDC_CHANNEL = LEDC_CHANNEL_0;  // 使用LEDC通道0
    static constexpr ledc_mode_t SERVO_LEDC_MODE = LEDC_LOW_SPEED_MODE;   // 低速模式
    static constexpr uint32_t SERVO_PWM_FREQ = 50;                       // 舵机PWM频率50Hz
    static constexpr ledc_timer_bit_t SERVO_PWM_RESOLUTION = LEDC_TIMER_13_BIT; // 13位分辨率（8192级别）

    // 舵机PWM脉宽定义（微秒）
    static constexpr int SERVO_MIN_PULSE_WIDTH = 500;     // 0度对应的脉宽（0.5ms）
    static constexpr int SERVO_MAX_PULSE_WIDTH = 2500;    // 180度对应的脉宽（2.5ms）
    static constexpr int SERVO_CENTER_PULSE_WIDTH = 1000; // 45度对应的脉宽（1ms）

    // 角度范围常量
    static constexpr int MIN_ANGLE = 0;     // 最小角度
    static constexpr int MAX_ANGLE = 180;   // 最大角度
    static constexpr int CENTER_ANGLE = 90; // 中心角度

public:
    // gpio 为 PWM 信号引脚，默认 GPIO18
    explicit ServoController(gpio_num_t gpio = GPIO_NUM_18);

    ~ServoController();

    // 配置 LEDC 定时器和通道，并把舵机摆到中位；任一环节失败都返回错误码
    esp_err_t init();

    // angle 超出 0~180 会被钳位；未初始化时返回 ESP_ERR_INVALID_STATE
    esp_err_t setAngle(int angle);

    // 相对当前角度旋转：正数顺时针，负数逆时针；angle 为 0 时直接返回 ESP_OK
    esp_err_t rotate(int angle);

    int getCurrentAngle() const;

    // 等价于 setAngle(CENTER_ANGLE)
    esp_err_t resetToCenter();

    bool isInitialized() const;

private:
    // 输入角度先钳位到 0~180，再换算成 13 位分辨率下的 LEDC 占空比
    uint32_t calculateDuty(int angle) const;

    int constrainAngle(int angle) const;

private:
    gpio_num_t servo_gpio_;  // 舵机 PWM 信号引脚
    int current_angle_;      // 当前舵机角度
    bool initialized_;       // 初始化状态标志
    static const char* TAG;  // 日志标签
};
