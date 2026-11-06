#include <fluxcrawl/checkpoint.hpp>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

namespace fluxcrawl {
CrawlCheckpoint CheckpointManager::read(const std::filesystem::path& path)
{
    std::ifstream input{path, std::ios::binary};
    if (!input) { throw std::runtime_error("Cannot open checkpoint: " + path.string()); }
    std::string text;
    char buffer[8192];
    while (input.read(buffer, sizeof(buffer)) || input.gcount() != 0) {
        if (static_cast<std::size_t>(input.gcount()) > max_file_bytes - text.size()) { throw std::runtime_error("Checkpoint exceeds 64 MiB limit"); }
        text.append(buffer, static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof()) { throw std::runtime_error("Failed reading checkpoint: " + path.string()); }
    return deserialize(text);
}
std::size_t CheckpointManager::write(const std::filesystem::path& path, const CrawlCheckpoint& checkpoint,
                                     const std::function<void()>& before_replace)
{
    if (checkpoint.spool_sync) { checkpoint.spool_sync->sync(); }
    const auto text = serialize(checkpoint); // Validate before creating anything.
    static std::atomic<std::uint64_t> sequence{0};
    static const auto nonce = std::random_device{}();
    auto temporary = path;
    temporary += ".tmp." + std::to_string(nonce) + "." + std::to_string(sequence.fetch_add(1));
    struct Cleanup {
        std::filesystem::path path;
        std::FILE* file = nullptr;
        bool owned = false;
        ~Cleanup() {
            if (file) { std::fclose(file); }
            if (owned) { std::error_code ignored; std::filesystem::remove(path, ignored); }
        }
    } cleanup{temporary};
    cleanup.file = std::fopen(temporary.string().c_str(), "wx"); // Exclusive creation, same directory.
    if (!cleanup.file) { throw std::runtime_error("Cannot create checkpoint temporary file: " + std::string{std::strerror(errno)}); }
    cleanup.owned = true;
    if (std::fwrite(text.data(), 1, text.size(), cleanup.file) != text.size() || std::fflush(cleanup.file) != 0) {
        throw std::runtime_error("Failed writing/flushing checkpoint: " + path.string());
    }
#if defined(__unix__) || defined(__APPLE__)
    if (::fsync(::fileno(cleanup.file)) != 0) { throw std::runtime_error("Failed syncing checkpoint: " + path.string()); }
#endif
    const auto close_result = std::fclose(cleanup.file); cleanup.file = nullptr;
    if (close_result != 0) { throw std::runtime_error("Failed closing checkpoint: " + path.string()); }
    (void)read(temporary); // Check the bytes actually written before replacing last good state.
    if (before_replace) { before_replace(); }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) { throw std::runtime_error("Cannot replace checkpoint: " + error.message()); }
    cleanup.owned = false;
    return text.size();
}
} // namespace fluxcrawl
