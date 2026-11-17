#include "plugin_host/plugin_host.hpp"
#include <filesystem>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path root(argv[1]);
    apex::plugin_host::Host host(root / "state", root / "breadcrumb");
    host.discover({root});
    host.activate();
    return 1;
}
