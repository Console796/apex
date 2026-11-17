#include <cstdint>
#include <dlfcn.h>
struct probe { std::uint32_t size; int (*add)(int, int); int (*times)(int, int); };
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) return 2;
    auto entry = reinterpret_cast<const probe* (*)()>(dlsym(module, "probe_entry"));
    if (!entry) return 3;
    const auto* table = entry();
    const bool ok = table && table->size == sizeof(*table) && table->add(7, 5) == 12 && table->times(7, 5) == 35;
    dlclose(module);
    return ok ? 0 : 4;
}
