# 外接 LED 控制
idf_component_register(SRCS "led_controller.cc"
                       INCLUDE_DIRS "include"
                       REQUIRES driver)
