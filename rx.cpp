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

static constexpr auto OversamplingFactor = 1;
static constexpr auto SampleDuration = side_channel::params::BitPeriod / OversamplingFactor;
static constexpr auto PHYAveragingFactor = 8;

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

class Correlator
{
public:
    Correlator(std::vector<bool> sequence, const std::uint32_t initial_position) :
        sequence_(sequence),
        position_(initial_position)
    { }

    struct Result
    {
        float correlation = 0;
        std::optional<bool> received;
    };

    Result feed(const bool sample)
    {
        std::optional<bool> received;
        if (position_ >= sequence_.size())
        {
            updateCorrelation();
            received.emplace(match_hi_ > match_lo_);
            position_ = 0;
            match_hi_ = 0;
            match_lo_ = 0;
        }
        if (sample == sequence_.at(position_))
        {
            match_hi_++;
        }
        else
        {
            match_lo_++;
        }
        position_++;
        return {correlation_, received};
    }

    // Diagnostic accessor.
    float getCorrelation() const { return correlation_; }

private:
    void updateCorrelation()
    {
        if (position_ > 0)
        {
            const bool hi_top = match_hi_ > match_lo_;
            const auto top = hi_top ? match_hi_ : match_lo_;
            const auto bot = hi_top ? match_lo_ : match_hi_;
            assert(top >= bot);
            const float new_correlation = static_cast<float>(top - bot) / static_cast<float>(position_);
            correlation_ += (new_correlation - correlation_) * 0.1f;
        }
    }

    const std::vector<bool> sequence_;
    std::uint32_t position_;
    std::uint32_t match_hi_ = 0;
    std::uint32_t match_lo_ = 0;
    float correlation_ = 0.0F;
};

class MultiCorrelator
{
    static constexpr auto SequenceLength = side_channel::params::CMDACodeLength * OversamplingFactor;

public:
    using Result = Correlator::Result;

    MultiCorrelator()
    {
        using side_channel::params::CDMACode;
        // Create the code sequence where each bit is expanded by the oversampling factor.
        std::vector<bool> seq;
        for (auto i = 0U; i < CDMACode.size(); i++)
        {
            for (auto j = 0U; j < OversamplingFactor; j++)
            {
                seq.push_back(CDMACode[i]);
                std::printf("%d", int(CDMACode[i]));
            }
        }
        std::puts("");
        // Create the array of correlators where each item is offset by the sampling period.
        for (std::uint32_t i = 0; i < SequenceLength; i++)
        {
            array_.emplace_back(seq, i);
        }
    }

    Result feed(const bool sample)
    {
        Result best;
        for (auto& a : array_)
        {
            auto res = a.feed(sample);
            if (res.correlation > best.correlation)
            {
                best = res;
            }
        }
        sample_count_++;
        // The result is not meaningful until at least two full periods are over.
        // This is because the correlators need a full period to estimate the correlation factor.
        if (sample_count_ > SequenceLength * 2)
        {
            return best;
        }
        return {};
    }

    /// Correlation factor per each correlator.
    std::vector<float> getCorrelationVector() const
    {
        std::vector<float> out;
        std::transform(std::begin(array_),
                       std::end(array_),
                       std::back_insert_iterator(out),
                       [](const Correlator& x) { return x.getCorrelation(); });
        return out;
    }

    /// How many samples have been processed so far.
    std::uint64_t getSampleCount() const { return sample_count_; }

private:
    std::vector<Correlator> array_;
    std::uint64_t sample_count_ = 0;
};

/// Reads data from the channel bit-by-bit.
class BitReader
{
public:
    /// Blocks until the next bit is received. The result is the received bit and the RSSI in [0, 1].
    std::tuple<bool, float> next()
    {
        for (;;)
        {
            const bool phy_state = readPHY();
            const auto result = multi_correlator_.feed(phy_state);

            // Print diagnostics periodically.
            if (multi_correlator_.getSampleCount() % 100 == 0)
            {
               const auto cv = multi_correlator_.getCorrelationVector();
               for (auto c : cv)
               {
                   std::printf("%d", int(c * 10.0F + 0.5F));
               }
               std::puts("");
            }

            if (result.received)
            {
                return {*result.received, result.correlation};
            }
        }
    }

private:
    MultiCorrelator multi_correlator_;
};

/// Reads symbols from the channel.
/// Each frame is delimited by the frame delimiter on each side. The delimiter is 9 consecutive zero bits.
/// Each byte within the frame is preceded by a single high start bit (which differentiates it from the delimiter).
class SymbolReader
{
public:
    struct Delimiter {};
    using Symbol = std::variant<Delimiter, std::uint8_t>;

    SymbolReader(const float rssi_threshold = 0.2F) : rssi_threshold_(rssi_threshold) { }

    std::tuple<Symbol, float> next()
    {
        while (true)
        {
            const auto [bit, rssi] = bit_reader_.next();
            if (rssi > rssi_threshold_)
            {
                if (remaining_bits_ >= 0)
                {
                    buffer_ = (buffer_ << 1U) | bit;
                    remaining_bits_--;
                    if (remaining_bits_ < 0)
                    {
                        return {Symbol{buffer_}, rssi};
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
                        return {Symbol{Delimiter{}}, rssi};
                    }
                }
            }
            else
            {
                //std::printf("NO CARRIER (%.3f)\n", rssi);
                remaining_bits_ = -1;
                consecutive_zeros_ = 0;
            }
        }
    }

private:
    BitReader bit_reader_;

    std::uint64_t consecutive_zeros_ = 0;

    std::uint8_t buffer_ = 0;
    std::int8_t remaining_bits_ = -1;
    const float rssi_threshold_;
};

int main()
{
    side_channel::initThread();
    SymbolReader reader;
    while (true)
    {
        const auto [symbol, rssi] = reader.next();
        if (std::holds_alternative<SymbolReader::Delimiter>(symbol))
        {
            std::printf("DELIMITER (%0.3f)\n", rssi);
        }
        else
        {
            const auto byte = std::get<std::uint8_t>(symbol);
            std::printf("0x%02x (%0.3f)\n", byte, rssi);
        }
    }
    return 0;
}
