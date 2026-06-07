#include "RemoteJson.hpp"

namespace basalt::proxy::remote::json {

std::string stringField(std::string_view body, std::string_view key, bool& wellFormed) {
    wellFormed = true;
    if (body.empty())
        return {};
    const auto parsed = wire::common::parseJson(body);
    const auto* object = parsed ? parsed->get<JsonObject>() : nullptr;
    if (object == nullptr) {
        wellFormed = false;
        return {};
    }
    const JsonValue* value = object->find(key);
    if (value == nullptr)
        return {};
    const auto* text = value->get<std::string>();
    if (text == nullptr) {
        wellFormed = false;
        return {};
    }
    return *text;
}

} // namespace basalt::proxy::remote::json
