#include "json.h"

#include <charconv>
#include <cmath>
#include <limits>

namespace wb::json {

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Object) return nullptr;
    for (auto it = o_.rbegin(); it != o_.rend(); ++it)
        if (it->first == key) return &it->second;
    return nullptr;
}

bool Value::truthy() const {
    switch (type_) {
    case Type::Null: return false;
    case Type::Bool: return b_;
    case Type::Int: return i_ != 0;
    case Type::Double: return d_ != 0.0;
    case Type::String: return !s_.empty();
    case Type::Array: return !a_.empty();
    case Type::Object: return !o_.empty();
    }
    return false;
}

namespace {

std::string_view trim(std::string_view s) {
    const char* ws = " \t\r\n\f\v";
    auto a = s.find_first_not_of(ws);
    if (a == std::string_view::npos) return {};
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// Python pozwala na podkreslenia miedzy cyframi ("1_000").
bool strip_underscores(std::string_view in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '_') {
            bool ok = i > 0 && i + 1 < in.size() && isdigit(static_cast<unsigned char>(in[i - 1])) &&
                      isdigit(static_cast<unsigned char>(in[i + 1]));
            if (!ok) return false;
            continue;
        }
        out.push_back(c);
    }
    return true;
}

int64_t saturate(double v) {
    if (v >= 9.2e18) return std::numeric_limits<int64_t>::max();
    if (v <= -9.2e18) return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(v);
}

} // namespace

int64_t Value::to_int(int64_t fallback) const {
    switch (type_) {
    case Type::Bool: return b_ ? 1 : 0;
    case Type::Int: return i_;
    case Type::Double:
        return (std::isnan(d_) || std::isinf(d_)) ? fallback : saturate(std::trunc(d_));
    case Type::String: {
        std::string digits;
        if (!strip_underscores(trim(s_), digits) || digits.empty()) return fallback;
        const char* first = digits.data();
        const char* last = first + digits.size();
        if (*first == '+') ++first;
        if (first == last) return fallback;
        int64_t v = 0;
        auto [p, ec] = std::from_chars(first, last, v);
        if (ec == std::errc::result_out_of_range)
            return *first == '-' ? std::numeric_limits<int64_t>::min()
                                 : std::numeric_limits<int64_t>::max();
        if (ec != std::errc() || p != last) return fallback;
        return v;
    }
    default: return fallback;
    }
}

double Value::to_double() const {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    switch (type_) {
    case Type::Bool: return b_ ? 1.0 : 0.0;
    case Type::Int: return static_cast<double>(i_);
    case Type::Double: return d_;
    case Type::String: {
        std::string text;
        if (!strip_underscores(trim(s_), text) || text.empty()) return nan;
        const char* first = text.data();
        const char* last = first + text.size();
        bool negative = false;
        if (*first == '+' || *first == '-') {
            negative = *first == '-';
            ++first;
            if (first == last || *first == '-' || *first == '+') return nan;
        }
        double v = 0.0;
        auto [p, ec] = std::from_chars(first, last, v);
        if (ec == std::errc::result_out_of_range) {
            // Tak jak float("1e999") - przepelnienie to nieskonczonosc.
            v = std::numeric_limits<double>::infinity();
            for (const char* q = first; q != last; ++q)
                if ((*q == 'e' || *q == 'E') && q + 1 != last && q[1] == '-') v = 0.0;
        } else if (ec != std::errc() || p != last) {
            return nan;
        }
        return negative ? -v : v;
    }
    default: return nan;
    }
}

std::string Value::to_string_or(std::string fallback) const {
    return type_ == Type::String ? s_ : std::move(fallback);
}

// ------------------------------------------------------------------ parser

namespace {

constexpr int kMaxDepth = 512;

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    Value run() {
        ws();
        Value v = value(0);
        ws();
        if (i_ != t_.size()) fail("Extra data");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& what) const {
        size_t line = 1, col = 1;
        for (size_t k = 0; k < i_ && k < t_.size(); ++k) {
            if (t_[k] == '\n') { ++line; col = 1; } else { ++col; }
        }
        throw ParseError(what + ": line " + std::to_string(line) + " column " +
                         std::to_string(col) + " (char " + std::to_string(i_) + ")");
    }

