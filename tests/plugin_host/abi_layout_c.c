/* Compile-time ABI layout check, C side (the C++ side is abi_layout_cpp.cpp; both read abi_layout.inc).
   Every struct of the plugin header is laid out without implicit padding, so its layout is the same on LP64
   (Linux, macOS) and LLP64 (Windows): only fixed-width integers, double, size_t and pointers are used, and
   the header asserts that pointers are 64 bits wide. A struct that gains a field or loses a member's natural
   alignment fails here until the list and the golden sizes are updated on purpose. */
#include <apex/apex_plugin.h>

#define ABI_HEADER(T, has_size) enum { T##_has_size = has_size };
#define ABI_FIRST(T, a) _Static_assert(offsetof(T, a) == 0, #T " starts at 0");
#define ABI_NEXT(T, a, b) _Static_assert(offsetof(T, b) == offsetof(T, a) + sizeof(((T*)0)->a), #T "." #b " follows " #a " without padding");
#define ABI_LAST(T, a, golden) \
    _Static_assert(sizeof(T) == offsetof(T, a) + sizeof(((T*)0)->a), #T " has tail padding"); \
    _Static_assert(sizeof(T) == (golden), #T " changed size");
#include "abi_layout.inc"
#undef ABI_HEADER
#undef ABI_FIRST
#undef ABI_NEXT
#undef ABI_LAST

/* Number of structs checked, compared with the C++ side by the host tests. */
#define ABI_HEADER(T, has_size) + 1
#define ABI_FIRST(T, a)
#define ABI_NEXT(T, a, b)
#define ABI_LAST(T, a, golden)
int apex_abi_layout_count_c(void) { return 0
#include "abi_layout.inc"
    ; }
