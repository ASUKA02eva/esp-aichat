# ESP-AI 智能语音助手 —— 顶层工程文件
#
# 整个仓库只有一个可执行固件：
#   main/              统一状态机（唤醒词 → 本地命令词 → 大模型对话）
#   components/        可复用组件（BSP / 音频 / 网络 / LED / 舵机 / 音频素材）
#   server/            电脑端 Python 大模型服务端
#   assets/            音频素材源文件（mp3）
#   tools/             音频转 C 头文件等工具

cmake_minimum_required(VERSION 3.16.0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp-aichat)
