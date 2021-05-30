/// Pavel Kirienko <pavel@uavcan.org>
/// Distributed under the terms of the MIT license.

#pragma once

#include <chrono>
#include <bitset>
#include <cstdint>

/// The transmitter can modulate load on all available cores to traverse virtualization boundaries that implement
/// non-direct CPU core mapping (e.g., virtual core X may be mapped to physical core Y such that X!=Y).
/// This is usually not necessary outside of virtualized environments, in which case only 0th core should be used.
#ifndef MAX_CONCURRENCY
#   define MAX_CONCURRENCY 999
#endif

#if MAX_CONCURRENCY == 1
#include <pthread.h>
#endif

namespace side_channel
{

inline void initThread()
{
#if MAX_CONCURRENCY == 1
    // Force affinity with the 0th core.
    cpu_set_t cpuset{};
    CPU_ZERO(&cpuset);
    CPU_SET(0, &cpuset);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
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

namespace params
{

/// One chip of the raw signal takes this much time to transmit.
/// Accurate timing is absolutely essential for reliability of the data link.
/// It is possible to go as low as 1 millisecond and possibly lower, resulting in very high throughput,
/// although it does make the data link somewhat unstable outside of ideal conditions.
/// Values ca. 100 ms enable robust communication under adverse conditions.
/// Increasing the spread code length also improves the SNR.
static constexpr std::chrono::nanoseconds ChipPeriod{16'000'000};

/// The pseudorandom CDMA spread code unique to this TX/RX pair.
/// In this example we use a subsequence of the 1023-bit Gold code for GPS SV#1.
/// Helpful resource: https://natronics.github.io/blag/2014/gps-prn/
static constexpr auto CDMACodeLength = 1023U;
static std::bitset<CDMACodeLength> CDMACode(
    "1100100000111001010010011110010100010011111010101101000100010101"
    "0101100100011110100111111011011100110111110010101010000100000000"
    "1110101001000100110111100000111101011100110011110110000000101111"
    "0011111010100110001011011100011011110101000101011000001000000001"
    "0000001100011101100000011100011011111111101001110100101101100001"
    "0101011000100111001011011101100011101110111100001101100001100100"
    "1001000001101101001011011110001011100000010100100111111000001010"
    "1011100111110101111100110011000111000110110101010110110001101110"
    "1110000000000010110011011001110110100000101010111010111010010100"
    "0111001110001001010001010010110100001010110110101101100011100111"
    "1011001000011111100101101000100001111101010111001100100100100101"
    "1111111110000111110111100011011100101100001110010101000010100101"
    "0111111000111101101001110110011111101111101000110001111100000001"
    "0010100010110100010001001101100000011101101000110100010010001110"
    "0010110011001001111001101111110011001010011010011010111100110110"
    "101001110111100011010100010000100010010011100001110010100010000"
);

}
}
