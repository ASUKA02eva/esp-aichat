# SG90 舵机控制（LEDC PWM）
idf_component_register(SRCS "servo_controller.cc"
                       INCLUDE_DIRS "include"
                       REQUIRES driver)
