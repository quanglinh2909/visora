#include "store/AiJobJson.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"

#include "core/Log.hpp"

namespace visora::store {
namespace {

constexpr const char* kCategory = "store";

#include OATPP_CODEGEN_BEGIN(DTO)

// The stored shape, field for field. Named for the column rather than for the
// domain type, because that is what it describes.
class StoredStage : public oatpp::DTO {
    DTO_INIT(StoredStage, DTO)

    DTO_FIELD(oatpp::Int32, parent);
    DTO_FIELD(oatpp::String, modelPath);
    DTO_FIELD(oatpp::String, modelType);
    DTO_FIELD(oatpp::String, transform);
    DTO_FIELD(oatpp::String, inputClasses);
    DTO_FIELD(oatpp::String, classFilter);
    DTO_FIELD(oatpp::Float64, conf);
};

#include OATPP_CODEGEN_END(DTO)

using StoredStages = oatpp::List<oatpp::Object<StoredStage>>;

// One mapper, built once. Null fields are dropped so a row stays small and
// reads the way a hand-written one would; the reader treats absent and null
// identically anyway.
oatpp::parser::json::mapping::ObjectMapper& mapper() {
    static oatpp::parser::json::mapping::ObjectMapper instance = [] {
        oatpp::parser::json::mapping::ObjectMapper made;
        // Absent rather than null: a row stays small and reads the way a
        // hand-written one would. The reader treats the two identically.
        made.getSerializer()->getConfig()->includeNullFields = false;
        return made;
    }();
    return instance;
}

std::string text(const oatpp::String& value) { return value ? std::string(value->c_str()) : std::string(); }

}  // namespace

std::vector<int> parseClassList(const std::string& value) {
    std::vector<int> out;
    if (value.empty() || value == "all") return out;

    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string token =
            value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!token.empty()) {
            char* end = nullptr;
            const long parsed = std::strtol(token.c_str(), &end, 10);
            // A token that is not a number is skipped, not fatal: one typo in
            // one field must not make a job unreadable.
            if (end != token.c_str()) out.push_back(static_cast<int>(parsed));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string formatClassList(const std::vector<int>& classes) {
    std::string out;
    for (const int value : classes) {
        if (!out.empty()) out += ',';
        out += std::to_string(value);
    }
    return out;
}

std::string stagesToJson(const std::vector<vision::AiStage>& stages) {
    auto list = StoredStages::createShared();
    for (const vision::AiStage& stage : stages) {
        auto stored = StoredStage::createShared();
        stored->parent = stage.parent;
        stored->modelPath = stage.modelPath.c_str();
        stored->modelType = stage.modelType.c_str();
        stored->transform = stage.transform.c_str();
        stored->inputClasses = formatClassList(stage.inputClasses).c_str();
        stored->classFilter = formatClassList(stage.classFilter).c_str();
        stored->conf = static_cast<double>(stage.confidence);
        list->push_back(stored);
    }
    try {
        return text(mapper().writeToString(list));
    } catch (const std::exception& error) {
        VS_WARN(kCategory) << "could not write the stage tree: " << error.what();
        return "[]";
    }
}

std::vector<vision::AiStage> stagesFromJson(const std::string& json) {
    std::vector<vision::AiStage> out;
    if (json.empty()) return out;

    StoredStages list;
    try {
        list = mapper().readFromString<StoredStages>(oatpp::String(json.c_str()));
    } catch (const std::exception& error) {
        VS_WARN(kCategory) << "unreadable stage tree, the job will not start: " << error.what();
        return out;
    }
    if (!list) return out;

    int index = 0;
    for (const auto& stored : *list) {
        if (!stored) {
            ++index;
            continue;
        }
        vision::AiStage stage;
        stage.modelPath = text(stored->modelPath);
        stage.modelType = text(stored->modelType);
        stage.transform = text(stored->transform);
        stage.inputClasses = parseClassList(text(stored->inputClasses));
        stage.classFilter = parseClassList(text(stored->classFilter));
        // Compared against nullptr, never `if (stored->conf)`: oatpp's wrappers
        // have an operator bool that returns the VALUE, so `parent = 0` read
        // that way becomes "not supplied" and the stage silently attaches to
        // the wrong parent.
        stage.confidence = stored->conf.getPtr() != nullptr
                               ? static_cast<float>(*stored->conf)
                               : 0.25f;
        // No parent written means a straight chain: stage 0 on the frame, stage
        // i on stage i-1. That is what the predecessor did, and rows it wrote
        // rely on it.
        stage.parent = stored->parent.getPtr() != nullptr ? *stored->parent
                                                          : (index == 0 ? -1 : index - 1);
        out.push_back(std::move(stage));
        ++index;
    }
    return out;
}

}  // namespace visora::store
