/// Pavel Kirienko <pavel@uavcan.org>
/// Distributed under the terms of the MIT license.
/// g++ -std=c++17 -O2 -Wall rx.cpp -lpthread -o rx && ./rx

#include "side_channel_params.hpp"
#include <cstdio>
#include <sstream>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <type_traits>
#include <algorithm>
#include <numeric>
#include <optional>
#include <cassert>
#include <tuple>
#include <variant>
#include <cmath>

static constexpr auto OversamplingFactor = 3;
static constexpr auto SampleDuration = side_channel::params::ChipPeriod / OversamplingFactor;
static constexpr auto PHYAveragingFactor = 8;

/// Compute mean and standard deviation for the set.
template <typename S>
inline std::pair<S, S> computeMeanStdev(const std::vector<S>& cvec)
{
    const auto mean = std::accumulate(std::begin(cvec), std::end(cvec), 0.0F) / cvec.size();
    auto variance = S{};
    for (auto e : cvec)
    {
        variance += std::pow(e - mean, 2) / cvec.size();
    }
    return {mean, std::sqrt(variance)};
}

/// Returns true if the PHY is driven high by the transmitter, false otherwise.
static bool readPHY()
{
    // Use delta relative to fixed state to avoid accumulation of phase error, because phase error attenuates the
    // useful signal at the receiver. TODO: Implement automatic frequency alignment via PLL
    static auto deadline = std::chrono::steady_clock::now();
    deadline += SampleDuration;
    const auto started_at = std::chrono::steady_clock::now();

    // Run counter threads to measure ticks per unit time.
    std::vector<std::int64_t> counters;
    const auto loop = [&counters](std::uint32_t index) {
        auto& cnt = counters.at(index);
        while (std::chrono::steady_clock::now() < deadline)
        {
            cnt++;
        }
    };
    static const auto thread_count = std::max<unsigned>(1, std::min<unsigned>(MAX_CONCURRENCY,
                                                                              std::thread::hardware_concurrency()));
    if (thread_count > 1U)
    {
        counters.resize(thread_count, 0);
        std::vector<std::thread> pool;
        for (auto i = 0U; i < thread_count; i++)
        {
            pool.emplace_back(loop, i);
        }
        for (auto& t : pool)
        {
            t.join();
        }
    }
    else  // Otherwise run in the main thread to take advantage of the CPU core affinity.
    {
        counters.push_back(0);
        loop(0);
    }

    // Estimate the tick rate.
    const double elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at).count();
    const double rate = double(std::accumulate(std::begin(counters), std::end(counters), 0)) / elapsed_ns;

    // Apply high-pass filtering to eliminate DC component.
    static double rate_average = rate;
    rate_average += (rate - rate_average) / PHYAveragingFactor;

    // A smaller counter value means that the CPU time is being consumed by the sender, meaning it's the high level.
    return rate < rate_average;
}

/// Estimates correlation of the real-time input signal against the reference CDMA spread code (chip code).
/// The correlator runs a set of channels concurrently, separated by a fixed phase offset.
/// The correlation estimate ranges in [0.0, 1.0], where 0 represents uncorrelated signal, 1 for perfect correlation.
class CorrelationChannel
{
public:
    CorrelationChannel(std::vector<bool> spread_code, const std::uint32_t offset) :
        spread_code_(spread_code),
        position_(offset)
    { }

    /// The bit clock can be trivially extracted from a code phase locked CDMA link.
    /// In this implementation, the leading edge of the clock occurs near the middle of the spread code period.
    /// The clock edge lags the bit it relates to by one spread code period.
    struct Result
    {
        float correlation = 0.0F;
        bool data;
        bool clock;
    };

    Result feed(const bool sample)
    {
        std::optional<bool> received;
        if (position_ >= spread_code_.size())
        {
            updateCorrelation();
            state_ = match_hi_ > match_lo_;
            position_ = 0;
            match_hi_ = 0;
            match_lo_ = 0;
        }
        if (sample == spread_code_.at(position_))
        {
            match_hi_++;
        }
        else
        {
            match_lo_++;
        }
        position_++;
        return {
            correlation_,
            state_,
            position_ > spread_code_.size() / 2
        };
    }

