#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace daw::toml {

// A small TOML reader, and helpers for writing it. It reads what project files need and what a
// person editing one by hand is likely to write: comments, bare, quoted and dotted keys, [tables]
// and [[arrays of tables]], basic and literal strings (one line), integers (decimal, hex, octal,
// binary, with underscores), floats (with exponents, inf and nan), booleans, arrays (over several
// lines, trailing comma) and inline tables. Multi-line strings and dates are refused with an error.

class Value;
using Array = std::vector<Value>;

// A table keeps its keys in the order they were read
class Table {
public:
    const Value *find(const std::string &key) const;
    Value *find(const std::string &key);
    // Adds the key, or returns the existing value
    Value &operator[](const std::string &key);
    const std::vector<std::pair<std::string, Value>> &entries() const { return entries_; }

private:
    std::vector<std::pair<std::string, Value>> entries_;
};

enum class Type { String, Integer, Float, Boolean, Array, Table };

class Value {
public:
    Value() : Value(Table{}) {}
    Value(std::string s) : type_(Type::String), string_(std::move(s)) {}
    Value(int64_t i) : type_(Type::Integer), integer_(i) {}
    Value(double f) : type_(Type::Float), float_(f) {}
    Value(bool b) : type_(Type::Boolean), boolean_(b) {}
    Value(Array a) : type_(Type::Array), array_(std::make_shared<Array>(std::move(a))) {}
    Value(Table t) : type_(Type::Table), table_(std::make_shared<Table>(std::move(t))) {}
    Value(const Value &other);
    Value &operator=(const Value &other);
    Value(Value &&) = default;
    Value &operator=(Value &&) = default;

    Type type() const { return type_; }
    bool isTable() const { return type_ == Type::Table; }
    bool isArray() const { return type_ == Type::Array; }
    bool isNumber() const { return type_ == Type::Integer || type_ == Type::Float; }

    const std::string &string() const { return string_; }
    int64_t integer() const { return integer_; }
    double number() const { return type_ == Type::Integer ? static_cast<double>(integer_) : float_; }
    bool boolean() const { return boolean_; }
    Array &array() { return *array_; }
    const Array &array() const { return *array_; }
    Table &table() { return *table_; }
    const Table &table() const { return *table_; }

    // Lookups that fall back to a default when the key is missing or has another type
    std::string get(const std::string &key, const std::string &fallback) const;
    std::string get(const std::string &key, const char *fallback) const { return get(key, std::string(fallback)); }
    double get(const std::string &key, double fallback) const;
    int64_t get(const std::string &key, int64_t fallback) const;
    int get(const std::string &key, int fallback) const { return static_cast<int>(get(key, static_cast<int64_t>(fallback))); }
    bool get(const std::string &key, bool fallback) const;
    // The array of tables (or plain array) under key, or nullptr
    const Array *arrayAt(const std::string &key) const;
    const Value *at(const std::string &key) const;

private:
    Type type_;
    std::string string_;
    int64_t integer_ = 0;
    double float_ = 0.0;
    bool boolean_ = false;
    // Shared only while being built; copies are deep
    std::shared_ptr<Array> array_;
    std::shared_ptr<Table> table_;
};

const char *typeName(Type type);

// Parses a whole document into its root table. On failure returns false with "line N: what" in
// error.
bool parse(const std::string &text, Value &root, std::string &error);

// Writing: a key (bare if it can be, else quoted), a quoted string, and numbers that read back
// exactly (floats always have a '.' or an exponent, so they stay floats)
std::string key(const std::string &k);
std::string quote(const std::string &s);
std::string number(double value);
std::string number(float value);

}
