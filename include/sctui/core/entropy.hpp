#pragma once

#include <random>
#include <array>
#include <chrono>
#include <fstream>
#include <cstdint>

namespace sctui::core {

class AdvancedRng {
public:
    [[nodiscard]] static std::mt19937_64 create_engine() {
        // Collect 512 bits (16 x 32-bit words) of true system entropy
        std::array<uint32_t, 16> entropy_pool{};

        // 1. Read directly from /dev/urandom on Linux
        std::ifstream urandom("/dev/urandom", std::ios::binary);
        if (urandom.is_open()) {
            urandom.read(reinterpret_cast<char*>(entropy_pool.data()), entropy_pool.size() * sizeof(uint32_t));
        }

        // 2. Mix in hardware entropy via std::random_device (e.g. CPU RDRAND instruction)
        std::random_device rd;
        for (size_t i = 0; i < entropy_pool.size(); ++i) {
            entropy_pool[i] ^= rd();
        }

        // 3. Mix in high-resolution steady clock nanosecond timestamp and ASLR memory entropy
        auto time_ns = static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
        entropy_pool[0] ^= static_cast<uint32_t>(time_ns & 0xFFFFFFFF);
        entropy_pool[1] ^= static_cast<uint32_t>((time_ns >> 32) & 0xFFFFFFFF);
        entropy_pool[2] ^= static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&rd) & 0xFFFFFFFF);

        // Distribute entropy across all 624 words of mt19937 state using std::seed_seq
        std::seed_seq seq(entropy_pool.begin(), entropy_pool.end());
        return std::mt19937_64(seq);
    }
};

} // namespace sctui::core
