// json.cpp — see json.h.
#include "json.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace json {

const Value* Value::find(const std::string& key) const {
    if (type != Object) return nullptr;
    auto it = obj.find(key);
    return it == obj.end() ? nullptr : &it->second;
}

std::string Value::as_string(const std::string& dflt) const {
    return type == String ? str : dflt;
}

double Value::as_number(double dflt) const {
    return type == Number ? num : dflt;
}

long long Value::as_int(long long dflt) const {
    if (type != Number || !std::isfinite(num)) return dflt;
    return (long long)num;
}

bool Value::as_bool(bool dflt) const {
    if (type == Bool)   return b;
    if (type == Number) return num != 0;
    return dflt;
}

namespace {

constexpr int MAX_DEPTH = 64;

struct Parser {
    const char* p;
    const char* end;
    std::string err;

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool fail(const char* msg) {
        if (err.empty()) err = msg;
        return false;
    }

    static void utf8_append(std::string& o, unsigned cp) {
        if (cp <= 0x7F) {
            o.push_back((char)cp);
        } else if (cp <= 0x7FF) {
            o.push_back((char)(0xC0 | (cp >> 6)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            o.push_back((char)(0xE0 | (cp >> 12)));
            o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            o.push_back((char)(0xF0 | (cp >> 18)));
            o.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            o.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(unsigned& out) {
        if (end - p < 4) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            out <<= 4;
            if (c >= '0' && c <= '9')      out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    // p points at the opening quote.
    bool parse_string(std::string& o) {
        ++p;
        o.clear();
        while (p < end) {
            unsigned char c = (unsigned char)*p++;
            if (c == '"') return true;
            if (c != '\\') {
                if (c < 0x20) return fail("control character in string");
                o.push_back((char)c);
                continue;
            }
            if (p >= end) return fail("truncated escape");
            char esc = *p++;
            switch (esc) {
                case '"':  o.push_back('"');  break;
                case '\\': o.push_back('\\'); break;
                case '/':  o.push_back('/');  break;
                case 'b':  o.push_back('\b'); break;
                case 'f':  o.push_back('\f'); break;
                case 'n':  o.push_back('\n'); break;
                case 'r':  o.push_back('\r'); break;
                case 't':  o.push_back('\t'); break;
                case 'u': {
                    unsigned cp;
                    if (!parse_hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                            return fail("unpaired surrogate");
                        p += 2;
                        unsigned lo;
                        if (!parse_hex4(lo)) return false;
                        if (lo < 0xDC00 || lo > 0xDFFF) return fail("unpaired surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return fail("unpaired surrogate");
                    }
                    utf8_append(o, cp);
                    break;
                }
                default: return fail("unknown escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(Value& v) {
        const char* start = p;
        if (p < end && *p == '-') ++p;
        if (p >= end) return fail("bad number");
        if (*p == '0') {
            ++p;
        } else if (*p >= '1' && *p <= '9') {
            while (p < end && *p >= '0' && *p <= '9') ++p;
        } else {
            return fail("bad number");
        }
        if (p < end && *p == '.') {
            ++p;
            if (p >= end || *p < '0' || *p > '9') return fail("bad number");
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < end && (*p == '+' || *p == '-')) ++p;
            if (p >= end || *p < '0' || *p > '9') return fail("bad number");
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        std::string tmp(start, (size_t)(p - start));
        v.type = Value::Number;
        v.num = std::strtod(tmp.c_str(), nullptr);
        return true;
    }

    bool parse_value(Value& v, int depth) {
        if (depth > MAX_DEPTH) return fail("nesting too deep");
        skip_ws();
        if (p >= end) return fail("unexpected end of input");
        switch (*p) {
            case '{': return parse_object(v, depth + 1);
            case '[': return parse_array(v, depth + 1);
            case '"': v.type = Value::String; return parse_string(v.str);
            case 't':
                if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) {
                    p += 4; v.type = Value::Bool; v.b = true; return true;
                }
                return fail("bad literal");
            case 'f':
                if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) {
                    p += 5; v.type = Value::Bool; v.b = false; return true;
                }
                return fail("bad literal");
            case 'n':
                if (end - p >= 4 && std::memcmp(p, "null", 4) == 0) {
                    p += 4; v.type = Value::Null; return true;
                }
                return fail("bad literal");
            // Python's json.dumps() emits these for non-finite floats.
            case 'N':
                if (end - p >= 3 && std::memcmp(p, "NaN", 3) == 0) {
                    p += 3; v.type = Value::Number;
                    v.num = std::numeric_limits<double>::quiet_NaN();
                    return true;
                }
                return fail("bad literal");
            case 'I':
                if (end - p >= 8 && std::memcmp(p, "Infinity", 8) == 0) {
                    p += 8; v.type = Value::Number;
                    v.num = std::numeric_limits<double>::infinity();
                    return true;
                }
                return fail("bad literal");
            default:
                if (end - p >= 9 && std::memcmp(p, "-Infinity", 9) == 0) {
                    p += 9; v.type = Value::Number;
                    v.num = -std::numeric_limits<double>::infinity();
                    return true;
                }
                if (*p == '-' || (*p >= '0' && *p <= '9')) return parse_number(v);
                return fail("unexpected character");
        }
    }

    bool parse_object(Value& v, int depth) {
        ++p;  // '{'
        v.type = Value::Object;
        skip_ws();
        if (p < end && *p == '}') { ++p; return true; }
        while (p < end) {
            skip_ws();
            if (p >= end || *p != '"') return fail("expected object key");
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (p >= end || *p != ':') return fail("expected ':'");
            ++p;
            Value val;
            if (!parse_value(val, depth)) return false;
            v.obj[key] = std::move(val);
            skip_ws();
            if (p >= end) return fail("unterminated object");
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return fail("expected ',' or '}'");
        }
        return fail("unterminated object");
    }

    bool parse_array(Value& v, int depth) {
        ++p;  // '['
        v.type = Value::Array;
        skip_ws();
        if (p < end && *p == ']') { ++p; return true; }
        while (p < end) {
            Value val;
            if (!parse_value(val, depth)) return false;
            v.arr.push_back(std::move(val));
            skip_ws();
            if (p >= end) return fail("unterminated array");
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            return fail("expected ',' or ']'");
        }
        return fail("unterminated array");
    }
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string& err) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    out = Value();
    if (!ps.parse_value(out, 0)) {
        err = ps.err.empty() ? "invalid JSON" : ps.err;
        return false;
    }
    return true;
}

}  // namespace json
