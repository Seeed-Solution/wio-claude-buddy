# SenseCAP Indicator port of the Claude Buddy (ESP32-S3, ESP-IDF v5.1.x —
# the version Seeed's SenseCAP_Indicator_ESP32 BSP is pinned to; do not
# float). Build:  idf.py set-target esp32s3 && idf.py build
#
# components/sensecap_bsp is the vendor BSP as a git submodule (pinned).
# It is not a component itself; Phase 2 wires its components/bsp + lvgl in
# via EXTRA_COMPONENT_DIRS when the display comes up.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(claude_buddy_indicator)
