# ================================================================
# ESP32-S3-BOX（正点原子 AI BOX1）bringup 工程 —— 顶层 CMake
#
# 目标芯片：ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB Octal PSRAM）
# IDF 版本：v5.2.1（E:\ESP-IDF\Espressif\frameworks\esp-idf-v5.2.1）
# ================================================================
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(box_bringup)
