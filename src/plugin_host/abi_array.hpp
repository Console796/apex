#pragma once
// Reading arrays of ABI structs (include/apex/apex_plugin.h, rule 4): the stride of an array is the
// struct_size of its elements, so an element a plugin built against an older or newer header wrote is
// read at its own size. Each element is copied into the host's current layout: fields the plugin's
// struct does not cover read as zero (the old behaviour), fields the host does not know are ignored.
#include <apex/apex_plugin.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>

namespace apex::plugin_host {
// The size of T in ABI 1.0, the least struct_size an element may have. It is the current size until a
// struct grows; the growing change then pins the old value with a specialisation, so that a plugin built
// against 1.0 is still accepted.
template <class T> inline constexpr std::size_t kAbiBaseSize = sizeof(T);

// True when the first `size` bytes of a struct cover `field`: the struct_size gate of rule 2.
#define APEX_ABI_COVERS(size, Type, field) ((size) >= offsetof(Type, field) + sizeof(static_cast<const Type*>(nullptr)->field))

template <class T> class AbiArray {
public:
    static_assert(std::is_trivially_copyable_v<T> && offsetof(T, struct_size) == 0);
    static constexpr std::size_t kMaxStride = 4096;
    // `count` elements at `data`. Invalid (and empty) when the pointer is null for a non-zero count, or
    // an element's struct_size is below the base size, not a multiple of the type's alignment, over 4 KiB, or differs from
    // the first element's (the stride is shared, so a differing one means the array is not an array of T).
    AbiArray(const T* data, std::size_t count, std::size_t baseSize = kAbiBaseSize<T>) {
        if (count == 0) { valid_ = true; return; }
        if (!data) return;
        const auto* bytes = reinterpret_cast<const unsigned char*>(data);
        std::uint32_t first = 0;
        std::memcpy(&first, bytes, sizeof(first));
        if (first < baseSize || first > kMaxStride || first % alignof(T) != 0) return;
        for (std::size_t i = 1; i < count; ++i) {
            std::uint32_t size = 0;
            std::memcpy(&size, bytes + i * first, sizeof(size));
            if (size != first) return;
        }
        data_ = bytes; count_ = count; stride_ = first; valid_ = true;
    }
    bool valid() const { return valid_; }
    std::size_t size() const { return count_; }
    std::size_t stride() const { return stride_; }
    // Element `i` in the host's layout, with struct_size set to what the plugin's element covered.
    T operator[](std::size_t i) const {
        T out{};
        std::memcpy(&out, data_ + i * stride_, stride_ < sizeof(T) ? stride_ : sizeof(T));
        return out;
    }
private:
    const unsigned char* data_ = nullptr;
    std::size_t count_ = 0, stride_ = 0;
    bool valid_ = false;
};

// One struct a plugin passes by pointer (a sink call's segment or label), in the host's layout; empty when
// null or smaller than the base size.
template <class T> std::optional<T> readStruct(const T* in, std::size_t baseSize = kAbiBaseSize<T>) {
    if (!in) return std::nullopt;
    const AbiArray<T> one(in, 1, baseSize);
    if (!one.valid()) return std::nullopt;
    return one[0];
}

// An out struct the host fills for a caller built against another header: the caller's struct_size says
// which fields exist. Writes `value` into the caller's buffer, only the part the caller covers.
template <class T> bool writeOut(T* out, const T& value, std::size_t baseSize = kAbiBaseSize<T>) {
    if (!out) return false;
    std::uint32_t callerSize = 0;
    std::memcpy(&callerSize, out, sizeof(callerSize));
    if (callerSize < baseSize || callerSize > AbiArray<T>::kMaxStride) return false;
    T copy = value;
    copy.struct_size = callerSize;
    std::memcpy(out, &copy, callerSize < sizeof(T) ? callerSize : sizeof(T));
    return true;
}
}
