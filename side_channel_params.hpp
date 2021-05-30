#pragma once

#include <chrono>
#include <bitset>
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

namespace params
{

/// One bit of the raw signal takes this much time to transmit.
static constexpr std::chrono::milliseconds BitPeriod{64};

/// The pseudorandom CDMA code unique to this transmitter. Should be shared between TX and RX.
/// Longer code improves SNR. Shorter code increases the data rate.
static constexpr auto CMDACodeLength = 64U;
static constexpr std::bitset<CMDACodeLength> CDMACode{0x86505beb097bf056ULL};

}
}