    /// Diagnistic accessor. Not part of the main business logic.
    float getCorrelation() const { return correlation_; }

private:
    void updateCorrelation()
    {
        const bool hi_top = match_hi_ > match_lo_;
        const auto top = hi_top ? match_hi_ : match_lo_;
        const auto bot = hi_top ? match_lo_ : match_hi_;
        assert(top >= bot);
        assert(position_ > 0);
        correlation_ = static_cast<float>(top - bot) / static_cast<float>(position_);
    }

    const std::vector<bool> spread_code_;
    std::uint32_t position_;
    std::uint32_t match_hi_ = 0;
    std::uint32_t match_lo_ = 0;
    float correlation_ = 0.0F;
    bool state_ = false;
};

class Correlator
{
    static constexpr auto SequenceLength = side_channel::params::CDMACodeLength * OversamplingFactor;

public:
    /// The clock is recovered from the spread code along with the data.
    /// Positive values represent truth, negative values represent falsity.
    struct Result
    {
        float data  = 0.0F;
        float clock = 0.0F;  ///< active high
    };

    Correlator()
    {
        using side_channel::params::CDMACode;
        // Create the spread code sequence where each bit is expanded by the oversampling factor.
        std::vector<bool> seq;
        for (auto i = 0U; i < CDMACode.size(); i++)
        {
            for (auto j = 0U; j < OversamplingFactor; j++)
            {
                seq.push_back(CDMACode[i]);
            }
        }
        // Create the array of correlators where each item is offset by the sampling period.
        for (std::uint32_t i = 0; i < SequenceLength; i++)
        {
            channels_.emplace_back(seq, i);
        }
    }

    Result feed(const bool sample)
    {
        float data = 0.0F;
        float clock = 0.0F;
        for (auto& a : channels_)
        {
            const auto res = a.feed(sample);
            // Nonlinear weighting helps suppress noise from uncorrelated channels.
            const float weight = std::pow(res.correlation, 4.0F);
            data  += res.data  ? weight : -weight;
            clock += res.clock ? weight : -weight;
        }
        return {
            data,
            clock
        };
    }

    /// Correlation factor per each correlator.
    std::vector<float> getCorrelationVector() const
    {
        std::vector<float> out;
        std::transform(std::begin(channels_),
                       std::end(channels_),
                       std::back_insert_iterator(out),
                       [](const CorrelationChannel& x) { return x.getCorrelation(); });
        return out;
    }

    /// Performs a simple heuristic assessment of the code phase lock. This is unreliable though.
    bool isCodePhaseSynchronized(const float stdev_multiple_threshold = 5.0F) const
    {
        const auto cvec = getCorrelationVector();
        const auto [mean, stdev] = computeMeanStdev(cvec);
        const auto max = *std::max_element(std::begin(cvec), std::end(cvec));
        return (max - mean) > (stdev * stdev_multiple_threshold);
    }

private:
    std::vector<CorrelationChannel> channels_;
};

/// Reads data from the channel bit-by-bit. May read garbage if there is no carrier.
class BitReader
{
public:
    /// Blocks until the next bit is received.
    bool next()
    {
        for (;;)
        {
            const bool phy_state = readPHY();
            const auto result = correlator_.feed(phy_state);

            if (!clock_latch_ && result.clock > 0.0F)
            {
                clock_latch_ = true;
                return result.data > 0.0F;
            }

            if (clock_latch_ && result.clock < 0.0F)
            {
                clock_latch_ = false;
            }
        }
    }

    void printDiagnostics()
    {
        const auto cvec = correlator_.getCorrelationVector();
        const auto [mean, stdev] = computeMeanStdev(cvec);
        std::printf("mean=%.2f max=%.2f stdev=%.2f lock=%d | ",
                    mean,
                    *std::max_element(std::begin(cvec), std::end(cvec)),
                    stdev,
                    correlator_.isCodePhaseSynchronized());
        for (auto c : cvec)
        {
            if (c > 0.2F)  // Do not print the status of poorly correlated channels to reduce visual noise.
            {
                std::printf("%X", int(c * 16.0F));
            }
            else
            {
                std::printf(".");
            }
        }
        std::printf("\n");
        fflush(stdout);
    }

private:
    Correlator correlator_;
    bool clock_latch_ = false;
};

