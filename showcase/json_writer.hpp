#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace showcase::json {

struct Number {
    std::string text;
};

class Value {
public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value>;

    Value() : data_(nullptr) {}
    Value(std::nullptr_t) : data_(nullptr) {}
    Value(bool value) : data_(value) {}
    Value(const char* value) : data_(std::string(value)) {}
    Value(std::string value) : data_(std::move(value)) {}
    Value(Array value) : data_(std::move(value)) {}
    Value(Object value) : data_(std::move(value)) {}

    template <typename Integer>
        requires(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>)
    Value(Integer value) : data_(Number{std::to_string(value)}) {}

    static Value array() { return Value(Array{}); }
    static Value object() { return Value(Object{}); }

    Array& as_array() { return std::get<Array>(data_); }
    Object& as_object() { return std::get<Object>(data_); }

    Value& operator[](std::string key) {
        return as_object()[std::move(key)];
    }

    void push(Value value) { as_array().push_back(std::move(value)); }

    const auto& data() const { return data_; }

private:
    std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> data_;
};

inline void write_indent(std::ostream& output, std::size_t depth) {
    for (std::size_t index = 0; index < depth * 2; ++index) {
        output.put(' ');
    }
}

inline void write_string(std::ostream& output, const std::string& text) {
    static constexpr char hex[] = "0123456789abcdef";
    output.put('"');
    for (const unsigned char character : text) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20) {
                output << "\\u00" << hex[(character >> 4) & 0xf]
                       << hex[character & 0xf];
            } else {
                output.put(static_cast<char>(character));
            }
        }
    }
    output.put('"');
}

inline void write(std::ostream& output, const Value& value, std::size_t depth = 0) {
    const auto& data = value.data();
    if (std::holds_alternative<std::nullptr_t>(data)) {
        output << "null";
        return;
    }
    if (const auto* boolean = std::get_if<bool>(&data)) {
        output << (*boolean ? "true" : "false");
        return;
    }
    if (const auto* number = std::get_if<Number>(&data)) {
        output << number->text;
        return;
    }
    if (const auto* text = std::get_if<std::string>(&data)) {
        write_string(output, *text);
        return;
    }
    if (const auto* array = std::get_if<Value::Array>(&data)) {
        if (array->empty()) {
            output << "[]";
            return;
        }
        output << "[\n";
        for (std::size_t index = 0; index < array->size(); ++index) {
            write_indent(output, depth + 1);
            write(output, array->at(index), depth + 1);
            output << (index + 1 == array->size() ? '\n' : ',');
            if (index + 1 != array->size()) {
                output << '\n';
            }
        }
        write_indent(output, depth);
        output << ']';
        return;
    }

    const auto& object = std::get<Value::Object>(data);
    if (object.empty()) {
        output << "{}";
        return;
    }
    output << "{\n";
    std::size_t index = 0;
    for (const auto& [key, child] : object) {
        write_indent(output, depth + 1);
        write_string(output, key);
        output << ": ";
        write(output, child, depth + 1);
        output << (++index == object.size() ? '\n' : ',');
        if (index != object.size()) {
            output << '\n';
        }
    }
    write_indent(output, depth);
    output << '}';
}

} // namespace showcase::json
