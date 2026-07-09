# shim/ is listed as this component's include dir so the verbatim-shared
# headers resolve <Arduino.h> / "wio_platform.h" / "transport.h" here
# (include-path precedence keeps their text identical to the Wio originals).
idf_component_register(SRCS "app_main.cpp"
                       INCLUDE_DIRS "shim"
                       REQUIRES buddy_core net_bridge_core arduinojson esp_timer)

# The shared Wio headers use a terse style (two statements per line) that
# trips -Werror=misleading-indentation under IDF's -Wall; they are verbatim
# copies we must not restyle, so silence that warning for this TU only.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-misleading-indentation)
