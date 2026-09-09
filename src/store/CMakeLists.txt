# libstore — persistence adapters for the ports the domain declares.
#
# The in-memory repository has no dependencies and is always built: it is what
# the service tests run against, and a usable single-node mode on a board with
# no PostgreSQL.
add_library(visora_store STATIC
    InMemoryAiJobRepository.cpp
    InMemoryCameraRepository.cpp
    InMemoryRecordingRepository.cpp
)
add_library(visora::store ALIAS visora_store)
target_link_libraries(visora_store PUBLIC visora::media visora::vision)

if(VISORA_WITH_API)
    target_sources(visora_store PRIVATE
        AiJobJson.cpp
        PostgresAiJobRepository.cpp
        PostgresCameraRepository.cpp
        PostgresRecordingRepository.cpp
    )
    # PUBLIC on purpose: the header exposes oatpp types, so anything that
    # constructs this adapter needs them. Nothing else in the domain does.
    target_link_libraries(visora_store PUBLIC oatpp::oatpp oatpp::oatpp-postgresql)
endif()
