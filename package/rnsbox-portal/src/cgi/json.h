// json.h — minimal JSON reader for machine-readable CLI output (rnstatus -j).
//
// The portal only needs a small, non-validating consumer for `rnstatus --json`:
// objects, arrays, strings (including \uXXXX escapes), numbers, booleans and
// null. No serialiser — responses are built with http::json_escape().
#ifndef RNSBOX_JSON_H
#define RNSBOX_JSON_H

#include <map>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };

    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    bool is_null()   const { return type == Null; }
    bool is_bool()   const { return type == Bool; }
    bool is_number() const { return type == Number; }
    bool is_string() const { return type == String; }
    bool is_array()  const { return type == Array; }
    bool is_object() const { return type == Object; }

    // Member lookup on an object; nullptr when absent or when this isn't one.
    const Value* find(const std::string& key) const;

    std::string as_string(const std::string& dflt = "") const;
    double as_number(double dflt = 0) const;
    long long as_int(long long dflt = 0) const;
    bool as_bool(bool dflt = false) const;
};

// Parse the first JSON value in text. Trailing bytes are ignored: rnstatus's
// stdout is merged with stderr by util::run(), and warnings may precede or
// follow the document. Returns false with a short error on malformed input.
bool parse(const std::string& text, Value& out, std::string& err);

}  // namespace json

#endif
