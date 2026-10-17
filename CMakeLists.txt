if(CONFIG_ZMK_STATUS_LEDS)
    target_include_directories(app PRIVATE include)
    target_sources(app PRIVATE src/status_leds.c)
    target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_STATUS_LEDS app PRIVATE
        src/behaviors/behavior_status_leds.c
    )
endif()
