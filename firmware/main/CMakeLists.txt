idf_component_register(SRC_DIRS          "." "icons" "fonts"
                       PRIV_INCLUDE_DIRS  "." "${ESP_MATTER_PATH}/examples/common/utils")

if (CONFIG_ENABLE_SET_CERT_DECLARATION_API)
    target_add_binary_data(${COMPONENT_TARGET} "certification_declaration/certification_declaration.der" BINARY)
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

# Firmware version info: build_info.h (git describe, regenerated every build) and the Matter
# SoftwareVersion number. Release procedure: CLAUDE.md "Firmware versions".
set(BUILD_INFO_H ${CMAKE_CURRENT_BINARY_DIR}/build_info.h)
add_custom_target(medreminder_build_info ALL
    COMMAND ${CMAKE_COMMAND} -DREPO=${CMAKE_CURRENT_LIST_DIR}/.. -DOUT=${BUILD_INFO_H}
            -P ${CMAKE_CURRENT_LIST_DIR}/../tools/build_info.cmake
    BYPRODUCTS ${BUILD_INFO_H})
add_dependencies(${COMPONENT_LIB} medreminder_build_info)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_VERSION_NUMBER=${PROJECT_VER_NUMBER})
