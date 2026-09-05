// SPDX-License-Identifier: MIT
// diagnostics/json_writer.h — minimal dependency-free JSON emitter.

#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace blender_dlss5::diagnostics {

// Appends a JSON-escaped string (surrounded by quotes) to `out`.
void JsonEscape(std::ostringstream& out, const std::string& value);

// Small streaming builder. No structural validation: the caller builds
// well-formed documents by pairing begin/end calls.
class JsonWriter {
public:
    void begin_object();
    void end_object();
    void begin_array();
    void end_array();
    void key(const std::string& k);
    void string_value(const std::string& v);   // emits "v" (escaped)
    void int_value(int64_t v);
    void uint_value(uint64_t v);
    void double_value(double v);
    void bool_value(bool v);
    void null_value();
    std::string str() const { return stream_.str(); }

private:
    void before_value();  // comma bookkeeping for the incoming value

    std::ostringstream stream_;

    struct Frame {
        bool is_object = true;
        bool first = true;       // no element/key written yet
        bool after_key = false;  // the next value belongs to a just-written key
    };
    std::vector<Frame> stack_;
};

}  // namespace blender_dlss5::diagnostics
