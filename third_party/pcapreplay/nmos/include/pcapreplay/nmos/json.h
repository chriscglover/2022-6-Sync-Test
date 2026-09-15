// A small JSON value type: enough for IS-04 and IS-05, and nothing more.
//
// Both directions are needed. IS-04 registration and the Node API only have to
// *emit* JSON, which a string builder would cover, but IS-05 has to *accept* it
// -- a controller routing this sender PATCHes a staged transport params object
// at us and we have to read it back out. So there is a parser here too.
//
// Deliberately not a general-purpose library: no number formatting options, no
// comments, no streaming. Object key order is preserved, because reading a raw
// NMOS response with the fields in a sensible order is a real debugging aid.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pcapreplay::nmos {

class Json {
public:
    enum class Type { Null, Bool, Int, Real, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v)                : type_(Type::Bool), b_(v) {}
    Json(int v)                 : type_(Type::Int), i_(v) {}
    Json(std::int64_t v)        : type_(Type::Int), i_(v) {}
    Json(std::uint64_t v)       : type_(Type::Int), i_(std::int64_t(v)) {}
    Json(double v)              : type_(Type::Real), d_(v) {}
    Json(const char* v)         : type_(Type::String), s_(v ? v : "") {}
    Json(std::string v)         : type_(Type::String), s_(std::move(v)) {}

    static Json array()  { Json j; j.type_ = Type::Array;  return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }
    static Json array(std::initializer_list<Json> items) {
        Json j = array();
        j.arr_.assign(items.begin(), items.end());
        return j;
    }

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Int || type_ == Type::Real; }
    bool isBool()   const { return type_ == Type::Bool; }

    bool         asBool(bool fallback = false) const;
    std::int64_t asInt(std::int64_t fallback = 0) const;
    double       asReal(double fallback = 0.0) const;
    std::string  asString(const std::string& fallback = {}) const;

    // Object access. operator[] inserts, has()/at() do not.
    Json&       operator[](const std::string& key);
    const Json& at(const std::string& key) const;
    bool        has(const std::string& key) const;
    void        erase(const std::string& key);
    const std::vector<std::pair<std::string, Json>>& items() const { return obj_; }

    // Array access.
    void  push(Json v) { type_ = Type::Array; arr_.push_back(std::move(v)); }
    std::size_t size() const { return isArray() ? arr_.size() : obj_.size(); }
    const std::vector<Json>& elements() const { return arr_; }
    Json& element(std::size_t i) { return arr_[i]; }

    std::string dump(int indent = -1) const;

    // Returns a Null value with `error` set on failure.
    static Json parse(const std::string& text, std::string& error);

private:
    void write(std::string& out, int indent, int depth) const;

    Type         type_ = Type::Null;
    bool         b_ = false;
    std::int64_t i_ = 0;
    double       d_ = 0.0;
    std::string  s_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

std::string jsonEscape(const std::string& s);

}  // namespace pcapreplay::nmos
