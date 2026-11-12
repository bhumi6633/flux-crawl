#pragma once
#include <fluxcrawl/observability.hpp>
#include <filesystem>
namespace fluxcrawl {
// Read-only loopback server. Owns one I/O thread, capped connections and writes.
class ObservabilityServer {
public:
    ObservabilityServer(std::shared_ptr<Observability> source, std::uint16_t port,
                        std::filesystem::path assets = {});
    ~ObservabilityServer();
    ObservabilityServer(const ObservabilityServer&) = delete;
    ObservabilityServer& operator=(const ObservabilityServer&) = delete;
    std::uint16_t port() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Versioned wire representation, isolated from the engine library.
std::string observability_json(const ObservabilitySnapshot& snapshot);
}
