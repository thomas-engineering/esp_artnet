# Optional DMX debug / test features (see debug/dmx_debug.h).
# Included from main/CMakeLists.txt after idf_component_register().
#
#   idf.py -DDMX_DEBUG_TEST_PATTERN=1 build   channels 1-8 forced to 255
#   idf.py -DDMX_DEBUG_REPLAY=1       build   Arduino DMX_Master fade replay
#   idf.py -DDMX_DEBUG_READBACK=1     build   UART2 decodes our own TX line
#   idf.py -DDMX_DEBUG_SNIFF=1        build   listen-only bus sniffer
#
# The -D values are cached in build/; run `idf.py fullclean` afterwards.

set(_dmx_debug_srcs "")
foreach(_opt DMX_DEBUG_TEST_PATTERN DMX_DEBUG_REPLAY DMX_DEBUG_READBACK DMX_DEBUG_SNIFF)
    if(${_opt})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_opt})
        set(_dmx_debug_srcs "debug/dmx_debug.c")
    endif()
endforeach()
if(DMX_DEBUG_READBACK)
    list(APPEND _dmx_debug_srcs "debug/dmx_selftest.c")
endif()
if(DMX_DEBUG_SNIFF)
    list(APPEND _dmx_debug_srcs "debug/dmx_sniff.c")
endif()
if(_dmx_debug_srcs)
    target_sources(${COMPONENT_LIB} PRIVATE ${_dmx_debug_srcs})
endif()
