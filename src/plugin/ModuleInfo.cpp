#include "plugin/ModuleInfo.h"

#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>

namespace plugin {

namespace {

namespace fs = std::filesystem;

// A value of the file. It is JSON5 as Steinberg's tools write it (comments, trailing commas, keys
// without quotes), read by a small parser of its own: the plugin module has no other, and the file
// comes with the plugin, so nothing in it is trusted (its size, its depth, its strings are limited)
struct Value {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    double number = 0.0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    const Value *get(const std::string &key) const
    {
        for (const auto &m : members)
            if (m.first == key)
                return &m.second;
        return nullptr;
    }
    std::string string(const std::string &key) const
    {
        const Value *v = get(key);
        return v && v->kind == Kind::String ? v->text : std::string();
    }
};

class Parser {
public:
    Parser(const std::string &text) : p_(text.data()), end_(text.data() + text.size()) {}

    bool parse(Value &out)
    {
        if (!value(out, 0))
            return false;
        skip();
        return p_ == end_;
    }

private:
    static constexpr int kMaxDepth = 64;
    static constexpr size_t kMaxString = 4096;

    void skip()
    {
        while (p_ < end_) {
            if (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n') {
                ++p_;
            } else if (*p_ == '/' && p_ + 1 < end_ && p_[1] == '/') {
                while (p_ < end_ && *p_ != '\n')
                    ++p_;
            } else if (*p_ == '/' && p_ + 1 < end_ && p_[1] == '*') {
                p_ += 2;
                while (p_ + 1 < end_ && !(p_[0] == '*' && p_[1] == '/'))
                    ++p_;
                p_ = p_ + 1 < end_ ? p_ + 2 : end_;
            } else {
                break;
            }
        }
    }

    static bool identifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$'; }

    bool value(Value &out, int depth)
    {
        skip();
        if (p_ >= end_ || depth > kMaxDepth)
            return false;
        if (*p_ == '{')
            return object(out, depth);
        if (*p_ == '[')
            return array(out, depth);
        if (*p_ == '"' || *p_ == '\'') {
            out.kind = Value::Kind::String;
            return string(out.text);
        }
        if (*p_ == '-' || *p_ == '+' || *p_ == '.' || std::isdigit(static_cast<unsigned char>(*p_)))
            return number(out);
        std::string word;
        while (p_ < end_ && identifierChar(*p_))
            word += *p_++;
        if (word == "true" || word == "false") {
            out.kind = Value::Kind::Bool;
            out.number = word == "true" ? 1.0 : 0.0;
            return true;
        }
        if (word == "null") {
            out.kind = Value::Kind::Null;
            return true;
        }
        if (word == "Infinity" || word == "NaN") {
            out.kind = Value::Kind::Number;
            return true;
        }
        return false;
    }

    bool object(Value &out, int depth)
    {
        out.kind = Value::Kind::Object;
        ++p_;
        for (;;) {
            skip();
            if (p_ >= end_)
                return false;
            if (*p_ == '}') {
                ++p_;
                return true;
            }
            std::string key;
            if (*p_ == '"' || *p_ == '\'') {
                if (!string(key))
                    return false;
            } else {
                while (p_ < end_ && identifierChar(*p_))
                    key += *p_++;
                if (key.empty())
                    return false;
            }
            skip();
            if (p_ >= end_ || *p_ != ':')
                return false;
            ++p_;
            Value v;
            if (!value(v, depth + 1))
                return false;
            out.members.emplace_back(std::move(key), std::move(v));
            skip();
            if (p_ < end_ && *p_ == ',')
                ++p_;
            else if (p_ < end_ && *p_ != '}')
                return false;
        }
    }

    bool array(Value &out, int depth)
    {
        out.kind = Value::Kind::Array;
        ++p_;
        for (;;) {
            skip();
            if (p_ >= end_)
                return false;
            if (*p_ == ']') {
                ++p_;
                return true;
            }
            Value v;
            if (!value(v, depth + 1))
                return false;
            out.items.push_back(std::move(v));
            skip();
            if (p_ < end_ && *p_ == ',')
                ++p_;
            else if (p_ < end_ && *p_ != ']')
                return false;
        }
    }

    void utf8(uint32_t c, std::string &out)
    {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }

    bool hex4(uint32_t &c)
    {
        c = 0;
        for (int i = 0; i < 4; ++i, ++p_) {
            if (p_ >= end_ || !std::isxdigit(static_cast<unsigned char>(*p_)))
                return false;
            const char h = *p_;
            c = c * 16 + static_cast<uint32_t>(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
        }
        return true;
    }

    bool string(std::string &out)
    {
        const char quote = *p_++;
        while (p_ < end_ && *p_ != quote) {
            if (out.size() > kMaxString)
                return false;
            if (*p_ != '\\') {
                out += *p_++;
                continue;
            }
            if (++p_ >= end_)
                return false;
            const char e = *p_++;
            switch (e) {
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case '\n': break; // a line continued
            case 'u': {
                uint32_t c = 0;
                if (!hex4(c))
                    return false;
                if (c >= 0xD800 && c < 0xDC00 && p_ + 1 < end_ && p_[0] == '\\' && p_[1] == 'u') {
                    p_ += 2;
                    uint32_t low = 0;
                    if (!hex4(low))
                        return false;
                    c = low >= 0xDC00 && low < 0xE000 ? 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00) : 0xFFFD;
                }
                utf8(c, out);
                break;
            }
            default: out += e; break; // \" \' \\ \/ and anything else as itself
            }
        }
        if (p_ >= end_)
            return false;
        ++p_;
        return true;
    }

    bool number(Value &out)
    {
        const char *start = p_;
        while (p_ < end_ && (std::isalnum(static_cast<unsigned char>(*p_)) || *p_ == '-' || *p_ == '+' || *p_ == '.'))
            ++p_;
        const std::string text(start, p_);
        char *stop = nullptr;
        out.kind = Value::Kind::Number;
        out.number = std::strtod(text.c_str(), &stop);
        return stop && *stop == 0 && !text.empty();
    }

    const char *p_;
    const char *end_;
};

// A class id as the file writes it (32 hex digits, its bytes in order) as the plugin's id
bool idOf(const std::string &cid, std::string &id)
{
    if (cid.size() != 32)
        return false;
    id = "vst3:";
    for (char c : cid) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
        id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return true;
}

}

bool readModuleInfo(const std::string &bundle, std::vector<Descriptor> &found, std::string &error)
{
    std::error_code ec;
    const fs::path file = fs::u8path(bundle) / "Contents" / "Resources" / "moduleinfo.json";
    const auto size = fs::file_size(file, ec);
    if (ec || !fs::is_regular_file(file, ec)) {
        error = "The bundle has no moduleinfo.json";
        return false;
    }
    if (size > (4u << 20)) {
        error = "Its moduleinfo.json is too big";
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    if (!in) {
        error = "Its moduleinfo.json couldn't be read";
        return false;
    }
    Value root;
    std::string json = text.str();
    if (json.size() >= 3 && json.compare(0, 3, "\xEF\xBB\xBF") == 0)
        json.erase(0, 3);
    if (!Parser(json).parse(root) || root.kind != Value::Kind::Object) {
        error = "Its moduleinfo.json isn't JSON";
        return false;
    }
    const Value *factory = root.get("Factory Info");
    const std::string factoryVendor = factory ? factory->string("Vendor") : std::string();
    const std::string url = factory ? factory->string("URL") : std::string();
    const Value *classes = root.get("Classes");
    std::vector<Descriptor> plugins;
    for (size_t i = 0; classes && classes->kind == Value::Kind::Array && i < classes->items.size() && i < 1000; ++i) {
        const Value &c = classes->items[i];
        Descriptor d;
        if (c.kind != Value::Kind::Object || c.string("Category") != "Audio Module Class" || !idOf(c.string("CID"), d.id))
            continue;
        d.name = c.string("Name");
        d.vendor = c.string("Vendor");
        if (d.vendor.empty())
            d.vendor = factoryVendor;
        d.version = c.string("Version");
        d.url = url;
        std::string subcategories;
        if (const Value *sub = c.get("Sub Categories")) {
            if (sub->kind == Value::Kind::String)
                subcategories = sub->text;
            for (const Value &word : sub->items)
                if (word.kind == Value::Kind::String)
                    subcategories += (subcategories.empty() ? "" : "|") + word.text;
        }
        d.features = vst3Features(subcategories);
        d.path = bundle;
        if (!d.name.empty())
            plugins.push_back(std::move(d));
    }
    if (plugins.empty()) {
        error = "Its moduleinfo.json lists no plugin";
        return false;
    }
    found.insert(found.end(), plugins.begin(), plugins.end());
    return true;
}

}
