#include "Json.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace daw::json {

namespace {

class Parser {
public:
    explicit Parser(const std::string &text) : s_(text) {}

    bool document(toml::Value &root, std::string &error)
    {
        space();
        if (!value(root, 0))
            return fail(error);
        space();
        if (i_ != s_.size()) {
            why_ = "unexpected text after the end";
            return fail(error);
        }
        return true;
    }

private:
    static constexpr int kMaxDepth = 200;

    bool fail(std::string &error)
    {
        error = "at " + std::to_string(i_) + ": " + (why_.empty() ? "unreadable JSON" : why_);
        return false;
    }

    void space()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    bool literal(const char *word)
    {
        const std::string w(word);
        if (s_.compare(i_, w.size(), w) != 0) {
            why_ = "unknown word";
            return false;
        }
        i_ += w.size();
        return true;
    }

    bool value(toml::Value &out, int depth)
    {
        if (depth > kMaxDepth) {
            why_ = "nested too deeply";
            return false;
        }
        if (i_ >= s_.size()) {
            why_ = "it ends too soon";
            return false;
        }
        const char c = s_[i_];
        if (c == '{')
            return object(out, depth);
        if (c == '[')
            return array(out, depth);
        if (c == '"') {
            std::string text;
            if (!string(text))
                return false;
            out = toml::Value(std::move(text));
            return true;
        }
        if (c == 't') {
            out = toml::Value(true);
            return literal("true");
        }
        if (c == 'f') {
            out = toml::Value(false);
            return literal("false");
        }
        if (c == 'n') {
            out = toml::Value(toml::Table{});
            return literal("null");
        }
        return number(out);
    }

    bool object(toml::Value &out, int depth)
    {
        ++i_; // {
        toml::Table table;
        space();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            out = toml::Value(std::move(table));
            return true;
        }
        for (;;) {
            space();
            std::string key;
            if (i_ >= s_.size() || s_[i_] != '"') {
                why_ = "expected a quoted key";
                return false;
            }
            if (!string(key))
                return false;
            space();
            if (i_ >= s_.size() || s_[i_] != ':') {
                why_ = "expected ':'";
                return false;
            }
            ++i_;
            space();
            toml::Value item;
            if (!value(item, depth + 1))
                return false;
            table[key] = std::move(item);
            space();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}') {
                ++i_;
                out = toml::Value(std::move(table));
                return true;
            }
            why_ = "expected ',' or '}'";
            return false;
        }
    }

    bool array(toml::Value &out, int depth)
    {
        ++i_; // [
        toml::Array items;
        space();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            out = toml::Value(std::move(items));
            return true;
        }
        for (;;) {
            space();
            toml::Value item;
            if (!value(item, depth + 1))
                return false;
            items.push_back(std::move(item));
            space();
            if (i_ < s_.size() && s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']') {
                ++i_;
                out = toml::Value(std::move(items));
                return true;
            }
            why_ = "expected ',' or ']'";
            return false;
        }
    }

    static void utf8(std::string &out, uint32_t cp)
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

    bool hex4(uint32_t &cp)
    {
        if (i_ + 4 > s_.size()) {
            why_ = "short \\u escape";
            return false;
        }
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            cp <<= 4;
            if (h >= '0' && h <= '9')
                cp |= static_cast<uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f')
                cp |= static_cast<uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                cp |= static_cast<uint32_t>(h - 'A' + 10);
            else {
                why_ = "bad \\u escape";
                return false;
            }
        }
        return true;
    }

    bool string(std::string &out)
    {
        ++i_; // "
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20) {
                why_ = "control character in a string";
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size())
                break;
            const char e = s_[i_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                    i_ += 2;
                    uint32_t low = 0;
                    if (!hex4(low))
                        return false;
                    if (low >= 0xDC00 && low < 0xE000)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    else
                        cp = 0xFFFD;
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                utf8(out, cp);
                break;
            }
            default:
                why_ = "unknown escape";
                return false;
            }
        }
        why_ = "unterminated string";
        return false;
    }

    bool number(toml::Value &out)
    {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-')
            ++i_;
        bool fraction = false;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                ++i_;
            } else if (c == '.' || c == 'e' || c == 'E' || ((c == '+' || c == '-') && i_ > start &&
                                                             (s_[i_ - 1] == 'e' || s_[i_ - 1] == 'E'))) {
                fraction = true;
                ++i_;
            } else {
                break;
            }
        }
        const std::string text = s_.substr(start, i_ - start);
        if (text.empty() || text == "-") {
            why_ = "expected a value";
            return false;
        }
        char *end = nullptr;
        errno = 0;
        if (!fraction) {
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0') {
                out = toml::Value(static_cast<int64_t>(v));
                return true;
            }
        }
        const double d = std::strtod(text.c_str(), &end);
        if (!end || *end != '\0') {
            why_ = "bad number";
            return false;
        }
        out = toml::Value(d);
        return true;
    }

    const std::string &s_;
    size_t i_ = 0;
    std::string why_;
};

}

bool parse(const std::string &text, toml::Value &root, std::string &error)
{
    return Parser(text).document(root, error);
}

}
