#pragma once

#include <array>
#include <string>
#include <string_view>

namespace apex::document {

enum class ConnectionField { Target, Input, Parameter, Delay, Times };
struct Connection {
    // Editing a malformed row and serializing it normalizes it to the five codec fields.
    std::string target, input, parameter, delay, times;
    char separator = '\x1b';
    std::string raw;
    bool wellFormed = true;

    std::array<std::string, 5> fields() const;
    void setField(ConnectionField field, std::string text);

private:
    friend Connection parseConnection(std::string_view text);
    friend std::string serialize(const Connection& connection);
    std::array<std::string, 5> original_;
    bool parsed_ = false;
};

Connection parseConnection(std::string_view text);
std::string serialize(const Connection& connection);

} // namespace apex::document
