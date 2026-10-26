#pragma once

#include "editor/connection_validation.hpp"

#include <span>

namespace apex::editor {

struct IncomingConnection {
    document::NodeId source = 0;
    std::size_t index = 0;
    std::string output;
    document::Connection connection;
};

std::vector<IncomingConnection> incomingConnections(const ConnectionValidator& validator,
                                                     const document::Document& doc,
                                                     std::span<const document::NodeId> targets);

} // namespace apex::editor
