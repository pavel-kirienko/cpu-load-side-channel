/// g++ -lpthread -std=c++17 -O2 -Wall tx.cpp -o tx && ./tx

#include "side_channel_params.hpp"
#include <cstdio>
#include <thread>
#include <vector>


static void drivePHY(const bool level, const std::chrono::microseconds duration)
{
    std::printf("%d", level);
    fflush(stdout);
    if (level)
    {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline)
        {
            (void) 0;   // This busyloop is only needed to generate dummy CPU load.
        }
    }
    else
    {
        std::this_thread::sleep_for(duration);
    }
}

static void emitBit(const bool value)
{
    using side_channel::params::BitPeriod;
    using side_channel::params::CDMACode;

    for (auto i = 0U; i < CDMACode.size(); i++)
    {
        const bool code_position = CDMACode[i];
        const bool bit = value ? code_position : !code_position;
        drivePHY(bit, BitPeriod);
    }
}

/// Each byte is preceded by a single high start bit.
static void emitByte(const std::uint8_t data)
{
    auto i = sizeof(data) * 8U;
    std::printf("byte 0x%02x; ", data);
    emitBit(1); // START BIT
    while (i --> 0)
    {
        const bool bit = (static_cast<std::uintmax_t>(data) & (1ULL << i)) != 0U;
        emitBit(bit);
        std::printf(" ");
    }
    std::puts("");
}

/// The delimiter shall be at least 9 zero bits long (longer is ok).
/// Longer delimiter allows the reciever to find correlation before the data transmission is started.
static void emitFrameDelimiter()
{
    std::printf("delimiter; ");
    for (auto i = 0U; i < 10; i++)
    {
        emitBit(0);
        std::printf(" ");
    }
    std::puts("");
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
    emitPacket(std::vector<std::uint8_t>({1, 2, 3, 4, 5}));
    return 0;
}
