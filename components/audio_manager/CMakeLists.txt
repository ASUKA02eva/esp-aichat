# 音频管理器：录音缓冲区、响应播放、流式播放
idf_component_register(SRCS "audio_manager.cc"
                       INCLUDE_DIRS "include"
                       REQUIRES bsp_board)
