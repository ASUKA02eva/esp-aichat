/**
 * @file servo_controller.cc
 * @brief ServoController 实现：LEDC PWM 配置与角度换算
 *
 * setAngle() 先钳位角度再换算占空比；未初始化时直接返回 ESP_ERR_INVALID_STATE。
 */

#include "servo_controller.h"

const char* ServoController::TAG = "舵机控制器";

ServoController::ServoController(gpio_num_t gpio)
    : servo_gpio_(gpio), current_angle_(CENTER_ANGLE), initialized_(false) {
}

ServoController::~ServoController() {
}

esp_err_t ServoController::init() {
    ESP_LOGI(TAG, "初始化舵机 (GPIO%d)", servo_gpio_);

    ledc_timer_config_t ledc_timer = {
        .speed_mode = SERVO_LEDC_MODE,
        .duty_resolution = SERVO_PWM_RESOLUTION,
        .timer_num = SERVO_LEDC_TIMER,
        .freq_hz = SERVO_PWM_FREQ,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };
    
    esp_err_t ret = ledc_timer_config(&ledc_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC定时器配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ledc_channel_config_t ledc_channel = {
        .gpio_num = servo_gpio_,
        .speed_mode = SERVO_LEDC_MODE,
        .channel = SERVO_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = SERVO_LEDC_TIMER,
        .duty = 0, // 初始占空比为0
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD, // 默认模式：无输出时不关闭电源域
        .flags = {0}
    };
    
    ret = ledc_channel_config(&ledc_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LEDC通道配置失败: %s", esp_err_to_name(ret));
        return ret;
    }

    initialized_ = true;

    // 上电后先摆到中位，保证起始位置确定
    current_angle_ = CENTER_ANGLE;
    ret = setAngle(current_angle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "舵机初始角度设置失败");
        initialized_ = false; // 如果角度设置失败，重置初始化标志
        return ret;
    }

    ESP_LOGI(TAG, "舵机初始化成功");
    return ESP_OK;
}

esp_err_t ServoController::setAngle(int angle) {
    if (!initialized_) {
        ESP_LOGE(TAG, "舵机未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    angle = constrainAngle(angle);

    int pulse_width = SERVO_MIN_PULSE_WIDTH + 
                      (angle * (SERVO_MAX_PULSE_WIDTH - SERVO_MIN_PULSE_WIDTH)) / MAX_ANGLE;

    uint32_t duty = calculateDuty(angle);

    esp_err_t ret = ledc_set_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL, duty);
    if (ret == ESP_OK) {
        ledc_update_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL);
        current_angle_ = angle;
        ESP_LOGI(TAG, "舵机转动到 %d 度 (脉宽: %d us, 占空比: %lu)", 
                 angle, pulse_width, duty);
    } else {
        ESP_LOGE(TAG, "舵机角度设置失败: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t ServoController::rotate(int angle) {
    if (!initialized_) {
        ESP_LOGE(TAG, "舵机未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    int target_angle = current_angle_ + angle;

    target_angle = constrainAngle(target_angle);

    if (angle > 0) {
        ESP_LOGI(TAG, "舵机顺时针旋转%d度: %d° → %d°", angle, current_angle_, target_angle);
    } else if (angle < 0) {
        ESP_LOGI(TAG, "舵机逆时针旋转%d度: %d° → %d°", -angle, current_angle_, target_angle);
    } else {
        ESP_LOGI(TAG, "舵机保持当前位置: %d°", current_angle_);
        return ESP_OK;
    }

    return setAngle(target_angle);
}

int ServoController::getCurrentAngle() const {
    return current_angle_;
}

esp_err_t ServoController::resetToCenter() {
    ESP_LOGI(TAG, "重置舵机到中心位置 (%d度)", CENTER_ANGLE);
    return setAngle(CENTER_ANGLE);
}

bool ServoController::isInitialized() const {
    return initialized_;
}

uint32_t ServoController::calculateDuty(int angle) const {
    int pulse_width = SERVO_MIN_PULSE_WIDTH + 
                      (angle * (SERVO_MAX_PULSE_WIDTH - SERVO_MIN_PULSE_WIDTH)) / MAX_ANGLE;

    // 周期 20ms（50Hz），13 位分辨率满量程 8191，占空比按脉宽比例换算
    uint32_t duty = (pulse_width * ((1 << SERVO_PWM_RESOLUTION) - 1)) / 20000;
    
    return duty;
}

int ServoController::constrainAngle(int angle) const {
    if (angle < MIN_ANGLE) {
        return MIN_ANGLE;
    }
    if (angle > MAX_ANGLE) {
        return MAX_ANGLE;
    }
    return angle;
}
