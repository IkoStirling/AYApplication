#pragma once

#include <AYSerializer.h>
#include <AYSerializer/JsonSerializer.h>

#include <string>
#include <string_view>

namespace ayt::app
{

/// Encode one game-owned value as a SaveGame JSON payload using AYSerializer.
/// Reflected types must be registered before this function is called.
template<typename T>
bool encodeSaveGamePayload(T& value, std::string& payloadJson,
                           std::string& error, bool prettyPrint = false)
{
    ayt::serializer::JsonSerializer serializer(true, prettyPrint);
    serializer.beginObject(nullptr);
    serializer.field("data", value);
    serializer.endObject();
    const auto serializerError = serializer.lastError();
    if (!serializerError.ok()) {
        error = serializerError.path.empty()
            ? serializerError.message
            : serializerError.path + ": " + serializerError.message;
        return false;
    }
    payloadJson = serializer.output();
    if (payloadJson.empty()) {
        error = "AYSerializer produced an empty SaveGame payload.";
        return false;
    }
    error.clear();
    return true;
}

/// Decode a SaveGame JSON payload produced by encodeSaveGamePayload.
template<typename T>
bool decodeSaveGamePayload(std::string_view payloadJson, T& value,
                           std::string& error)
{
    ayt::serializer::JsonSerializer serializer(false);
    serializer.deserialize(std::string(payloadJson));
    if (!serializer.lastError().ok()) {
        const auto serializerError = serializer.lastError();
        error = serializerError.path.empty()
            ? serializerError.message
            : serializerError.path + ": " + serializerError.message;
        return false;
    }
    serializer.beginObject(nullptr);
    serializer.field("data", value);
    serializer.endObject();
    const auto serializerError = serializer.lastError();
    if (!serializerError.ok()) {
        error = serializerError.path.empty()
            ? serializerError.message
            : serializerError.path + ": " + serializerError.message;
        return false;
    }
    error.clear();
    return true;
}

} // namespace ayt::app
