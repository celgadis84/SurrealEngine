#pragma once

#ifdef WIN32
#ifndef WINVER
#define WINVER 0x0A00 // Windows 10
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <Windows.h>
#undef PlaySound
#undef DrawText
#undef FindWindow
#undef MessageBox
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include "Utils/Array.h"
#include "Utils/Exception.h"

#if defined(__N64__)
// N64 port: big endian CPU, packages are little endian. Works for integers and floats.
#include <cstring>
#include <type_traits>
template<typename T> static inline T SE_ByteSwap(T v)
{
	static_assert(sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8, "SE_ByteSwap size");
	if constexpr (sizeof(T) == 2) { uint16_t u; std::memcpy(&u, &v, 2); u = __builtin_bswap16(u); std::memcpy(&v, &u, 2); }
	else if constexpr (sizeof(T) == 4) { uint32_t u; std::memcpy(&u, &v, 4); u = __builtin_bswap32(u); std::memcpy(&v, &u, 4); }
	else { uint64_t u; std::memcpy(&u, &v, 8); u = __builtin_bswap64(u); std::memcpy(&v, &u, 8); }
	return v;
}
#define BSWAP16(i) SE_ByteSwap(i)
#define BSWAP32(i) SE_ByteSwap(i)
#define BSWAP64(i) SE_ByteSwap(i)
#else
// TODO: if we eventually support big endian platforms, these need to be implemented properly
#define BSWAP16(i) i
#define BSWAP32(i) i
#define BSWAP64(i) i
#endif

#if (defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))) || ((defined(__i386__) || defined(__x86_64__) || defined(__e2k__)) && defined(__SSE2__))
#define USE_SSE2
#else
#define NOSSE
#endif

#if defined(__ARM_NEON) || defined(_M_ARM64)
#define USE_NEON
#else
#define NONEON
#endif
