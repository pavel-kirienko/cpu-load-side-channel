#pragma once

#include <chrono>
#include <bitset>
#include <array>
#include <cstdint>
#include <pthread.h>

namespace side_channel
{

inline void initThread()
{
    // Force affinity with the 0th core.
    cpu_set_t cpuset{};
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

static constexpr std::uint16_t CRCInitial = 0xFFFFU;

// Naive implementation of CRC-16-CCITT
// Taken from https://github.com/UAVCAN/libcanard
inline std::uint16_t crcAdd(const std::uint16_t crc, const uint8_t byte)
{
    static constexpr std::uint16_t Top  = 0x8000U;
    static constexpr std::uint16_t Poly = 0x1021U;
    std::uint16_t out  = crc ^ (std::uint16_t)((std::uint16_t)(byte) << 8);
    out = (std::uint16_t)((std::uint16_t)(out << 1U) ^ (((out & Top) != 0U) ? Poly : 0U));
    out = (std::uint16_t)((std::uint16_t)(out << 1U) ^ (((out & Top) != 0U) ? Poly : 0U));
    out = (std::uint16_t)((std::uint16_t)(out << 1U) ^ (((out & Top) != 0U) ? Poly : 0U));
    out = (std::uint16_t)((std::uint16_t)(out << 1U) ^ (((out & Top) != 0U) ? Poly : 0U));
    out = (std::uint16_t)((std::uint16_t)(out << 1U) ^ (((out & Top) != 0U) ? Poly : 0U));
    out = (std::uint16_t)((std::uint16_t)(out << 1U) ^ (((out & Top) != 0U) ? Poly : 0U));
    out = (std::uint16_t)((std::uint16_t)(out << 1U) ^ (((out & Top) != 0U) ? Poly : 0U));
    out = (std::uint16_t)((std::uint16_t)(out << 1U) ^ (((out & Top) != 0U) ? Poly : 0U));
    return out;
}

namespace hamming_7_4
{

/// Maps received 7-bit Hamming code word to 4-bit result with error correction applied.
static constexpr std::array<std::uint8_t, 128> Decode = {{
    0x0, 0x0, 0x0, 0x3, 0x0, 0x5, 0xE, 0x7, 0x0, 0x9, 0xE, 0xB, 0xE, 0xD, 0xE, 0xE,
    0x0, 0x3, 0x3, 0x3, 0x4, 0xD, 0x6, 0x3, 0x8, 0xD, 0xA, 0x3, 0xD, 0xD, 0xE, 0xD,
    0x0, 0x5, 0x2, 0xB, 0x5, 0x5, 0x6, 0x5, 0x8, 0xB, 0xB, 0xB, 0xC, 0x5, 0xE, 0xB,
    0x8, 0x1, 0x6, 0x3, 0x6, 0x5, 0x6, 0x6, 0x8, 0x8, 0x8, 0xB, 0x8, 0xD, 0x6, 0xF,
    0x0, 0x9, 0x2, 0x7, 0x4, 0x7, 0x7, 0x7, 0x9, 0x9, 0xA, 0x9, 0xC, 0x9, 0xE, 0x7,
    0x4, 0x1, 0xA, 0x3, 0x4, 0x4, 0x4, 0x7, 0xA, 0x9, 0xA, 0xA, 0x4, 0xD, 0xA, 0xF,
    0x2, 0x1, 0x2, 0x2, 0xC, 0x5, 0x2, 0x7, 0xC, 0x9, 0x2, 0xB, 0xC, 0xC, 0xC, 0xF,
    0x1, 0x1, 0x2, 0x1, 0x4, 0x1, 0x6, 0xF, 0x8, 0x1, 0xA, 0xF, 0xC, 0xF, 0xF, 0xF
}};

/// Maps 4-bit nibble to be transmitted to 7-bit Hamming code word.
static constexpr std::array<std::uint8_t, 16> Encode = {{
    0x00, 0x71, 0x62, 0x13, 0x54, 0x25, 0x36, 0x47, 0x38, 0x49, 0x5A, 0x2B, 0x6C, 0x1D, 0x0E, 0x7F
}};

}

namespace params
{

/// One bit of the raw signal takes this much time to transmit.
static constexpr std::chrono::milliseconds BitPeriod{100};

/// The pseudorandom CDMA code unique to this transmitter. Should be shared between TX and RX.
/// Longer code improves SNR. Shorter code increases the data rate.
static constexpr auto CMDACodeLength = 64U;
static constexpr std::bitset<CMDACodeLength> CDMACode{0x86505beb097bf056ULL};

}
}
