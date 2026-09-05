// SPDX-License-Identifier: MIT
#include "json_writer.h"

#include <cstdio>

namespace blender_dlss5::diagnostics {

void JsonEscape(std::ostringstream& out, const std::string& value) {
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out << buf;
                } else {
                    out << static_cast<char>(c);
                }
        }
    }
    out << '"';
}

void JsonWriter::begin_object() {
    before_value();
    stream_ << '{';
    stack_.push_back({true, true, false});
}

void JsonWriter::end_object() {
    stream_ << '}';
    stack_.pop_back();
}

void JsonWriter::begin_array() {
    before_value();
    stream_ << '[';
    stack_.push_back({false, true, false});
}

void JsonWriter::end_array() {
    stream_ << ']';
    stack_.pop_back();
}

void JsonWriter::key(const std::string& k) {
    Frame& f = stack_.back();
    if (f.is_object && !f.first) stream_ << ',';
    JsonEscape(stream_, k);
    stream_ << ':';
    f.first = false;
    f.after_key = true;
}

void JsonWriter::string_value(const std::string& v) {
    before_value();
    JsonEscape(stream_, v);
}

void JsonWriter::int_value(int64_t v) {
    before_value();
    stream_ << v;
}

void JsonWriter::uint_value(uint64_t v) {
    before_value();
    stream_ << v;
}

void JsonWriter::double_value(double v) {
    before_value();
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    stream_ << buf;
}

void JsonWriter::bool_value(bool v) {
    before_value();
    stream_ << (v ? "true" : "false");
}

void JsonWriter::null_value() {
    before_value();
    stream_ << "null";
}

void JsonWriter::before_value() {
    if (stack_.empty()) return;  // root: a single scalar document
    Frame& f = stack_.back();
    if (f.is_object) {
        // In an object, values always follow a key; the comma between
        // key/value pairs was already emitted by key().
        f.after_key = false;
        return;
    }
    // Array element: comma after every element but the first.
    if (!f.first) stream_ << ',';
    f.first = false;
}

}  // namespace blender_dlss5::diagnostics
