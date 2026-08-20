#pragma once

#include <cctype>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace minijson {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Object, Array };
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;

    Value() = default;
    explicit Value(bool v) : type_(Type::Bool), bool_(v) {}
    explicit Value(double v) : type_(Type::Number), number_(v) {}
    explicit Value(std::string v) : type_(Type::String), string_(std::move(v)) {}
    explicit Value(Object v) : type_(Type::Object), object_(std::move(v)) {}
    explicit Value(Array v) : type_(Type::Array), array_(std::move(v)) {}

    Type type() const { return type_; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_bool() const { return type_ == Type::Bool; }

    const Value* find(const std::string& key) const {
        if (!is_object()) return nullptr;
        const auto it = object_.find(key);
        return it == object_.end() ? nullptr : &it->second;
    }

    const Object& object() const { return object_; }
    const Array& array() const { return array_; }
    const std::string& string() const { return string_; }
    double number() const { return number_; }
    bool boolean() const { return bool_; }

    std::string get_string(const std::string& def = {}) const {
        return is_string() ? string_ : def;
    }
    int get_int(int def = 0) const {
        return is_number() ? static_cast<int>(number_) : def;
    }
    float get_float(float def = 0.0f) const {
        return is_number() ? static_cast<float>(number_) : def;
    }
    bool get_bool(bool def = false) const {
        return is_bool() ? bool_ : def;
    }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Object object_;
    Array array_;
};

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Value parse() {
        skip_ws();
        Value result = parse_value();
        skip_ws();
        if (pos_ != text_.size()) fail("unexpected trailing data");
        return result;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;

    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error("JSON parse error at byte " + std::to_string(pos_) + ": " + what);
    }

    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    char take() {
        if (pos_ >= text_.size()) fail("unexpected end of input");
        return text_[pos_++];
    }

    void expect(char c) {
        if (take() != c) fail(std::string("expected '") + c + "'");
    }

    Value parse_value() {
        skip_ws();
        switch (peek()) {
        case '{': return parse_object();
        case '[': return parse_array();
        case '"': return Value(parse_string());
        case 't': consume_literal("true"); return Value(true);
        case 'f': consume_literal("false"); return Value(false);
        case 'n': consume_literal("null"); return Value();
        default:
            if (peek() == '-' || std::isdigit(static_cast<unsigned char>(peek()))) return Value(parse_number());
            fail("invalid value");
        }
    }

    Value parse_object() {
        expect('{');
        skip_ws();
        Value::Object out;
        if (peek() == '}') { ++pos_; return Value(std::move(out)); }
        for (;;) {
            skip_ws();
            if (peek() != '"') fail("object key must be a string");
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            out.emplace(std::move(key), parse_value());
            skip_ws();
            const char c = take();
            if (c == '}') break;
            if (c != ',') fail("expected ',' or '}'");
        }
        return Value(std::move(out));
    }

    Value parse_array() {
        expect('[');
        skip_ws();
        Value::Array out;
        if (peek() == ']') { ++pos_; return Value(std::move(out)); }
        for (;;) {
            out.emplace_back(parse_value());
            skip_ws();
            const char c = take();
            if (c == ']') break;
            if (c != ',') fail("expected ',' or ']'");
            skip_ws();
        }
        return Value(std::move(out));
    }

    static unsigned hex_value(char c) {
        if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
        if (c >= 'a' && c <= 'f') return 10u + static_cast<unsigned>(c - 'a');
        if (c >= 'A' && c <= 'F') return 10u + static_cast<unsigned>(c - 'A');
        throw std::runtime_error("invalid unicode escape");
    }

    static void append_utf8(std::string& out, unsigned cp) {
        if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            const char c = take();
            if (c == '"') return out;
            if (c == '\\') {
                const char e = take();
                switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) fail("short unicode escape");
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) cp = (cp << 4) | hex_value(text_[pos_++]);
                    append_utf8(out, cp);
                    break;
                }
                default: fail("invalid string escape");
                }
            } else {
                if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
                out.push_back(c);
            }
        }
        fail("unterminated string");
    }

    double parse_number() {
        const size_t start = pos_;
        if (peek() == '-') ++pos_;
        if (peek() == '0') ++pos_;
        else {
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid number");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (peek() == '.') {
            ++pos_;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid fraction");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!std::isdigit(static_cast<unsigned char>(peek()))) fail("invalid exponent");
            while (std::isdigit(static_cast<unsigned char>(peek()))) ++pos_;
        }
        const std::string tmp = text_.substr(start, pos_ - start);
        char* end = nullptr;
        const double value = std::strtod(tmp.c_str(), &end);
        if (!end || *end != '\0') fail("invalid number");
        return value;
    }

    void consume_literal(const char* literal) {
        while (*literal) {
            if (take() != *literal++) fail("invalid literal");
        }
    }
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

inline const Value* path(const Value& root, const std::string& a, const std::string& b = {}) {
    const Value* first = root.find(a);
    if (!first || b.empty()) return first;
    return first->find(b);
}

inline std::string str(const Value& root, const std::string& a, const std::string& b, const std::string& def) {
    const Value* v = path(root, a, b); return v ? v->get_string(def) : def;
}
inline int integer(const Value& root, const std::string& a, const std::string& b, int def) {
    const Value* v = path(root, a, b); return v ? v->get_int(def) : def;
}
inline float number(const Value& root, const std::string& a, const std::string& b, float def) {
    const Value* v = path(root, a, b); return v ? v->get_float(def) : def;
}
inline bool boolean(const Value& root, const std::string& a, const std::string& b, bool def) {
    const Value* v = path(root, a, b); return v ? v->get_bool(def) : def;
}
inline std::vector<std::string> strings(const Value& root, const std::string& a, const std::string& b) {
    std::vector<std::string> out;
    const Value* v = path(root, a, b);
    if (!v || !v->is_array()) return out;
    for (const auto& item : v->array()) if (item.is_string()) out.push_back(item.string());
    return out;
}

} // namespace minijson
