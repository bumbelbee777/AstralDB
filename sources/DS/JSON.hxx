#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <utility>

namespace AstralDB {
namespace DS {

struct JSON;
using JSONObject = std::unordered_map<std::string, JSON>;
using JSONArray = std::vector<JSON>;

struct JSON {
    std::variant<std::monostate, std::string, double, bool, JSONArray, JSONObject> Value;

    JSON() : Value(std::monostate{}) {}
    JSON(std::nullptr_t) : Value(std::monostate{}) {}
    JSON(const std::string& Value) : Value(Value) {}
    JSON(std::string&& Value) : Value(std::move(Value)) {}
    JSON(const char* Value) : Value(std::string(Value)) {}
    JSON(int Value) : Value(static_cast<double>(Value)) {}
    JSON(double Value) : Value(Value) {}
    JSON(bool Value) : Value(Value) {}
    JSON(const JSONArray& Value) : Value(Value) {}
    JSON(JSONArray&& Value) : Value(std::move(Value)) {}
    JSON(const JSONObject& Value) : Value(Value) {}
    JSON(JSONObject&& Value) : Value(std::move(Value)) {}

    bool IsNull() const { return std::holds_alternative<std::monostate>(Value); }
    bool IsString() const { return std::holds_alternative<std::string>(Value); }
    bool IsNumber() const { return std::holds_alternative<double>(Value); }
    bool IsBool() const { return std::holds_alternative<bool>(Value); }
    bool IsArray() const { return std::holds_alternative<JSONArray>(Value); }
    bool IsObject() const { return std::holds_alternative<JSONObject>(Value); }

    const std::string& AsString() const { return std::get<std::string>(Value); }
    double AsNumber() const { return std::get<double>(Value); }
    bool AsBool() const { return std::get<bool>(Value); }
    const JSONArray& AsArray() const { return std::get<JSONArray>(Value); }
    const JSONObject& AsObject() const { return std::get<JSONObject>(Value); }

    JSON& operator[](const std::string& Key) {
        if (!IsObject()) Value = JSONObject{};
        return std::get<JSONObject>(Value)[Key];
    }

    const JSON& operator[](const std::string& Key) const {
        static const JSON NullValue;
        if (!IsObject()) return NullValue;
        const auto& Obj = std::get<JSONObject>(Value);
        auto It = Obj.find(Key);
        return It != Obj.end() ? It->second : NullValue;
    }

    JSON& operator[](size_t Index) {
        if (!IsArray()) Value = JSONArray{};
        auto& Arr = std::get<JSONArray>(Value);
        if (Index >= Arr.size()) Arr.resize(Index + 1);
        return Arr[Index];
    }

    const JSON& operator[](size_t Index) const {
        static const JSON NullValue;
        if (!IsArray()) return NullValue;
        const auto& Arr = std::get<JSONArray>(Value);
        return Index < Arr.size() ? Arr[Index] : NullValue;
    }
};

} // namespace DS
} // namespace AstralDB