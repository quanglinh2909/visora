#include "api/mappers/StreamStatusMapper.hpp"

namespace visora::api {

oatpp::Object<StreamStatusDto> toDto(const media::CameraRuntimeStatus& status) {
    auto dto = StreamStatusDto::createShared();
    dto->id = status.id;
    dto->name = status.name;
    dto->state = media::toString(status.state);
    dto->inputRtsp = status.inputRtsp;
    dto->outputRtsp = status.outputRtsp;
    dto->codec = media::toString(status.codec);
    dto->hardware = status.hardware;
    dto->recordingEnabled = status.recordingEnabled;
    dto->retryCount = static_cast<v_uint32>(status.retryCount < 0 ? 0 : status.retryCount);
    dto->lastError = status.lastError;
    dto->lastChangedAt = status.lastChangedAt;
    dto->streaming = status.streaming;
    return dto;
}

oatpp::List<oatpp::Object<StreamStatusDto>> toDtoList(
    const std::vector<media::CameraRuntimeStatus>& statuses) {
    auto list = oatpp::List<oatpp::Object<StreamStatusDto>>::createShared();
    for (const auto& status : statuses) list->push_back(toDto(status));
    return list;
}

}  // namespace visora::api