/// Reads symbols from the channel.
/// Each frame is delimited by the frame delimiter on each side. The delimiter is 9 consecutive zero bits.
/// Each byte within the frame is preceded by a single high start bit (which differentiates it from the delimiter).
class SymbolReader
{
public:
    struct Delimiter {};
    using Symbol = std::variant<Delimiter, std::uint8_t>;

    Symbol next()
    {
        while (true)
        {
            const bool bit = bit_reader_.next();
            std::printf("bit %d\n", bit);
            bit_reader_.printDiagnostics();
            if (remaining_bits_ >= 0)
            {
                buffer_ = (buffer_ << 1U) | bit;
                remaining_bits_--;
                if (remaining_bits_ < 0)
                {
                    return Symbol{buffer_};
                }
            }
            else if (bit)  // Detect start bit.
            {
                consecutive_zeros_ = 0;
                remaining_bits_ = 7;
                buffer_ = 0;
            }
            else  // Detect frame delimiter.
            {
                consecutive_zeros_++;
                if (consecutive_zeros_ > 8)
                {
                    remaining_bits_ = -1;
                    return Symbol{Delimiter{}};
                }
            }
        }
    }

private:
    BitReader bit_reader_;

    std::uint64_t consecutive_zeros_ = 0;
    std::uint8_t  buffer_ = 0;
    std::int8_t   remaining_bits_ = -1;
};

/// Reads full data packets from the channel.
/// Packets are delimited using the delimiter symbol and contain CRC-16-CCITT at the end (big endian; residue zero).
class PacketReader
{
    template <class Visitor, class... Variants>
    friend constexpr auto visit( Visitor&& vis, Variants&&... vars );

public:
    std::vector<std::uint8_t> next()
    {
        while (true)
        {
            const auto sym = symbol_reader_.next();
            if (const auto ret = std::visit(assembler_, sym))
            {
                return *ret;
            }
        }
    }

private:
    class FrameAssembler
    {
    public:
        std::optional<std::vector<std::uint8_t>> operator()(const SymbolReader::Delimiter&)
        {
            //std::puts("frame delimiter");
            std::optional<std::vector<std::uint8_t>> result;
            if (buffer_.size() >= 2)
            {
                std::uint16_t crc = side_channel::CRCInitial;
                for (std::uint8_t v : buffer_)
                {
                    crc = side_channel::crcAdd(crc, v);
                }
                if (0 == crc)
                {
                    buffer_.pop_back();  // Drop the CRC from the end.
                    buffer_.pop_back();
                    result.emplace(buffer_);
                }
                else
                {
                    std::puts("crc error");
                }
            }
            buffer_.clear();
            return result;
        }

        std::optional<std::vector<std::uint8_t>> operator()(const std::uint8_t data)
        {
            //std::printf("byte 0x%02x\n", data);
            buffer_.push_back(data);
            return {};
        }

    private:
        std::vector<std::uint8_t> buffer_;
    };

    SymbolReader symbol_reader_;
    FrameAssembler assembler_;
};

int main()
{
    std::cout << "SPREAD CODE LENGTH: " << side_channel::params::CDMACodeLength << " bit" << std::endl;
    std::cout << "SPREAD CHIP PERIOD: " << side_channel::params::ChipPeriod.count() * 1e-6 << " ms" << std::endl;
    side_channel::initThread();
    PacketReader reader;
    while (true)
    {
        const auto packet = reader.next();

        std::ostringstream file_name;
        file_name << std::chrono::system_clock::now().time_since_epoch().count() << ".bin";
        if (std::ofstream out_file(file_name.str(), std::ios::binary | std::ios::out); out_file)
        {
            out_file.write(reinterpret_cast<const char*>(packet.data()), packet.size());
            out_file.close();
        }
        else
        {
            std::printf("Could not open file %s\n", file_name.str().c_str());
            return 1;
        }
        std::printf("\033[91m"
                    "received valid packet of %u bytes saved into file %s\n"
                    "\033[m",
                    static_cast<unsigned>(packet.size()), file_name.str().c_str());
    }
    return 0;
}
