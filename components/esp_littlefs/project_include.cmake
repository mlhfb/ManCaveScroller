#
# littlefs_create_partition_image
#
# Create a LittleFS image from a host directory and optionally include it in
# the default flash target.

function(littlefs_create_partition_image partition base_dir)
    set(options FLASH_IN_PROJECT)
    set(multi DEPENDS)
    cmake_parse_arguments(arg "${options}" "" "${multi}" "${ARGN}")

    get_filename_component(base_dir_full_path "${base_dir}" ABSOLUTE)
    partition_table_get_partition_info(size "--partition-name ${partition}" "size")
    partition_table_get_partition_info(offset "--partition-name ${partition}" "offset")

    if(NOT "${size}" OR NOT "${offset}")
        set(message "Failed to create littlefs image for partition '${partition}'. "
                    "Check project configuration if using the correct partition table file.")
        fail_at_build_time(littlefs_${partition}_bin "${message}")
        return()
    endif()

    set(image_file "${CMAKE_BINARY_DIR}/${partition}.bin")

    find_program(MKLITTLEFS_TOOL NAMES mklittlefs mklittlefs.exe
        PATHS "$ENV{USERPROFILE}/.platformio/packages/tool-mklittlefs"
        NO_DEFAULT_PATH)
    if(NOT MKLITTLEFS_TOOL)
        message(FATAL_ERROR "mklittlefs tool not found at PlatformIO tool path")
    endif()

    add_custom_target(littlefs_${partition}_bin ALL
        COMMAND "${MKLITTLEFS_TOOL}" -c "${base_dir_full_path}" -b 4096 -p 256 -s "${size}" "${image_file}"
        DEPENDS ${arg_DEPENDS}
    )

    set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY
        ADDITIONAL_MAKE_CLEAN_FILES "${image_file}")

    idf_component_get_property(main_args esptool_py FLASH_ARGS)
    idf_component_get_property(sub_args esptool_py FLASH_SUB_ARGS)
    esptool_py_flash_target(${partition}-flash "${main_args}" "${sub_args}")
    esptool_py_flash_target_image(${partition}-flash "${partition}" "${offset}" "${image_file}")
    add_dependencies(${partition}-flash littlefs_${partition}_bin)

    if(arg_FLASH_IN_PROJECT)
        esptool_py_flash_target_image(flash "${partition}" "${offset}" "${image_file}")
        add_dependencies(flash littlefs_${partition}_bin)
    endif()
endfunction()
