// Dumps the object ids, side ids and plane strings of a VMF as TSV for the srctools
// cross-check (tools/srctools_crosscheck.py). Not shipped.
#include "document/vmf.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {

using namespace apex::vmf;

std::string idOf(const Block& block) {
    const auto* id = block.find("id");
    return id ? std::string(id->raw()) : std::string();
}

void solid(const Solid& s) {
    std::printf("solid\t%s\n", idOf(s).c_str());
    for (const auto& side : s.sides) {
        const auto* plane = side.plane();
        std::printf("side\t%s\t%s\n", idOf(side).c_str(),
                    plane ? std::string(plane->raw()).c_str() : "");
    }
}

void entity(const Entity&);

template <typename T> void container(const T& c) {
    for (const auto& s : c.solids) solid(s);
    for (const auto& g : c.groups) container(g);
    if constexpr (requires { c.hidden; }) {
        for (const auto& h : c.hidden) container(h);
    }
    if constexpr (requires { c.entities; }) {
        for (const auto& e : c.entities) entity(e);
    }
}

void entity(const Entity& e) {
    std::printf("entity\t%s\n", idOf(e).c_str());
    container(e);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: apex_vmf_dump file.vmf\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto text = buffer.str();
    auto file = parse(text);
    if (!file) {
        file = parse(text, {apex::kv1::EscapeMode::Legacy});
    }
    if (!file) {
        std::fprintf(stderr, "parse failed: %s\n", file.error().what().c_str());
        return 1;
    }
    if (file->world) {
        container(*file->world);
    }
    for (const auto& e : file->entities) entity(e);
    for (const auto& g : file->groups) container(g);
    for (const auto& h : file->hidden) container(h);
    return 0;
}
