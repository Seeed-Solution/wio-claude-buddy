# Byte-identical copies of the Wio firmware's shared headers — the wire
# protocol, pet stats, and preferences facade. scripts/check_shared_files.sh
# fails CI if these drift from firmware/claude_buddy/. Header-only: they
# compile inside main's translation unit, where main/shim/ provides
# Arduino.h / wio_platform.h / transport.h.
idf_component_register(INCLUDE_DIRS "." REQUIRES arduinojson)
