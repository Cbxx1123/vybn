#include "Toml.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace daw::toml {

const Value *Table::find(const std::string &key) const
{
    for (const auto &[k, v] : entries_) {
        if (k == key)
            return &v;
    }
    return nullptr;
}

Value *Table::find(const std::string &key)
{
    for (auto &[k, v] : entries_) {
        if (k == key)
            return &v;
    }
    return nullptr;
}

Value &Table::operator[](const std::string &key)
{
    if (Value *v = find(key))
        return *v;
    entries_.emplace_back(key, Value());
    return entries_.back().second;
}

Value::Value(const Value &other)
    : type_(other.type_), string_(other.string_), integer_(other.integer_), float_(other.float_),
      boolean_(other.boolean_),
      array_(other.array_ ? std::make_shared<Array>(*other.array_) : nullptr),
      table_(other.table_ ? std::make_shared<Table>(*other.table_) : nullptr)
{
}

Value &Value::operator=(const Value &other)
{
    if (this != &other) {
        Value copy(other);
        *this = std::move(copy);
    }
    return *this;
}

const Value *Value::at(const std::string &key) const
{
    return isTable() ? table_->find(key) : nullptr;
}

std::string Value::get(const std::string &key, const std::string &fallback) const
{
    const Value *v = at(key);
    return v && v->type_ == Type::String ? v->string_ : fallback;
}

double Value::get(const std::string &key, double fallback) const
{
    const Value *v = at(key);
    return v && v->isNumber() ? v->number() : fallback;
}

int64_t Value::get(const std::string &key, int64_t fallback) const
{
    const Value *v = at(key);
    if (!v)
        return fallback;
    if (v->type_ == Type::Integer)
        return v->integer_;
    if (v->type_ == Type::Float && std::isfinite(v->float_))
        return static_cast<int64_t>(std::llround(v->float_));
    return fallback;
}

bool Value::get(const std::string &key, bool fallback) const
{
    const Value *v = at(key);
    return v && v->type_ == Type::Boolean ? v->boolean_ : fallback;
}

const Array *Value::arrayAt(const std::string &key) const
{
    const Value *v = at(key);
    return v && v->isArray() ? &v->array() : nullptr;
}

const char *typeName(Type type)
{
    switch (type) {
    case Type::String: return "a string";
    case Type::Integer: return "an integer";
    case Type::Float: return "a float";
    case Type::Boolean: return "a boolean";
    case Type::Array: return "an array";
    case Type::Table: return "a table";
    }
    return "a value";
}

namespace {

bool isBare(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

void appendUtf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser {
public:
    explicit Parser(const std::string &text) : s_(text) {}

    bool run(Value &root)
    {
        root = Value(Table{});
        Table *current = &root.table();
        // A UTF-8 byte order mark
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0)
            pos_ = 3;
        while (true) {
            skipSpace();
            if (atEnd())
                return true;
            const char c = peek();
            if (c == '#' || c == '\n' || c == '\r') {
                if (!endOfLine())
                    return false;
                continue;
            }
            if (c == '[') {
                const bool array = pos_ + 1 < s_.size() && s_[pos_ + 1] == '[';
                pos_ += array ? 2 : 1;
                std::vector<std::string> path;
                if (!readKey(path))
                    return false;
                skipSpace();
                if (!expect(array ? "]]" : "]"))
                    return false;
                current = array ? openArrayTable(root.table(), path) : openTable(root.table(), path);
                if (!current)
                    return false;
                if (!endOfLine())
                    return false;
                continue;
            }
            if (!keyValue(*current))
                return false;
            if (!endOfLine())
                return false;
        }
    }

    const std::string &error() const { return error_; }

private:
    bool atEnd() const { return pos_ >= s_.size(); }
    char peek() const { return atEnd() ? '\0' : s_[pos_]; }

    bool fail(const std::string &what)
    {
        if (error_.empty())
            error_ = "line " + std::to_string(line_) + ": " + what;
        return false;
    }

    void skipSpace()
    {
        while (!atEnd() && (peek() == ' ' || peek() == '\t'))
            ++pos_;
    }

    // Spaces, comments and newlines, inside arrays
    void skipBlank()
    {
        while (!atEnd()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r') {
                ++pos_;
            } else if (c == '\n') {
                ++pos_;
                ++line_;
            } else if (c == '#') {
                while (!atEnd() && peek() != '\n')
                    ++pos_;
            } else {
                break;
            }
        }
    }

    // After a statement: only a comment may follow on its line
    bool endOfLine()
    {
        skipSpace();
        if (peek() == '#') {
            while (!atEnd() && peek() != '\n')
                ++pos_;
        }
        if (peek() == '\r')
            ++pos_;
        if (atEnd())
            return true;
        if (peek() != '\n')
            return fail(std::string("unexpected '") + peek() + "' after a value");
        ++pos_;
        ++line_;
        return true;
    }

    bool expect(const char *token)
    {
        for (const char *t = token; *t; ++t) {
            if (peek() != *t)
                return fail(std::string("expected '") + token + "'");
            ++pos_;
        }
        return true;
    }

