#include "RenderDoc/Json.h"

#include <charconv>
#include <cctype>
#include <stdexcept>

namespace rdcimport
{
FJsonValue::FJsonValue() : Value(nullptr) {}
FJsonValue::FJsonValue(std::nullptr_t) : Value(nullptr) {}
FJsonValue::FJsonValue(bool value) : Value(value) {}
FJsonValue::FJsonValue(double value) : Value(value) {}
FJsonValue::FJsonValue(std::string value) : Value(std::move(value)) {}
FJsonValue::FJsonValue(FArray value) : Value(std::move(value)) {}
FJsonValue::FJsonValue(FObject value) : Value(std::move(value)) {}

bool FJsonValue::IsNull() const { return std::holds_alternative<std::nullptr_t>(Value); }
bool FJsonValue::IsBool() const { return std::holds_alternative<bool>(Value); }
bool FJsonValue::IsNumber() const { return std::holds_alternative<double>(Value); }
bool FJsonValue::IsString() const { return std::holds_alternative<std::string>(Value); }
bool FJsonValue::IsArray() const { return std::holds_alternative<FArray>(Value); }
bool FJsonValue::IsObject() const { return std::holds_alternative<FObject>(Value); }

bool FJsonValue::AsBool() const { return std::get<bool>(Value); }
double FJsonValue::AsNumber() const { return std::get<double>(Value); }
std::string_view FJsonValue::AsString() const { return std::get<std::string>(Value); }
const FJsonValue::FArray& FJsonValue::AsArray() const { return std::get<FArray>(Value); }
const FJsonValue::FObject& FJsonValue::AsObject() const { return std::get<FObject>(Value); }

const FJsonValue* FJsonValue::Find(std::string_view key) const
{
    if (!IsObject())
    {
        return nullptr;
    }

    const FObject& object = std::get<FObject>(Value);
    const auto found = object.find(key);
    return found != object.end() ? &found->second : nullptr;
}

namespace
{
class FParser
{
public:
    explicit FParser(std::string_view text) : Text(text) {}

    FJsonValue Parse()
    {
        SkipWhitespace();
        FJsonValue value = ParseValue();
        SkipWhitespace();
        if (Position != Text.size())
        {
            Fail("unexpected trailing content");
        }
        return value;
    }

private:
    [[noreturn]] void Fail(std::string_view message) const
    {
        throw std::runtime_error(
            "JSON parse error at byte " + std::to_string(Position) + ": " + std::string(message));
    }

    void SkipWhitespace()
    {
        while (Position < Text.size() && std::isspace(static_cast<unsigned char>(Text[Position])) != 0)
        {
            ++Position;
        }
    }

    char Peek() const
    {
        return Position < Text.size() ? Text[Position] : '\0';
    }

    char Consume()
    {
        if (Position >= Text.size())
        {
            Fail("unexpected end of input");
        }
        return Text[Position++];
    }

    void Expect(char token)
    {
        if (Consume() != token)
        {
            Fail("unexpected token");
        }
    }

    FJsonValue ParseValue()
    {
        switch (Peek())
        {
        case '{': return ParseObject();
        case '[': return ParseArray();
        case '"': return FJsonValue(ParseString());
        case 't': return ParseLiteral("true", FJsonValue(true));
        case 'f': return ParseLiteral("false", FJsonValue(false));
        case 'n': return ParseLiteral("null", FJsonValue(nullptr));
        default:
            if (Peek() == '-' || std::isdigit(static_cast<unsigned char>(Peek())) != 0)
            {
                return FJsonValue(ParseNumber());
            }
            Fail("invalid value");
        }
    }

    FJsonValue ParseLiteral(std::string_view token, FJsonValue value)
    {
        if (Text.substr(Position, token.size()) != token)
        {
            Fail("invalid literal");
        }
        Position += token.size();
        return value;
    }

    FJsonValue ParseObject()
    {
        Expect('{');
        SkipWhitespace();

        FJsonValue::FObject object;
        if (Peek() == '}')
        {
            ++Position;
            return FJsonValue(std::move(object));
        }

        while (true)
        {
            SkipWhitespace();
            std::string key = ParseString();
            SkipWhitespace();
            Expect(':');
            SkipWhitespace();
            object.emplace(std::move(key), ParseValue());
            SkipWhitespace();

            const char token = Consume();
            if (token == '}')
            {
                break;
            }
            if (token != ',')
            {
                Fail("expected ',' or '}'");
            }
        }

        return FJsonValue(std::move(object));
    }

    FJsonValue ParseArray()
    {
        Expect('[');
        SkipWhitespace();

        FJsonValue::FArray array;
        if (Peek() == ']')
        {
            ++Position;
            return FJsonValue(std::move(array));
        }

        while (true)
        {
            SkipWhitespace();
            array.push_back(ParseValue());
            SkipWhitespace();

            const char token = Consume();
            if (token == ']')
            {
                break;
            }
            if (token != ',')
            {
                Fail("expected ',' or ']'");
            }
        }

        return FJsonValue(std::move(array));
    }

    std::string ParseString()
    {
        Expect('"');
        std::string out;

        while (true)
        {
            const char c = Consume();
            if (c == '"')
            {
                break;
            }

            if (c == '\\')
            {
                const char escaped = Consume();
                switch (escaped)
                {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escaped);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                default:
                    Fail("unsupported escape");
                }
            }
            else
            {
                out.push_back(c);
            }
        }

        return out;
    }

    double ParseNumber()
    {
        const size_t start = Position;
        if (Peek() == '-')
        {
            ++Position;
        }

        while (std::isdigit(static_cast<unsigned char>(Peek())) != 0)
        {
            ++Position;
        }

        if (Peek() == '.')
        {
            ++Position;
            while (std::isdigit(static_cast<unsigned char>(Peek())) != 0)
            {
                ++Position;
            }
        }

        if (Peek() == 'e' || Peek() == 'E')
        {
            ++Position;
            if (Peek() == '+' || Peek() == '-')
            {
                ++Position;
            }
            while (std::isdigit(static_cast<unsigned char>(Peek())) != 0)
            {
                ++Position;
            }
        }

        const std::string_view token = Text.substr(start, Position - start);
        double value = 0.0;
        const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (ec != std::errc{} || end != token.data() + token.size())
        {
            Fail("invalid number");
        }
        return value;
    }

    std::string_view Text;
    size_t Position = 0;
};
}

FJsonValue ParseJson(std::string_view text)
{
    return FParser(text).Parse();
}
}
