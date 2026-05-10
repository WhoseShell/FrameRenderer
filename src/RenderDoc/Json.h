#pragma once

#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rdcimport
{
class FJsonValue
{
public:
    using FArray = std::vector<FJsonValue>;
    using FObject = std::map<std::string, FJsonValue, std::less<>>;

    FJsonValue();
    explicit FJsonValue(std::nullptr_t);
    explicit FJsonValue(bool value);
    explicit FJsonValue(double value);
    explicit FJsonValue(std::string value);
    explicit FJsonValue(FArray value);
    explicit FJsonValue(FObject value);

    bool IsNull() const;
    bool IsBool() const;
    bool IsNumber() const;
    bool IsString() const;
    bool IsArray() const;
    bool IsObject() const;

    bool AsBool() const;
    double AsNumber() const;
    std::string_view AsString() const;
    const FArray& AsArray() const;
    const FObject& AsObject() const;

    const FJsonValue* Find(std::string_view key) const;

private:
    std::variant<std::nullptr_t, bool, double, std::string, FArray, FObject> Value;
};

FJsonValue ParseJson(std::string_view text);
}
