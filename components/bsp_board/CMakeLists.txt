# 板级支持包：INMP441 数字麦克风 + MAX98357A 数字功放（I2S）
idf_component_register(SRCS "bsp_board.cc"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_driver_i2s)
