#pragma once
#include <cstdint>
#include <new>
namespace passport {
// A global constructed at startup instead of constant-initialised. GCC puts a constant-initialised object
// in .data, which stores its whole initial image in flash as well (8.9 KB for Runtime); this one lands in .bss.
template<class T> T& startupObject(){alignas(T) static uint8_t storage[sizeof(T)];return *new(storage) T;}
}
