# SenseCAP Indicator port of the Claude Buddy (ESP32-S3, ESP-IDF v5.1.x —
# the version Seeed's SenseCAP_Indicator_ESP32 BSP is pinned to; do not
# float). Build:  idf.py set-target esp32s3 && idf.py build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The vendor BSP repo (submodule) is not itself a component; its components/
# tree (bsp, lvgl, iot_button, i2c bus/devices, ...) is added here. LoRaWAN
# is excluded exactly as the vendor's own examples do.
set(EXTRA_COMPONENT_DIRS components/sensecap_bsp/components)
set(EXCLUDE_COMPONENTS LoRaWAN)

# Vendor components don't build warning-clean; silence warnings globally the
# same way the vendor examples do (our shared headers still get their strict
# treatment in the Wio Arduino build and the native tests).
add_compile_options(-fdiagnostics-color=always -w)

project(claude_buddy_indicator)
