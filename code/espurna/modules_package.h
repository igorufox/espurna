/*

MODULES: size checks of a module package, kept free of the chip so the host unit tests run them

*/

#pragma once

#include <cstdint>

namespace espurna {
namespace modules {
namespace package {

// the most RAM (or IRAM) a package may ask for: above every chip's arena, far below a size that wraps when rounded
constexpr uint32_t RamLimit { 256 * 1024 };

// rounds up to a multiple of `to`; saturates instead of wrapping, so a huge value never rounds down to 0
inline uint32_t roundUp(uint32_t value, uint32_t to) {
    const uint32_t rest = value % to;
    if (!rest) {
        return value;
    }

    const uint32_t add = to - rest;
    return (value > (UINT32_MAX - add)) ? UINT32_MAX : (value + add);
}

// [offset, offset + size) inside [lo, end): compared without the sum, no wrap past 4 GiB
inline bool inside(uint32_t offset, uint32_t size, uint32_t lo, uint32_t end) {
    return (end >= lo) && (offset >= lo) && (offset <= end) && (size <= (end - offset));
}

enum class Sizes {
    Ok,
    BadImage,
    ImageTooLarge,
    BadRam,
};

// do the sizes of a package fit an area of `area_size` bytes
inline Sizes sizesCheck(uint32_t image_size, uint32_t ram_size, uint32_t iram_size, uint32_t area_size) {
    if (!image_size || (image_size % 4)) {
        return Sizes::BadImage;
    }

    if (image_size > area_size) {
        return Sizes::ImageTooLarge;
    }

    if ((ram_size > RamLimit) || (iram_size > RamLimit)) {
        return Sizes::BadRam;
    }

    return Sizes::Ok;
}

} // namespace package
} // namespace modules
} // namespace espurna
