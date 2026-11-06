#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>
namespace fluxcrawl {
class BloomFilter {
public:
    BloomFilter(std::size_t expected_elements, double probability);
    void insert(std::string_view bytes);
    bool maybe_contains(std::string_view bytes) const;
    std::size_t bit_count() const { return bits_; }
    std::size_t byte_count() const { return words_.size()*sizeof(std::uint64_t); }
    std::size_t probes() const { return probes_; }
    std::size_t insertions() const { return insertions_; }
    std::size_t set_bits() const;
    static std::uint64_t hash(std::string_view bytes, std::uint64_t seed);
private:
    std::vector<std::uint64_t> words_;
    std::size_t bits_,probes_,insertions_=0;
};
}