    // A dotted key: bare or quoted parts separated by dots
    bool readKey(std::vector<std::string> &parts)
    {
        while (true) {
            skipSpace();
            std::string part;
            if (peek() == '"' || peek() == '\'') {
                if (!readString(part))
                    return false;
            } else {
                const size_t from = pos_;
                while (!atEnd() && isBare(peek()))
                    ++pos_;
                if (pos_ == from)
                    return fail("expected a key");
                part = s_.substr(from, pos_ - from);
            }
            parts.push_back(std::move(part));
            skipSpace();
            if (peek() != '.')
                return true;
            ++pos_;
        }
    }

    std::string joined(const std::vector<std::string> &path) const
    {
        std::string out;
        for (const std::string &p : path)
            out += (out.empty() ? "" : ".") + p;
        return out;
    }

    // Walks to a table through path, creating tables on the way. In an array of tables, the last
    // one is the one walked into.
    Table *walk(Table &from, const std::vector<std::string> &path, size_t count)
    {
        Table *t = &from;
        for (size_t i = 0; i < count; ++i) {
            Value &v = (*t)[path[i]];
            if (v.isArray() && !v.array().empty() && v.array().back().isTable()) {
                t = &v.array().back().table();
            } else if (v.isTable()) {
                t = &v.table();
            } else {
                fail("\"" + path[i] + "\" is already " + typeName(v.type()) + ", not a table");
                return nullptr;
            }
        }
        return t;
    }

    Table *openTable(Table &root, const std::vector<std::string> &path)
    {
        return walk(root, path, path.size());
    }

    Table *openArrayTable(Table &root, const std::vector<std::string> &path)
    {
        Table *parent = walk(root, path, path.size() - 1);
        if (!parent)
            return nullptr;
        Value *existing = parent->find(path.back());
        if (!existing) {
            (*parent)[path.back()] = Value(Array{});
            existing = parent->find(path.back());
        }
        if (!existing->isArray()) {
            fail("\"" + joined(path) + "\" is already " + typeName(existing->type()) + ", not an array of tables");
            return nullptr;
        }
        existing->array().push_back(Value(Table{}));
        return &existing->array().back().table();
    }

    bool keyValue(Table &table)
    {
        std::vector<std::string> path;
        if (!readKey(path))
            return false;
        skipSpace();
        if (!expect("="))
            return false;
        skipSpace();
        Value value;
        if (!readValue(value))
            return false;
        Table *target = walk(table, path, path.size() - 1);
        if (!target)
            return false;
        if (target->find(path.back()))
            return fail("\"" + joined(path) + "\" is set twice");
        (*target)[path.back()] = std::move(value);
        return true;
    }

    bool readValue(Value &out)
    {
        const char c = peek();
        if (c == '"' || c == '\'') {
            std::string s;
            if (!readString(s))
                return false;
            out = Value(std::move(s));
            return true;
        }
        if (c == '[')
            return readArray(out);
        if (c == '{')
            return readInlineTable(out);
        if (s_.compare(pos_, 4, "true") == 0 && !isBare(pos_ + 4 < s_.size() ? s_[pos_ + 4] : ' ')) {
            pos_ += 4;
            out = Value(true);
            return true;
        }
        if (s_.compare(pos_, 5, "false") == 0 && !isBare(pos_ + 5 < s_.size() ? s_[pos_ + 5] : ' ')) {
            pos_ += 5;
            out = Value(false);
            return true;
        }
        return readNumber(out);
    }

