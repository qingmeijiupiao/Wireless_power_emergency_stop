option(ESTOP_VALIDATE_SHARED_REMOTE "Compile migrated remote components without starting radio" OFF)
set(app_dependencies emergency_ui)
if(ESTOP_VALIDATE_SHARED_REMOTE)
    list(APPEND app_dependencies espnow_remote battery_level blackbox_service)
endif()
idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES ${app_dependencies}
)

set(SCRIPT_DIR "${CMAKE_SOURCE_DIR}/scripts")
add_custom_target(run_post_build ALL
    COMMAND python "${SCRIPT_DIR}/post_build.py"
    COMMENT "Generating merged firmware"
    VERBATIM
)
add_dependencies(run_post_build app bootloader)
