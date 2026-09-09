add_executable(visora main.cpp)
target_link_libraries(visora PRIVATE
    visora::api
    visora::hal
    visora::media
    visora::store
    visora::backends
)

# Swagger UI's static files ship with oatpp-swagger; the path differs across
# versions and vcpkg layouts, so it is resolved at configure time rather than
# guessed.
find_path(VISORA_SWAGGER_RES_DIR
    NAMES favicon-16x16.png
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
    PATH_SUFFIXES
        oatpp-1.3.0/bin/oatpp-swagger/res
        oatpp/bin/oatpp-swagger/res
        share/oatpp-swagger/bin/oatpp-swagger/res
    NO_DEFAULT_PATH
)
if(VISORA_SWAGGER_RES_DIR)
    target_compile_definitions(visora PRIVATE OATPP_SWAGGER_RES_PATH="${VISORA_SWAGGER_RES_DIR}")
else()
    message(WARNING "Swagger UI resources not found; /swagger/ui will not render")
    target_compile_definitions(visora PRIVATE OATPP_SWAGGER_RES_PATH="")
endif()
