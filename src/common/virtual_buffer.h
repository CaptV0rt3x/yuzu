// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <type_traits>
#include <utility>

namespace Common {

void* AllocateMemoryPages(std::size_t size) noexcept;
void FreeMemoryPages(void* base, std::size_t size) noexcept;

template <typename T>
class VirtualBuffer final {
public:
    static_assert(
        std::is_trivially_constructible_v<T>,
        "T must be trivially constructible, as non-trivial constructors will not be executed "
        "with the current allocator");

    constexpr VirtualBuffer() = default;
    explicit VirtualBuffer(std::size_t count) : alloc_size{count * sizeof(T)} {
        base_ptr = reinterpret_cast<T*>(AllocateMemoryPages(alloc_size));
    }

    ~VirtualBuffer() noexcept {
        FreeMemoryPages(base_ptr, alloc_size);
    }

    VirtualBuffer(const VirtualBuffer&) = delete;
    VirtualBuffer& operator=(const VirtualBuffer&) = delete;

    VirtualBuffer(VirtualBuffer&& other) noexcept
        : alloc_size{std::exchange(other.alloc_size, 0)}, base_ptr{std::exchange(other.base_ptr),
                                                                   nullptr} {}

    VirtualBuffer& operator=(VirtualBuffer&& other) noexcept {
        alloc_size = std::exchange(other.alloc_size, 0);
        base_ptr = std::exchange(other.base_ptr, nullptr);
        return *this;
    }

    void resize(std::size_t count) {
        FreeMemoryPages(base_ptr, alloc_size);

        alloc_size = count * sizeof(T);
        base_ptr = reinterpret_cast<T*>(AllocateMemoryPages(alloc_size));
    }

    [[nodiscard]] constexpr const T& operator[](std::size_t index) const {
        return base_ptr[index];
    }

    [[nodiscard]] constexpr T& operator[](std::size_t index) {
        return base_ptr[index];
    }

    [[nodiscard]] constexpr T* data() {
        return base_ptr;
    }

    [[nodiscard]] constexpr const T* data() const {
        return base_ptr;
    }

    [[nodiscard]] constexpr std::size_t size() const {
        return alloc_size / sizeof(T);
    }

private:
    std::size_t alloc_size{};
    T* base_ptr{};
};

} // namespace Common
