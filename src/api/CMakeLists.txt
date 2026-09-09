# libapi — HTTP. The only layer that includes oatpp.
#
# Controllers are adapters: parse, delegate to a domain service, map the result.
# No business logic, no SQL. That is what keeps the rules testable without a
# server and the storage swappable without touching the rules.
add_library(visora_api STATIC
    ByteRange.cpp
    Config.cpp
    HttpError.cpp
    mappers/AiMapper.cpp
    mappers/CameraMapper.cpp
    mappers/RecordingMapper.cpp
    mappers/StreamStatusMapper.cpp
    ws/CameraStateFeed.cpp
    ws/MotionEventFeed.cpp
)
add_library(visora::api ALIAS visora_api)

target_link_libraries(visora_api PUBLIC
    visora::media
    visora::store
    oatpp::oatpp
    oatpp::oatpp-swagger
    oatpp::oatpp-websocket
)