    bool readString(std::string &out)
    {
        const char quote = peek();
        if (s_.compare(pos_, 3, std::string(3, quote)) == 0)
            return fail("multi-line strings aren't supported");
        ++pos_;
        while (true) {
            if (atEnd() || peek() == '\n')
                return fail("unterminated string");
            const char c = s_[pos_++];
            if (c == quote)
                return true;
            if (c != '\\' || quote == '\'') {
                out += c;
                continue;
            }
            if (atEnd())
                return fail("unterminated string");
            const char e = s_[pos_++];
            switch (e) {
            case 'b': out += '\b'; break;
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'f': out += '\f'; break;
            case 'r': out += '\r'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case 'u':
            case 'U': {
                const size_t digits = e == 'u' ? 4 : 8;
                if (pos_ + digits > s_.size())
                    return fail("bad unicode escape");
                uint32_t cp = 0;
                for (size_t i = 0; i < digits; ++i) {
                    const char h = s_[pos_++];
                    cp <<= 4;
                    if (h >= '0' && h <= '9')
                        cp |= static_cast<uint32_t>(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        cp |= static_cast<uint32_t>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        cp |= static_cast<uint32_t>(h - 'A' + 10);
                    else
                        return fail("bad unicode escape");
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
                    return fail("bad unicode escape");
                appendUtf8(out, cp);
                break;
            }
            default:
                return fail(std::string("unknown escape \\") + e);
            }
        }
    }

    bool readArray(Value &out)
    {
        ++pos_;
        Array items;
        while (true) {
            skipBlank();
            if (atEnd())
                return fail("unterminated array");
            if (peek() == ']') {
                ++pos_;
                break;
            }
            Value item;
            if (!readValue(item))
                return false;
            items.push_back(std::move(item));
            skipBlank();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            return fail("expected ',' or ']' in an array");
        }
        out = Value(std::move(items));
        return true;
    }

    bool readInlineTable(Value &out)
    {
        ++pos_;
        Value table{Table{}};
        skipSpace();
        if (peek() == '}') {
            ++pos_;
            out = std::move(table);
            return true;
        }
        while (true) {
            skipSpace();
            if (!keyValue(table.table()))
                return false;
            skipSpace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                break;
            }
            return fail("expected ',' or '}' in an inline table");
        }
        out = std::move(table);
        return true;
    }

    bool readNumber(Value &out)
    {
        const size_t from = pos_;
        while (!atEnd()) {
            const char c = peek();
            if (isBare(c) || c == '+' || c == '.' || c == ':')
                ++pos_;
            else
                break;
        }
        std::string token = s_.substr(from, pos_ - from);
        if (token.empty())
            return fail(atEnd() || peek() == '\n' ? "expected a value" : std::string("unexpected '") + peek() + "'");

        std::string body = token;
        bool negative = false;
        if (body[0] == '+' || body[0] == '-') {
            negative = body[0] == '-';
            body.erase(0, 1);
        }
        if (body == "inf" || body == "nan") {
            const double v = body == "inf" ? HUGE_VAL : std::nan("");
            out = Value(negative ? -v : v);
            return true;
        }
        if (token.find(':') != std::string::npos ||
            (token.size() > 4 && token[4] == '-' && token.find_first_not_of("0123456789") == 4))
            return fail("dates and times aren't supported");

        // Underscores only between digits
        std::string digits;
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] != '_') {
                digits += body[i];
                continue;
            }
            if (i == 0 || i + 1 == body.size() || !std::isxdigit(static_cast<unsigned char>(body[i - 1])) ||
                !std::isxdigit(static_cast<unsigned char>(body[i + 1])))
                return fail("bad number \"" + token + "\"");
        }
        int base = 10;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'o' || digits[1] == 'b')) {
            if (token[0] == '+' || token[0] == '-')
                return fail("bad number \"" + token + "\"");
            base = digits[1] == 'x' ? 16 : digits[1] == 'o' ? 8 : 2;
            digits.erase(0, 2);
        }
        const bool isFloat = base == 10 && digits.find_first_of(".eE") != std::string::npos;
        if (digits.empty() || (base == 10 && !std::isdigit(static_cast<unsigned char>(digits[0]))))
            return fail("bad value \"" + token + "\"");
        errno = 0;
        char *end = nullptr;
        if (isFloat) {
            if (digits.back() == '.' || digits.find(".e") != std::string::npos || digits.find(".E") != std::string::npos)
                return fail("bad number \"" + token + "\"");
            const double v = std::strtod(digits.c_str(), &end);
            if (*end != '\0' || errno == ERANGE)
                return fail("bad number \"" + token + "\"");
            out = Value(negative ? -v : v);
            return true;
        }
        if (base == 10 && digits.size() > 1 && digits[0] == '0')
            return fail("bad number \"" + token + "\" (leading zero)");
        const unsigned long long v = std::strtoull(digits.c_str(), &end, base);
        if (*end != '\0' || errno == ERANGE || v > (negative ? 9223372036854775808ull : 9223372036854775807ull))
            return fail("bad number \"" + token + "\"");
        out = Value(negative ? static_cast<int64_t>(0 - v) : static_cast<int64_t>(v));
        return true;
    }

    const std::string &s_;
    size_t pos_ = 0;
    int line_ = 1;
    std::string error_;
};

}

bool parse(const std::string &text, Value &root, std::string &error)
{
    Parser parser(text);
    if (parser.run(root))
        return true;
    error = parser.error();
    return false;
}

std::string key(const std::string &k)
{
    bool bare = !k.empty();
    for (char c : k)
        bare = bare && isBare(c);
    return bare ? k : quote(k);
}

std::string quote(const std::string &s)
{
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04X", static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

namespace {

// The shortest %g form that reads back as the same value
template <typename T, typename Read>
std::string shortest(T value, int maxDigits, Read read)
{
    if (std::isnan(value))
        return "nan";
    if (std::isinf(value))
        return value < 0 ? "-inf" : "inf";
    char buffer[40];
    for (int digits = 1; digits <= maxDigits; ++digits) {
        std::snprintf(buffer, sizeof(buffer), "%.*g", digits, static_cast<double>(value));
        if (read(buffer) == value)
            break;
    }
    std::string out = buffer;
    if (out.find_first_of(".e") == std::string::npos)
        out += ".0";
    return out;
}

}

std::string number(double value)
{
    return shortest(value, 17, [](const char *s) { return std::strtod(s, nullptr); });
}

std::string number(float value)
{
    return shortest(value, 9, [](const char *s) { return std::strtof(s, nullptr); });
}

}
