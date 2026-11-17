// Compile-time ABI layout check, C++ side (see abi_layout_c.c). The same list and golden sizes must hold when the
// header is compiled as C++, and every struct must be standard layout and trivially copyable so that the
// host can copy elements and plugins can zero-initialise them.
#include <apex/apex_plugin.h>
#include <cstddef>
#include <type_traits>

namespace {
#define ABI_HEADER(T, has_size) \
    static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>, #T " must be plain data");
#define ABI_FIRST(T, a) static_assert(offsetof(T, a) == 0, #T " starts at 0");
#define ABI_NEXT(T, a, b) static_assert(offsetof(T, b) == offsetof(T, a) + sizeof(static_cast<T*>(nullptr)->a), #T "." #b " follows " #a " without padding");
#define ABI_LAST(T, a, golden) \
    static_assert(sizeof(T) == offsetof(T, a) + sizeof(static_cast<T*>(nullptr)->a), #T " has tail padding"); \
    static_assert(sizeof(T) == (golden), #T " changed size");
#include "abi_layout.inc"
#undef ABI_HEADER
#undef ABI_FIRST
#undef ABI_NEXT
#undef ABI_LAST
}

#define ABI_HEADER(T, has_size) + 1
#define ABI_FIRST(T, a)
#define ABI_NEXT(T, a, b)
#define ABI_LAST(T, a, golden)
extern "C" int apex_abi_layout_count_cpp() { return 0
#include "abi_layout.inc"
    ; }
