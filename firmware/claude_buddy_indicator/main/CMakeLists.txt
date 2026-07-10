# shim/ is listed as this component's include dir so the verbatim-shared
# headers resolve <Arduino.h> / "wio_platform.h" / "transport.h" here
# (include-path precedence keeps their text identical to the Wio originals).
#
# The shared buddy engine (buddy.cpp + 18 species) compiles as part of this
# component: the sources live in components/buddy_core (diff-guarded copies)
# but need the shim include dirs, which are private to main.
file(GLOB BUDDY_ENGINE_SRCS
     ${CMAKE_CURRENT_LIST_DIR}/../components/buddy_core/buddy.cpp
     ${CMAKE_CURRENT_LIST_DIR}/../components/buddy_core/buddy_sp_*.cpp)

idf_component_register(SRCS "app_main.cpp" "net_bridge_shell.c" "lv_port.c"
                            "buddy_canvas.cpp" "wifi_provision.c" ${BUDDY_ENGINE_SRCS}
                       INCLUDE_DIRS "shim" "."
                       REQUIRES buddy_core net_bridge_core arduinojson
                                esp_timer esp_wifi esp_event esp_netif
                                nvs_flash mqtt bsp lvgl)

# The shared Wio headers use a terse style (two statements per line) that
# trips -Werror=misleading-indentation under IDF's -Wall; they are verbatim
# copies we must not restyle, so silence that warning for this TU only.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-misleading-indentation)