    void ws() {
        while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\t' || t_[i_] == '\n' || t_[i_] == '\r'))
            ++i_;
    }

    bool literal(std::string_view word) {
        if (t_.substr(i_, word.size()) != word) return false;
        i_ += word.size();
        return true;
    }

    Value value(int depth) {
        if (depth > kMaxDepth) fail("Nesting too deep");
        if (i_ >= t_.size()) fail("Expecting value");
        char c = t_[i_];
        if (c == '{') return object(depth);
        if (c == '[') return array(depth);
        if (c == '"') return Value(string());
        if (literal("true")) return Value(true);
        if (literal("false")) return Value(false);
        if (literal("null")) return Value();
        if (literal("NaN")) return Value(std::numeric_limits<double>::quiet_NaN());
        if (literal("Infinity")) return Value(std::numeric_limits<double>::infinity());
        if (literal("-Infinity")) return Value(-std::numeric_limits<double>::infinity());
        if (c == '-' || (c >= '0' && c <= '9')) return number();
        fail("Expecting value");
    }

    Value object(int depth) {
        Value out = Value::object();
        ++i_;
        ws();
        if (i_ < t_.size() && t_[i_] == '}') { ++i_; return out; }
        for (;;) {
            if (i_ >= t_.size() || t_[i_] != '"')
                fail("Expecting property name enclosed in double quotes");
            std::string key = string();
            ws();
            if (i_ >= t_.size() || t_[i_] != ':') fail("Expecting ':' delimiter");
            ++i_;
            ws();
            out.set(std::move(key), value(depth + 1));
            ws();
            if (i_ < t_.size() && t_[i_] == ',') { ++i_; ws(); continue; }
            if (i_ < t_.size() && t_[i_] == '}') { ++i_; return out; }
            fail("Expecting ',' delimiter");
        }
    }

    Value array(int depth) {
        Value out = Value::array();
        ++i_;
        ws();
        if (i_ < t_.size() && t_[i_] == ']') { ++i_; return out; }
        for (;;) {
            out.push(value(depth + 1));
            ws();
            if (i_ < t_.size() && t_[i_] == ',') { ++i_; ws(); continue; }
            if (i_ < t_.size() && t_[i_] == ']') { ++i_; return out; }
            fail("Expecting ',' delimiter");
        }
    }

    unsigned hex4() {
        if (i_ + 4 > t_.size()) fail("Invalid \\uXXXX escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = t_[i_ + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else fail("Invalid \\uXXXX escape");
        }
        i_ += 4;
        return v;
    }

    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
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

    std::string string() {
        std::string out;
        ++i_;
        for (;;) {
            if (i_ >= t_.size()) fail("Unterminated string");
            unsigned char c = static_cast<unsigned char>(t_[i_]);
            if (c == '"') { ++i_; return out; }
            if (c < 0x20) fail("Invalid control character");
            if (c != '\\') { out.push_back(static_cast<char>(c)); ++i_; continue; }
            ++i_;
            if (i_ >= t_.size()) fail("Unterminated string");
            char e = t_[i_++];
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
                unsigned cp = hex4();
                if (cp >= 0xD800 && cp <= 0xDBFF && t_.substr(i_, 2) == "\\u") {
                    size_t save = i_;
                    i_ += 2;
                    unsigned lo = hex4();
                    if (lo >= 0xDC00 && lo <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else
                        i_ = save;
                }
                put_utf8(out, cp);
                break;
            }
            default: fail("Invalid \\escape");
            }
        }
    }

    Value number() {
        size_t start = i_;
        if (t_[i_] == '-') ++i_;
        if (i_ >= t_.size()) fail("Expecting value");
        if (t_[i_] == '0') {
            ++i_;
        } else if (t_[i_] >= '1' && t_[i_] <= '9') {
            while (i_ < t_.size() && isdigit(static_cast<unsigned char>(t_[i_]))) ++i_;
        } else {
            i_ = start;
            fail("Expecting value");
        }
        bool integer = true;
        if (i_ + 1 < t_.size() && t_[i_] == '.' && isdigit(static_cast<unsigned char>(t_[i_ + 1]))) {
            integer = false;
            ++i_;
            while (i_ < t_.size() && isdigit(static_cast<unsigned char>(t_[i_]))) ++i_;
        }
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            size_t k = i_ + 1;
            if (k < t_.size() && (t_[k] == '+' || t_[k] == '-')) ++k;
            if (k < t_.size() && isdigit(static_cast<unsigned char>(t_[k]))) {
                integer = false;
                i_ = k;
                while (i_ < t_.size() && isdigit(static_cast<unsigned char>(t_[i_]))) ++i_;
            }
        }
        const char* first = t_.data() + start;
        const char* last = t_.data() + i_;
        if (integer) {
            int64_t v = 0;
            auto [p, ec] = std::from_chars(first, last, v);
            if (ec == std::errc()) return Value(v);
            // Liczba za duza na 64 bity - i tak zostanie przycieta do zakresu.
            return Value(*first == '-' ? std::numeric_limits<int64_t>::min()
                                       : std::numeric_limits<int64_t>::max());
        }
        double d = 0.0;
        auto [p, ec] = std::from_chars(first, last, d);
        if (ec == std::errc::result_out_of_range) {
            d = std::numeric_limits<double>::infinity();
            if (*first == '-') d = -d;
        }
        return Value(d);
    }

    std::string_view t_;
    size_t i_ = 0;
};

