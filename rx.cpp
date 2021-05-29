/// g++ -lpthread -std=c++17 -O2 -Wall rx.cpp -o rx && ./rx

#include "side_channel_params.hpp"
#include <cstdio>
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
static constexpr auto SampleDuration = side_channel::params::BitPeriod / OversamplingFactor;
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
    std::int64_t counter = 0;
    const auto deadline = std::chrono::steady_clock::now() + SampleDuration;
    while (std::chrono::steady_clock::now() < deadline)
    {
        counter++;
    }
    static std::int64_t counter_average = counter;
    counter_average += (counter - counter_average) / PHYAveragingFactor;
    // A smaller counter value means that the CPU time is being consumed by the sender, meaning it's the high level.
    return counter < counter_average;
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
    static constexpr auto SequenceLength = side_channel::params::CMDACodeLength * OversamplingFactor;

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

/// Reads data from the channel bit-by-bit.
class BitReader
{
public:
    /// Blocks until the next bit is received.
    bool next()
    {
        for (;;)
        {
            const bool phy_state = readPHY();  // TODO FIXME PHASE CORRECT PHY READ
            const auto result = correlator_.feed(phy_state);

            const bool synced = correlator_.isCodePhaseSynchronized();
            if (synced != synced_)
            {
                synced_ = synced;
                if (synced_)
                {
                    std::puts("SIGNAL ACQUIRED");
                }
                else
                {
                    std::puts("CARRIER LOST");
                    continue;
                }
            }

            if (synced_ && !clock_latch_ && result.clock > 0.0F)
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
                std::printf(" ");
            }
        }
        std::puts("");
    }

private:
    Correlator correlator_;
    bool clock_latch_ = false;
    bool synced_ = false;
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
                std::puts("START BIT");
                consecutive_zeros_ = 0;
                remaining_bits_ = 8;
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

int main()
{
    side_channel::initThread();
    SymbolReader reader;
    BitReader bit_reader;
    while (true)
    {
        std::printf("RX BIT %d\n", bit_reader.next());
//         const auto symbol = reader.next();
//         if (std::holds_alternative<SymbolReader::Delimiter>(symbol))
//         {
//             std::printf("DELIMITER\n");
//         }
//         else
//         {
//             const auto byte = std::get<std::uint8_t>(symbol);
//             std::printf("0x%02x\n", byte);
//         }
    }
    return 0;
}