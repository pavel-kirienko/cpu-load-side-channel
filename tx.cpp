/// Pavel Kirienko <pavel@uavcan.org>
/// Distributed under the terms of the MIT license.
/// g++ -std=c++17 -O2 -Wall tx.cpp -lpthread -o tx && ./tx

#include "side_channel_params.hpp"
#include <cstdio>
#include <iostream>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>
#include <atomic>
#include <cassert>
#include <stdexcept>

static void drivePHY(const bool level, const std::chrono::nanoseconds duration)
{
    // Use delta relative to fixed state to avoid accumulation of phase error, because phase error attenuates the
    // useful signal at the receiver.
    static auto deadline = std::chrono::steady_clock::now();
    deadline += duration;
    if (level)
    {
        std::atomic<bool> finish = false;
        const auto loop = [&finish]() {
            while (!finish)
            {
                // This busyloop is only needed to generate dummy CPU load between possibly contentious checks.
                volatile std::uint16_t i = 1;
                while (i != 0)
                {
                    i = i + 1U;
                }
            }
        };
        static const auto thread_count = std::max<unsigned>(1, std::min<unsigned>(MAX_CONCURRENCY,
                                                                                  std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        assert(thread_count > 0);
        for (auto i = 0U; i < (thread_count - 1); i++)
        {
            pool.emplace_back(loop);
        }
        while (std::chrono::steady_clock::now() < deadline)
        {
            volatile std::uint16_t i = 1;  // Dummy load in case now() is blocking.
            while (i != 0)
            {
                i = i + 1U;
            }
        }
        finish = true;
        for (auto& t : pool)
        {
            t.join();
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

static std::vector<std::uint8_t> readFile(const std::string& path)
{
    std::ifstream ifs(path, std::ios::binary);
    if (ifs)
    {
        ifs.unsetf(std::ios::skipws);
        ifs.seekg(0, std::ios::end);
        std::vector<std::uint8_t> buf;
        buf.reserve(ifs.tellg());
        ifs.seekg(0, std::ios::beg);
        buf.insert(buf.begin(), std::istream_iterator<std::uint8_t>(ifs), std::istream_iterator<std::uint8_t>());
        return buf;
    }
    throw std::logic_error("Cannot read file " + path);
}

int main(const int argc, const char* const argv[])
{
    std::cout << "SPREAD CODE LENGTH: " << side_channel::params::CDMACodeLength << " bit" << std::endl;
    std::cout << "SPREAD CHIP PERIOD: " << side_channel::params::ChipPeriod.count() * 1e-6 << " ms" << std::endl;
    if (argc < 2)
    {
        std::cerr << "Usage:\n\t" << argv[0] << " <file>" << std::endl;
        return 1;
    }
    side_channel::initThread();
    const auto data = readFile(argv[1]);
    std::cerr << "Transmitting " << data.size() << " bytes read from " << argv[1] << std::endl;
    emitPacket(data);
    return 0;
}
