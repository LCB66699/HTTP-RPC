#pragma once
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <cctype>

namespace rpc_json {

class Value;

using Null = std::nullptr_t;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

enum class ValueType { Null, Bool, Float, String, Array, Object };

class Value {
public:
    Value();
    Value(std::nullptr_t);
    Value(bool v);
    Value(int v);
    Value(double v);
    Value(const char* v);
    Value(std::string v);
    Value(Array v);
    Value(Object v);
    Value(const Value& other);
    Value(Value&& other) noexcept;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept;
    ~Value();

    ValueType type() const { return type_; }

    bool is_null() const { return type_ == ValueType::Null; }
    bool is_bool() const { return type_ == ValueType::Bool; }
    bool is_number() const { return type_ == ValueType::Float; }
    bool is_boolean() const { return is_bool(); }
    bool is_string() const { return type_ == ValueType::String; }
    bool is_object() const { return type_ == ValueType::Object; }
    bool is_array() const { return type_ == ValueType::Array; }

    const char* type_name() const {
        switch (type_) {
            case ValueType::Null: return "null";
            case ValueType::Bool: return "bool";
            case ValueType::Float: return "number";
            case ValueType::String: return "string";
            case ValueType::Array: return "array";
            case ValueType::Object: return "object";
        }
        return "unknown";
    }

    bool get_bool() const { return *reinterpret_cast<const bool*>(&mem_); }
    int get_int() const { return static_cast<int>(*reinterpret_cast<const double*>(&mem_)); }
    double get_double() const { return *reinterpret_cast<const double*>(&mem_); }
    const std::string& get_string() const { return *reinterpret_cast<const std::string*>(&mem_); }
    std::string& get_string() { return *reinterpret_cast<std::string*>(&mem_); }
    const Array& get_array() const { return *reinterpret_cast<const Array*>(&mem_); }
    Array& get_array() { return *reinterpret_cast<Array*>(&mem_); }
    const Object& get_object() const { return *reinterpret_cast<const Object*>(&mem_); }
    Object& get_object() { return *reinterpret_cast<Object*>(&mem_); }

    bool contains(const std::string& key) const {
        if (!is_object()) return false;
        return get_object().count(key) > 0;
    }

    Value& operator[](const char* key) { return (*this)[std::string(key)]; }
    const Value& operator[](const char* key) const { return (*this)[std::string(key)]; }
    Value& operator[](const std::string& key);
    const Value& operator[](const std::string& key) const;
    Value& operator[](size_t idx);
    const Value& operator[](size_t idx) const;

    size_t size() const {
        if (is_array()) return get_array().size();
        if (is_object()) return get_object().size();
        return 0;
    }

    std::string dump() const;
    static Value parse(const std::string& input);
    static Value parse(std::istream& in);

private:
    static constexpr size_t BUF_SIZE  = 64;
    static constexpr size_t BUF_ALIGN = 8;
    static_assert(BUF_SIZE >= sizeof(std::string), "Buffer too small");
    static_assert(BUF_SIZE >= sizeof(Array), "Buffer too small");
    static_assert(BUF_SIZE >= sizeof(Object), "Buffer too small");

    alignas(BUF_ALIGN) unsigned char mem_[BUF_SIZE];
    ValueType type_;

    void destroy();
    void copy_from(const Value& other);
    void move_from(Value&& other);
    void dump_to(std::ostringstream& oss) const;
    static std::string esc(const std::string& s);
};

// Parser class (implementation in .cpp)
class Parser {
public:
    explicit Parser(const std::string& s);
    Value parse();

private:
    const std::string& s_;
    size_t pos_;
    char peek() const;
    char next();
    void skip_ws();
    void expect(char c);
    Value parse_value();
    void parse_lit(const char* lit);
    Value parse_string();
    Value parse_number();
    Value parse_object();
    Value parse_array();
};

inline void to_json(Value& j, const std::string& v) { j = Value(v); }
inline void to_json(Value& j, const char* v) { j = Value(v); }
inline void to_json(Value& j, int v) { j = Value(static_cast<double>(v)); }
inline void to_json(Value& j, double v) { j = Value(v); }
inline void to_json(Value& j, bool v) { j = Value(v); }

inline void from_json(const Value& j, std::string& v) { v = j.get_string(); }
inline void from_json(const Value& j, int& v) { v = j.get_int(); }
inline void from_json(const Value& j, double& v) { v = j.get_double(); }
inline void from_json(const Value& j, bool& v) { v = j.get_bool(); }

} // namespace rpc_json
