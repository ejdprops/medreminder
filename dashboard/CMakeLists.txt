# MedReminder dashboard: CrowPanel 7" (ESP32-S3, 800x480 RGB, GT911 touch), fed by a XIAO C6 node
# over one wire (firmware/main/uart_link.c). Plain ESP-IDF, no Matter. Build with ./build.sh.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(medreminder_dash)
