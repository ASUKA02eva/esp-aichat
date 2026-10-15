# 外接按键（用于按键唤醒）
idf_component_register(SRCS "button_controller.cc"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer)
