#include "document/connection_codec.hpp"

#include <stdexcept>

namespace apex::document {

std::array<std::string, 5> Connection::fields() const {
    return {target, input, parameter, delay, times};
}
void Connection::setField(ConnectionField field, std::string text) {
    switch (field) {
    case ConnectionField::Target: target = std::move(text); break;
    case ConnectionField::Input: input = std::move(text); break;
    case ConnectionField::Parameter: parameter = std::move(text); break;
    case ConnectionField::Delay: delay = std::move(text); break;
    case ConnectionField::Times: times = std::move(text); break;
    default: throw std::invalid_argument("invalid connection field");
    }
}
Connection parseConnection(std::string_view text) {
    Connection result;
    result.raw = text;
    result.separator = text.contains('\x1b') ? '\x1b' : ',';
    std::array<std::string*, 5> fields{&result.target, &result.input, &result.parameter, &result.delay, &result.times};
    std::size_t start = 0, count = 0;
    for (;;) {
        const auto end = text.find(result.separator, start);
        if (count < fields.size()) *fields[count] = text.substr(start, end - start);
        ++count;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    result.wellFormed = count == 5;
    result.original_ = result.fields();
    result.parsed_ = true;
    return result;
}
std::string serialize(const Connection& connection) {
    const auto fields = connection.fields();
    if (connection.parsed_ && fields == connection.original_) return connection.raw;
    std::string result;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) result += connection.separator;
        result += fields[i];
    }
    return result;
}

} // namespace apex::document
