#include "rpc_json.h"
#include <stdexcept>

namespace rpc_json {

// Parser implementation
Parser::Parser(const std::string& s) : s_(s), pos_(0) {}

Value Parser::parse() { skip_ws(); return parse_value(); }

char Parser::peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
char Parser::next() { return pos_ < s_.size() ? s_[pos_++] : '\0'; }

void Parser::skip_ws() {
    while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
}

void Parser::expect(char c) {
    if (next() != c) throw std::runtime_error(std::string("Expected '") + c + "'");
}

Value Parser::parse_value() {
    char c = peek();
    if (c == '"') return parse_string();
    if (c == 'n') { parse_lit("null"); return Value(); }
    if (c == 't') { parse_lit("true"); return Value(true); }
    if (c == 'f') { parse_lit("false"); return Value(false); }
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
    throw std::runtime_error(std::string("Unexpected: '") + c + "'");
}

void Parser::parse_lit(const char* lit) { while (*lit) expect(*lit++); }

Value Parser::parse_string() {
    expect('"');
    std::string out;
    while (peek() != '"') {
        if (peek() == '\\') {
            next();
            switch (next()) {
                case '"':  out += '"'; break;
                case '\\': out += '\\'; break;
                case '/':  out += '/'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': for (int i = 0; i < 4; ++i) next(); out += '?'; break;
                default: out += '?'; break;
            }
        } else {
            out += next();
        }
    }
    expect('"');
    return Value(out);
}

Value Parser::parse_number() {
    std::string num;
    if (peek() == '-') num += next();
    while (peek() >= '0' && peek() <= '9') num += next();
    if (peek() == '.') { num += next(); while (peek() >= '0' && peek() <= '9') num += next(); }
    if (peek() == 'e' || peek() == 'E') {
        num += next();
        if (peek() == '+' || peek() == '-') num += next();
        while (peek() >= '0' && peek() <= '9') num += next();
    }
    return Value(std::stod(num));
}

Value Parser::parse_object() {
    Object obj;
    expect('{'); skip_ws();
    if (peek() != '}') {
        do {
            skip_ws();
            std::string key = parse_string().get_string();
            skip_ws(); expect(':'); skip_ws();
            obj[key] = parse_value();
            skip_ws();
        } while (peek() == ',' && (next(), true));
    }
    expect('}');
    return Value(obj);
}

Value Parser::parse_array() {
    Array arr;
    expect('['); skip_ws();
    if (peek() != ']') {
        do { skip_ws(); arr.push_back(parse_value()); skip_ws(); }
        while (peek() == ',' && (next(), true));
    }
    expect(']');
    return Value(arr);
}

Value Value::parse(const std::string& input) { return Parser(input).parse(); }
Value Value::parse(std::istream& in) {
    std::ostringstream oss; oss << in.rdbuf(); return Parser(oss.str()).parse();
}

// Missing operator[] implementations
Value& Value::operator[](const std::string& key) {
    if (!is_object()) { destroy(); new (&mem_) Object(); type_ = ValueType::Object; }
    auto& obj = get_object();
    auto it = obj.find(key);
    if (it != obj.end()) return it->second;
    obj[key] = Value();
    return obj[key];
}

const Value& Value::operator[](const std::string& key) const {
    static Value nil;
    if (!is_object()) return nil;
    auto it = get_object().find(key);
    return it != get_object().end() ? it->second : nil;
}

Value& Value::operator[](size_t idx) {
    if (!is_array()) { destroy(); new (&mem_) Array(); type_ = ValueType::Array; }
    auto& arr = get_array();
    if (idx >= arr.size()) arr.resize(idx + 1);
    return arr[idx];
}

const Value& Value::operator[](size_t idx) const {
    static Value nil;
    if (!is_array()) return nil;
    auto& arr = get_array();
    return idx < arr.size() ? arr[idx] : nil;
}

// Missing dump() implementation
std::string Value::dump() const {
    std::ostringstream oss;
    dump_to(oss);
    return oss.str();
}

// Value class non-inline methods
Value::Value() : type_(ValueType::Null) { new (&mem_) Null(nullptr); }
Value::Value(std::nullptr_t) : type_(ValueType::Null) { new (&mem_) Null(nullptr); }
Value::Value(bool v) : type_(ValueType::Bool) { new (&mem_) bool(v); }
Value::Value(int v) : type_(ValueType::Float) { new (&mem_) double(v); }
Value::Value(double v) : type_(ValueType::Float) { new (&mem_) double(v); }
Value::Value(const char* v) : type_(ValueType::String) { new (&mem_) std::string(v); }
Value::Value(std::string v) : type_(ValueType::String) { new (&mem_) std::string(std::move(v)); }
Value::Value(Array v) : type_(ValueType::Array) { new (&mem_) Array(std::move(v)); }
Value::Value(Object v) : type_(ValueType::Object) { new (&mem_) Object(std::move(v)); }

Value::Value(const Value& other) : type_(other.type_) { copy_from(other); }
Value::Value(Value&& other) noexcept : type_(other.type_) { move_from(std::move(other)); }

Value& Value::operator=(const Value& other) {
    if (this != &other) { destroy(); type_ = other.type_; copy_from(other); }
    return *this;
}
Value& Value::operator=(Value&& other) noexcept {
    if (this != &other) { destroy(); type_ = other.type_; move_from(std::move(other)); }
    return *this;
}

Value::~Value() { destroy(); }

void Value::destroy() {
    switch (type_) {
        case ValueType::String: reinterpret_cast<std::string*>(&mem_)->~basic_string(); break;
        case ValueType::Array:  reinterpret_cast<Array*>(&mem_)->~vector(); break;
        case ValueType::Object: reinterpret_cast<Object*>(&mem_)->~map(); break;
        default: break;
    }
}

void Value::copy_from(const Value& other) {
    switch (other.type_) {
        case ValueType::Null:   new (&mem_) Null(nullptr); break;
        case ValueType::Bool:   new (&mem_) bool(other.get_bool()); break;
        case ValueType::Float:  new (&mem_) double(other.get_double()); break;
        case ValueType::String: new (&mem_) std::string(other.get_string()); break;
        case ValueType::Array:  new (&mem_) Array(other.get_array()); break;
        case ValueType::Object: new (&mem_) Object(other.get_object()); break;
    }
}

void Value::move_from(Value&& other) {
    switch (other.type_) {
        case ValueType::Null:   new (&mem_) Null(nullptr); break;
        case ValueType::Bool:   new (&mem_) bool(other.get_bool()); break;
        case ValueType::Float:  new (&mem_) double(other.get_double()); break;
        case ValueType::String: new (&mem_) std::string(std::move(other.get_string())); break;
        case ValueType::Array:  new (&mem_) Array(std::move(*reinterpret_cast<Array*>(&other.mem_))); break;
        case ValueType::Object: new (&mem_) Object(std::move(*reinterpret_cast<Object*>(&other.mem_))); break;
    }
    other.destroy();
    other.type_ = ValueType::Null;
}

void Value::dump_to(std::ostringstream& oss) const {
    switch (type_) {
        case ValueType::Null: oss << "null"; break;
        case ValueType::Bool: oss << (get_bool() ? "true" : "false"); break;
        case ValueType::Float: {
            double v = get_double();
            if (v == static_cast<long long>(v) && (v > -1e15 && v < 1e15))
                oss << static_cast<long long>(v);
            else
                oss << v;
            break;
        }
        case ValueType::String: oss << '"' << esc(get_string()) << '"'; break;
        case ValueType::Array: {
            oss << '[';
            auto& arr = get_array();
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0) oss << ',';
                arr[i].dump_to(oss);
            }
            oss << ']';
            break;
        }
        case ValueType::Object: {
            oss << '{';
            auto& obj = get_object();
            bool first = true;
            for (auto& [k, v] : obj) {
                if (!first) oss << ',';
                first = false;
                oss << '"' << esc(k) << "\":";
                v.dump_to(oss);
            }
            oss << '}';
            break;
        }
    }
}

std::string Value::esc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c;
        }
    }
    return out;
}

} // namespace rpc_json
