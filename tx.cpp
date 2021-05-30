/// Pavel Kirienko <pavel@uavcan.org>
/// Distributed under the terms of the MIT license.
/// g++ -std=c++17 -O2 -Wall tx.cpp -lpthread -o tx && ./tx

#include "side_channel_params.hpp"
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

static void drivePHY(const bool level, const std::chrono::nanoseconds duration)
{
    // Use delta relative to fixed state to avoid accumulation of phase error, because phase error attenuates the
    // useful signal at the receiver.
    static auto deadline = std::chrono::steady_clock::now();
    deadline += duration;
    if (level)
    {
        const auto loop = []() {
            while (std::chrono::steady_clock::now() < deadline)
            {
                // This busyloop is only needed to generate dummy CPU load between possibly blocking calls to now().
                volatile std::uint16_t i = 1;
                while (i != 0)
                {
                    i = i + 1U;
                }
            }
        };
        const auto thread_count = std::min<unsigned>(MAX_CONCURRENCY, std::thread::hardware_concurrency());
        if (thread_count > 1U)
        {
            std::vector<std::thread> pool;
            for (auto i = 0U; i < thread_count; i++)
            {
                pool.emplace_back(loop);
            }
            for (auto& t : pool)
            {
                t.join();
            }
        }
        else
        {
            loop();
        }
    }
    else
    {
        std::this_thread::sleep_for(deadline - std::chrono::steady_clock::now());
    }
}

static void emitBit(const bool value)
{
    using side_channel::params::ChipPeriod;
    using side_channel::params::CDMACode;

    for (auto i = 0U; i < CDMACode.size(); i++)
    {
        const bool code_position = CDMACode[i];
        const bool bit = value ? code_position : !code_position;
        drivePHY(bit, ChipPeriod);
    }
}

/// Each byte is preceded by a single high start bit.
static void emitByte(const std::uint8_t data)
{
    auto i = sizeof(data) * 8U;
    std::printf("byte 0x%02x\n", data);
    emitBit(1); // START BIT
    while (i --> 0)
    {
        const bool bit = (static_cast<std::uintmax_t>(data) & (1ULL << i)) != 0U;
        emitBit(bit);
    }
}

/// The delimiter shall be at least 9 zero bits long (longer is ok).
/// Longer delimiter allows the reciever to find correlation before the data transmission is started.
static void emitFrameDelimiter()
{
    std::printf("delimiter\n");
    for (auto i = 0U; i < 20; i++)
    {
        emitBit(0);
    }
}

static void emitPacket(const std::vector<std::uint8_t>& data)
{
    emitFrameDelimiter();
    std::uint16_t crc = side_channel::CRCInitial;
    for (std::uint8_t v : data)
    {
        emitByte(v);
        crc = side_channel::crcAdd(crc, v);
    }
    emitByte(static_cast<std::uint8_t>(crc >> 8U));
    emitByte(static_cast<std::uint8_t>(crc >> 0U));
    emitFrameDelimiter();
}

int main()
{
    side_channel::initThread();
    emitPacket(std::vector<std::uint8_t>({1, 2, 3}));
    return 0;
}
