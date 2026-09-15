#include "pcapreplay/nmos/json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pcapreplay::nmos {
namespace {

const Json kNull{};

void writeIndent(std::string& out, int indent, int depth) {
    if (indent < 0) return;
    out += '\n';
    out.append(std::size_t(indent * depth), ' ');
}

}  // namespace

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    o += buf;
                } else {
                    o += char(c);
                }
        }
    }
    return o;
}

bool Json::asBool(bool fallback) const {
    if (type_ == Type::Bool) return b_;
    if (type_ == Type::Int)  return i_ != 0;
    return fallback;
}

std::int64_t Json::asInt(std::int64_t fallback) const {
    if (type_ == Type::Int)  return i_;
    if (type_ == Type::Real) return std::int64_t(d_);
    if (type_ == Type::Bool) return b_ ? 1 : 0;
    return fallback;
}

double Json::asReal(double fallback) const {
    if (type_ == Type::Real) return d_;
    if (type_ == Type::Int)  return double(i_);
    return fallback;
}

std::string Json::asString(const std::string& fallback) const {
    return type_ == Type::String ? s_ : fallback;
}

Json& Json::operator[](const std::string& key) {
    type_ = Type::Object;
    for (auto& kv : obj_)
        if (kv.first == key) return kv.second;
    obj_.emplace_back(key, Json{});
    return obj_.back().second;
}

const Json& Json::at(const std::string& key) const {
    for (const auto& kv : obj_)
        if (kv.first == key) return kv.second;
    return kNull;
}

bool Json::has(const std::string& key) const {
    for (const auto& kv : obj_)
        if (kv.first == key) return true;
    return false;
}

void Json::erase(const std::string& key) {
    obj_.erase(std::remove_if(obj_.begin(), obj_.end(),
                              [&](const auto& kv) { return kv.first == key; }),
               obj_.end());
}

void Json::write(std::string& out, int indent, int depth) const {
    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += b_ ? "true" : "false"; break;
        case Type::Int:    out += std::to_string(i_); break;
        case Type::Real: {
            // Round-trippable and free of the exponent forms that trip up
            // strict JSON schema validators.
            char buf[40];
            std::snprintf(buf, sizeof buf, "%.17g", d_);
            out += buf;
            break;
        }
        case Type::String: out += '"'; out += jsonEscape(s_); out += '"'; break;
        case Type::Array:
            if (arr_.empty()) { out += "[]"; break; }
            out += '[';
            for (std::size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                writeIndent(out, indent, depth + 1);
                arr_[i].write(out, indent, depth + 1);
            }
            writeIndent(out, indent, depth);
            out += ']';
            break;
        case Type::Object:
            if (obj_.empty()) { out += "{}"; break; }
            out += '{';
            for (std::size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                writeIndent(out, indent, depth + 1);
                out += '"';
                out += jsonEscape(obj_[i].first);
                out += indent < 0 ? "\":" : "\": ";
                obj_[i].second.write(out, indent, depth + 1);
            }
            writeIndent(out, indent, depth);
            out += '}';
            break;
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    write(out, indent, 0);
    return out;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace {

struct Parser {
    const std::string& t;
    std::size_t p = 0;
    std::string err;

    explicit Parser(const std::string& text) : t(text) {}

    void ws() {
        while (p < t.size() && (t[p] == ' ' || t[p] == '\t' ||
                                t[p] == '\n' || t[p] == '\r')) ++p;
    }
    bool fail(const char* m) {
        if (err.empty())
            err = std::string(m) + " at offset " + std::to_string(p);
        return false;
    }

    bool literal(const char* lit) {
        const std::size_t n = std::char_traits<char>::length(lit);
        if (t.compare(p, n, lit) != 0) return fail("bad literal");
        p += n;
        return true;
    }

    bool hex4(unsigned& out) {
        if (p + 4 > t.size()) return fail("truncated \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t[p + std::size_t(i)];
            out <<= 4;
            if      (c >= '0' && c <= '9') out |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') out |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= unsigned(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        p += 4;
        return true;
    }

    void utf8(std::string& s, unsigned cp) {
        if (cp < 0x80) { s += char(cp); }
        else if (cp < 0x800) {
            s += char(0xC0 | (cp >> 6));
            s += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += char(0xE0 | (cp >> 12));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        } else {
            s += char(0xF0 | (cp >> 18));
            s += char(0x80 | ((cp >> 12) & 0x3F));
            s += char(0x80 | ((cp >> 6) & 0x3F));
            s += char(0x80 | (cp & 0x3F));
        }
    }

    bool string(std::string& out) {
        if (p >= t.size() || t[p] != '"') return fail("expected string");
        ++p;
        out.clear();
        while (p < t.size()) {
            const char c = t[p++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p >= t.size()) break;
            const char e = t[p++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    // Surrogate pair.
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 1 < t.size() &&
                        t[p] == '\\' && t[p + 1] == 'u') {
                        p += 2;
                        unsigned lo = 0;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            utf8(out, cp), cp = lo;
                    }
                    utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool value(Json& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        ws();
        if (p >= t.size()) return fail("unexpected end");
        const char c = t[p];
        if (c == '{') {
            ++p;
            out = Json::object();
            ws();
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (p >= t.size() || t[p] != ':') return fail("expected ':'");
                ++p;
                Json v;
                if (!value(v, depth + 1)) return false;
                out[key] = std::move(v);
                ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            out = Json::array();
            ws();
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            for (;;) {
                Json v;
                if (!value(v, depth + 1)) return false;
                out.push(std::move(v));
                ws();
                if (p < t.size() && t[p] == ',') { ++p; continue; }
                if (p < t.size() && t[p] == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!string(s)) return false;
            out = Json(std::move(s));
            return true;
        }
        if (c == 't') { if (!literal("true"))  return false; out = Json(true);  return true; }
        if (c == 'f') { if (!literal("false")) return false; out = Json(false); return true; }
        if (c == 'n') { if (!literal("null"))  return false; out = Json();      return true; }

        // Number.
        const std::size_t start = p;
        if (p < t.size() && (t[p] == '-' || t[p] == '+')) ++p;
        bool real = false;
        while (p < t.size()) {
            const char d = t[p];
            if (d >= '0' && d <= '9') { ++p; continue; }
            if (d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') {
                real = true; ++p; continue;
            }
            break;
        }
        if (p == start) return fail("unexpected character");
        const std::string num = t.substr(start, p - start);
        if (real) out = Json(std::strtod(num.c_str(), nullptr));
        else      out = Json(std::int64_t(std::strtoll(num.c_str(), nullptr, 10)));
        return true;
    }
};

}  // namespace

Json Json::parse(const std::string& text, std::string& error) {
    Parser ps(text);
    Json out;
    if (!ps.value(out, 0)) {
        error = ps.err.empty() ? "invalid JSON" : ps.err;
        return Json{};
    }
    ps.ws();
    if (ps.p != text.size()) {
        error = "trailing data at offset " + std::to_string(ps.p);
        return Json{};
    }
    error.clear();
    return out;
}

}  // namespace pcapreplay::nmos
