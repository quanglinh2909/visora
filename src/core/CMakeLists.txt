# libcore — pure types, geometry and logging.
#
# It links nothing. That is the point: if anything in core ever reaches for
# GStreamer, librga or oatpp, the build breaks here rather than in review.
add_library(visora_core STATIC
    Geometry.cpp
    Image.cpp
    ImageMath.cpp
    Json.cpp
    Log.cpp
    Result.cpp
    Time.cpp
)
add_library(visora::core ALIAS visora_core)

target_include_directories(visora_core PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_compile_features(visora_core PUBLIC cxx_std_20)