// ------------------------------------------------------------------ zapis

void put_string(std::string& out, const std::string& s) {
    static const char* hex = "0123456789abcdef";
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\f': out += "\\f"; break;
        case '\b': out += "\\b"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 15]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void put_double(std::string& out, double v) {
    if (std::isnan(v)) { out += "NaN"; return; }
    if (std::isinf(v)) { out += v > 0 ? "Infinity" : "-Infinity"; return; }
    char buf[64];
    auto [p, ec] = std::to_chars(buf, buf + sizeof buf, v);
    std::string_view s(buf, static_cast<size_t>(p - buf));
    out += s;
    if (s.find_first_of(".eE") == std::string_view::npos) out += ".0";
}

void put(std::string& out, const Value& v, int indent) {
    using T = Value::Type;
    switch (v.type()) {
    case T::Null: out += "null"; break;
    case T::Bool: out += v.as_bool() ? "true" : "false"; break;
    case T::Int: out += std::to_string(v.as_int()); break;
    case T::Double: put_double(out, v.as_double()); break;
    case T::String: put_string(out, v.as_string()); break;
    case T::Array: {
        if (v.items().empty()) { out += "[]"; break; }
        out += "[\n";
        bool first = true;
        for (const Value& item : v.items()) {
            if (!first) out += ",\n";
            first = false;
            out.append(static_cast<size_t>(indent + 2), ' ');
            put(out, item, indent + 2);
        }
        out.push_back('\n');
        out.append(static_cast<size_t>(indent), ' ');
        out.push_back(']');
        break;
    }
    case T::Object: {
        if (v.members().empty()) { out += "{}"; break; }
        out += "{\n";
        bool first = true;
        for (const auto& [key, item] : v.members()) {
            if (!first) out += ",\n";
            first = false;
            out.append(static_cast<size_t>(indent + 2), ' ');
            put_string(out, key);
            out += ": ";
            put(out, item, indent + 2);
        }
        out.push_back('\n');
        out.append(static_cast<size_t>(indent), ' ');
        out.push_back('}');
        break;
    }
    }
}

} // namespace

Value parse(std::string_view text) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF")
        throw ParseError("Unexpected UTF-8 BOM (decode using utf-8-sig): line 1 column 1 (char 0)");
    return Parser(text).run();
}

std::string dump(const Value& v) {
    std::string out;
    put(out, v, 0);
    return out;
}

bool is_valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int extra;
        unsigned cp, min;
        if (c < 0x80) { ++i; continue; }
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; min = 0x10000; }
        else return false;
        if (i + extra >= s.size()) return false;
        for (int k = 1; k <= extra; ++k) {
            unsigned char n = static_cast<unsigned char>(s[i + k]);
            if ((n & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (n & 0x3F);
        }
        if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += extra + 1;
    }
    return true;
}

} // namespace wb::json
