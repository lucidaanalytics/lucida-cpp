// Minimal JSON reader for gateway responses (flag evaluation). No exceptions: parse_json returns false on bad input.
// Numbers without a fraction or exponent that fit int64 become int64, everything else double. Nesting is capped.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "lucida/lucida.hpp"

namespace lucida {
namespace detail {

class JsonReader {
public:
    explicit JsonReader(const std::string& s) : s_(s) {}

    bool parse(Value& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        return i_ == s_.size();
    }

private:
    static constexpr int kMaxNesting = 32;

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) i_++;
    }

    bool lit(const char* word) {
        size_t n = 0;
        while (word[n]) n++;
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n;
        return true;
    }

    bool value(Value& out, int depth) {
        if (i_ >= s_.size() || depth > kMaxNesting) return false;
        const char c = s_[i_];
        if (c == '{') return object(out, depth);
        if (c == '[') return list(out, depth);
        if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            out = Value(std::move(str));
            return true;
        }
        if (c == 't') { out = Value(true); return lit("true"); }
        if (c == 'f') { out = Value(false); return lit("false"); }
        if (c == 'n') { out = Value(); return lit("null"); }
        return number(out);
    }

    bool number(Value& out) {
        const size_t start = i_;
        bool integral = true;
        if (i_ < s_.size() && s_[i_] == '-') i_++;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '.' || c == 'e' || c == 'E') integral = false;
            else if (!(c >= '0' && c <= '9') && c != '-' && c != '+') break;
            i_++;
        }
        if (i_ == start) return false;
        const std::string tok = s_.substr(start, i_ - start);
        char* end = nullptr;
        if (integral && tok.size() < 19) {
            out = Value(static_cast<int64_t>(std::strtoll(tok.c_str(), &end, 10)));
        } else {
            out = Value(std::strtod(tok.c_str(), &end));
        }
        return end && *end == '\0';
    }

    static void utf8(std::string& out, uint32_t cp) {
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

    bool hex4(uint32_t& v) {
        if (i_ + 4 > s_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        i_++; // opening quote
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size()) return false;
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
                uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00) { // surrogate pair
                    uint32_t lo;
                    if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return false;
                    i_ += 2;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool list(Value& out, int depth) {
        i_++;
        List l;
        ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            i_++;
            out = Value(std::move(l));
            return true;
        }
        for (;;) {
            Value item;
            ws();
            if (!value(item, depth + 1)) return false;
            l.push_back(std::move(item));
            ws();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { i_++; continue; }
            if (s_[i_] == ']') { i_++; break; }
            return false;
        }
        out = Value(std::move(l));
        return true;
    }

    bool object(Value& out, int depth) {
        i_++;
        Object o;
        ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            i_++;
            out = Value(std::move(o));
            return true;
        }
        for (;;) {
            ws();
            if (i_ >= s_.size() || s_[i_] != '"') return false;
            std::string key;
            if (!string(key)) return false;
            ws();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            i_++;
            ws();
            Value item;
            if (!value(item, depth + 1)) return false;
            o.emplace_back(std::move(key), std::move(item));
            ws();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { i_++; continue; }
            if (s_[i_] == '}') { i_++; break; }
            return false;
        }
        out = Value(std::move(o));
        return true;
    }

    const std::string& s_;
    size_t i_ = 0;
};

inline bool parse_json(const std::string& text, Value& out) { return JsonReader(text).parse(out); }

/// The member `key` of an object value, or nullptr.
inline const Value* member(const Value& v, const std::string& key) {
    const Object* o = std::get_if<Object>(&v.v);
    if (!o) return nullptr;
    for (const auto& kv : *o)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

} // namespace detail
} // namespace lucida
