#include <fluxcrawl/bloom_filter.hpp>
#include <fluxcrawl/dedup.hpp>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <algorithm>
namespace fluxcrawl {
std::string_view dedup_name(DedupMode mode) {
    switch(mode){case DedupMode::Memory:return "memory";case DedupMode::Sqlite:return "sqlite";case DedupMode::BloomSqlite:return "bloom-sqlite";}
    throw std::invalid_argument("Unsupported dedup mode");
}
void DedupOptions::validate() const {
    (void)dedup_name(mode);
    if(mode!=DedupMode::Memory && memory_reserve) { throw std::invalid_argument("Seen reserve applies to memory dedup only"); }
    if(!expected_elements || !std::isfinite(false_positive_rate) || false_positive_rate<=0 || false_positive_rate>=1) { throw std::invalid_argument("Bloom requires positive expected elements and finite 0 < false-positive rate < 1"); }
}
BloomFilter::BloomFilter(std::size_t expected, double probability) {
    DedupOptions{DedupMode::BloomSqlite,expected,probability,0}.validate();
    const auto ln2=std::log(2.0L);
    const long double required=std::ceil(-static_cast<long double>(expected)*std::log(static_cast<long double>(probability))/(ln2*ln2));
    if(!std::isfinite(required) || required>static_cast<long double>(std::numeric_limits<std::size_t>::max()-63)) { throw std::length_error("Bloom bit count overflows"); }
    bits_=std::max(std::size_t{64},static_cast<std::size_t>(required));
    const auto words=(bits_+63)/64;
    if(words>words_.max_size()) { throw std::length_error("Bloom allocation exceeds vector capacity"); }
    probes_=std::max(std::size_t{1},static_cast<std::size_t>(std::round(static_cast<long double>(bits_)/expected*ln2)));
    words_.assign(words,0);
}
std::uint64_t BloomFilter::hash(std::string_view bytes,std::uint64_t seed) {
    // Defined FNV-1a byte iteration followed by MurmurHash3's fmix64 avalanche.
    auto value=seed;
    for(char byte:bytes){value^=static_cast<unsigned char>(byte);value*=1099511628211ULL;}
    value^=value>>33;value*=0xff51afd7ed558ccdULL;value^=value>>33;value*=0xc4ceb9fe1a85ec53ULL;return value^(value>>33);
}
namespace {
std::pair<std::uint64_t,std::uint64_t> hashes(std::string_view bytes) {
    return {BloomFilter::hash(bytes,14695981039346656037ULL),BloomFilter::hash(bytes,7809847782465536322ULL)|1ULL};
}
}
void BloomFilter::insert(std::string_view bytes) {
    auto [first,step]=hashes(bytes);
    for(std::size_t i=0;i<probes_;++i){const auto position=first%bits_;words_[position/64]|=std::uint64_t{1}<<(position%64);first+=step;}
    ++insertions_;
}
bool BloomFilter::maybe_contains(std::string_view bytes) const {
    auto [first,step]=hashes(bytes);
    for(std::size_t i=0;i<probes_;++i){const auto position=first%bits_;if(!(words_[position/64]&(std::uint64_t{1}<<(position%64)))){return false;}first+=step;}return true;
}
std::size_t BloomFilter::set_bits() const {std::size_t n=0;for(auto word:words_){n+=static_cast<std::size_t>(std::popcount(word));}return n;}
}
