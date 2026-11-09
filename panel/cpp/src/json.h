// Minimalny JSON zgodny z tym, co czyta i pisze modul json Pythona - config.json
// jest wspolny dla obu wersji panelu.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wb::json {

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Value {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };
    using Member = std::pair<std::string, Value>;

    Value() = default;
    Value(bool v) : type_(Type::Bool), b_(v) {}
    Value(int v) : type_(Type::Int), i_(v) {}
    Value(int64_t v) : type_(Type::Int), i_(v) {}
    Value(double v) : type_(Type::Double), d_(v) {}
    Value(const char* v) : type_(Type::String), s_(v) {}
    Value(std::string v) : type_(Type::String), s_(std::move(v)) {}

    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }

    bool as_bool() const { return b_; }
    int64_t as_int() const { return i_; }
    double as_double() const { return d_; }
    const std::string& as_string() const { return s_; }
    const std::vector<Value>& items() const { return a_; }
    const std::vector<Member>& members() const { return o_; }

    // Klucz obiektu; przy powtorzonych wygrywa ostatni, jak w Pythonie.
    const Value* find(std::string_view key) const;
    bool contains(std::string_view key) const { return find(key) != nullptr; }

    void push(Value v) { a_.push_back(std::move(v)); }
    void set(std::string key, Value v) { o_.emplace_back(std::move(key), std::move(v)); }

    // Odpowiedniki bool(v), int(v) i float(v) z Pythona. Gdy Python rzucilby
    // wyjatkiem: to_int zwraca `fallback`, a to_double - NaN.
    bool truthy() const;
    int64_t to_int(int64_t fallback) const;
    double to_double() const;
    std::string to_string_or(std::string fallback) const;

private:
    Type type_ = Type::Null;
    bool b_ = false;
    int64_t i_ = 0;
    double d_ = 0.0;
    std::string s_;
    std::vector<Value> a_;
    std::vector<Member> o_;
};

Value parse(std::string_view text);

// Jak json.dumps(v, indent=2, ensure_ascii=False).
std::string dump(const Value& v);

// Czy bajty sa poprawnym UTF-8 (Python odrzuca inne przy odczycie pliku).
bool is_valid_utf8(std::string_view s);

} // namespace wb::json
