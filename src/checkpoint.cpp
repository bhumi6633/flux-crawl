#include <fluxcrawl/checkpoint.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace fluxcrawl {
namespace {
using Counter = std::pair<std::string_view, std::size_t CrawlResult::*>;
constexpr Counter fields[] = {
#define FIELD(name) {#name, &CrawlResult::name}
    FIELD(robots_fetches), FIELD(robots_origins), FIELD(robots_cache_hits), FIELD(robots_allowed),
    FIELD(urls_blocked_by_robots), FIELD(robots_fetch_failures), FIELD(robots_retries_scheduled),
    FIELD(robots_retries_attempted), FIELD(robots_resources_reused), FIELD(robots_retries_cancelled),
    FIELD(pages_attempted), FIELD(responses_received), FIELD(fetch_failures), FIELD(retries_scheduled),
    FIELD(retries_attempted), FIELD(retries_exhausted), FIELD(responses_recovered_after_retry),
    FIELD(retry_after_used), FIELD(retries_cancelled), FIELD(resources_completed),
    FIELD(terminal_http_failures), FIELD(terminal_fetch_failures), FIELD(html_pages_processed),
    FIELD(urls_discovered), FIELD(urls_pending), FIELD(bytes_downloaded), FIELD(recovery_attempts),
    FIELD(recovery_retry_attempts), FIELD(attempts_unresolved), FIELD(robots_attempts_unresolved)
#undef FIELD
};
[[noreturn]] void bad(std::string_view message) { throw std::runtime_error("Invalid checkpoint: " + std::string{message}); }
void require(bool value, std::string_view message) { if (!value) { bad(message); } }
struct Reader {
    std::istringstream in;
    explicit Reader(std::string_view text) : in{std::string{text}} { in.imbue(std::locale::classic()); }
    void key(std::string_view expected) { std::string value; require(bool(in >> value) && value == expected, "expected field " + std::string{expected}); }
    std::uint64_t number() {
        std::string token; require(bool(in >> token), "missing number");
        std::uint64_t value = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        require(error == std::errc{} && end == token.data() + token.size(), "invalid nonnegative integer");
        return value;
    }
    bool boolean() { const auto n = number(); require(n <= 1, "invalid boolean"); return n != 0; }
    std::string string() { in >> std::ws; require(in.peek() == '"', "expected quoted string"); std::string value; require(bool(in >> std::quoted(value)), "invalid quoted string"); return value; }
    double real() { double value = 0; require(bool(in >> value) && std::isfinite(value), "invalid finite number"); return value; }
    Url url() { const auto value = string(); auto parsed = Url::parse(value); require(parsed && parsed->str() == value, "URL must be valid and canonical"); return *parsed; }
    std::size_t size() { const auto value = number(); require(value <= std::numeric_limits<std::size_t>::max(), "size overflow"); return static_cast<std::size_t>(value); }
    std::int64_t integer() { const auto value = number(); require(value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()), "integer overflow"); return static_cast<std::int64_t>(value); }
};
void options(std::ostream& out, const CrawlOptions& o)
{
    out << "crawl " << o.max_depth << ' ' << o.max_pages << ' ' << o.same_host_only << ' ' << o.respect_robots
        << "\nretry " << o.retry.max_retries << ' ' << o.retry.initial_backoff.count() << ' ' << o.retry.max_backoff.count()
        << ' ' << o.retry.multiplier << ' ' << o.retry.jitter
        << "\nhost " << bool(o.scheduling.max_per_host) << ' ' << o.scheduling.max_per_host.value_or(0)
        << ' ' << bool(o.scheduling.rate) << ' ' << (o.scheduling.rate ? o.scheduling.rate->requests_per_second : 0)
        << ' ' << (o.scheduling.rate ? o.scheduling.rate->burst : 0) << '\n';
}
}
CrawlCheckpoint CheckpointManager::metadata_only(const CrawlCheckpoint& source)
{
    CrawlCheckpoint c{.seed = source.seed};
    c.format_version = source.format_version; c.application_version = source.application_version;
    c.options.max_depth = source.options.max_depth; c.options.max_pages = source.options.max_pages;
    c.options.same_host_only = source.options.same_host_only; c.options.respect_robots = source.options.respect_robots;
    c.deferred = source.deferred; c.spool_sync = source.spool_sync;
    c.options.backpressure = source.options.backpressure;
    c.options.retry = source.options.retry; c.options.scheduling = source.options.scheduling;
    c.http = source.http; c.sequence = source.sequence; c.completed = source.completed;
    c.counters = copy_counters(source.counters); return c;
}
CrawlResult CheckpointManager::copy_counters(const CrawlResult& result)
{
    CrawlResult copy;
    for (const auto& [name, member] : fields) { (void)name; copy.*member = result.*member; }
    return copy;
}
bool CheckpointManager::same_semantics(const CrawlOptions& a, const CrawlOptions& b)
{
    std::ostringstream x, y; x.imbue(std::locale::classic()); y.imbue(std::locale::classic());
    x << std::setprecision(17); y << std::setprecision(17); options(x, a); options(y, b); return x.str() == y.str() && a.backpressure.enabled == b.backpressure.enabled
        && (!a.backpressure.enabled || (a.backpressure.high_watermark == b.backpressure.high_watermark && a.backpressure.low_watermark == b.backpressure.low_watermark));
}
void CheckpointManager::validate(const CrawlCheckpoint& c, std::size_t maximum_seen)
{
    require((c.format_version == 1 || c.format_version == CrawlCheckpoint::supported_version), "unsupported format version");
    require(c.sequence != std::numeric_limits<std::uint64_t>::max(), "sequence overflow");
    c.options.retry.validate(); c.options.backpressure.validate();
    DiscoverySpool::validate(c.deferred, c.options.max_depth);
    require(c.options.backpressure.enabled || c.deferred.count() == 0, "deferred state requires admission control"); (void)HostScheduler{1, c.options.scheduling};
    require(c.http.connect_timeout.count() > 0 && c.http.request_timeout.count() > 0
        && c.http.connect_timeout.count() <= std::numeric_limits<long>::max()
        && c.http.request_timeout.count() <= std::numeric_limits<long>::max()
        && c.http.max_redirects >= 0 && !c.http.user_agent.empty()
        && c.http.user_agent.find_first_of("\r\n") == std::string::npos
        && c.http.user_agent.find('\0') == std::string::npos, "invalid HTTP configuration");
    const auto& r = c.counters;
    require(r.pages_attempted <= c.options.max_pages, "attempt budget exceeded");
    require(c.seen.size() <= maximum_seen && c.unfinished.size() <= c.seen.size(), "state size invalid");
    std::unordered_set<Url> seen;
    for (const auto& url : c.seen) {
        require(seen.insert(url).second, "duplicate seen URL");
        require(!c.options.same_host_only || url.host() == c.seed.host(), "seen URL violates same-host policy");
    }
    require(seen.contains(c.seed), "seed missing from seen");
    std::unordered_set<Url> pending;
    for (const auto& work : c.unfinished) {
        require(!work.item.policy_request && work.item.depth <= c.options.max_depth
            && work.item.retry_count <= c.options.retry.max_retries, "invalid unfinished metadata");
        require(work.item.url != c.seed || work.item.depth == 0, "seed depth must be zero");
        require(work.item.retry_count <= r.pages_attempted, "retry has no prior attempts");
        require(seen.contains(work.item.url) && pending.insert(work.item.url).second, "unknown or duplicate unfinished URL");
        require(!work.eligible_unix_ms || (*work.eligible_unix_ms >= 0 && work.item.retry_count != 0 && !work.was_active), "invalid retry deadline");
    }
    require(r.urls_discovered == seen.size() && r.urls_pending == c.unfinished.size(), "discovery counters disagree");
    // Subtraction checks avoid overflow even for adversarial aggregate values.
    auto remainder = seen.size();
    for (auto value : {r.resources_completed, r.urls_blocked_by_robots, r.robots_resources_reused, r.urls_pending}) {
        require(value <= remainder, "resource accounting disagrees"); remainder -= value;
    }
    require(remainder == 0, "resource accounting incomplete");
    remainder = r.pages_attempted;
    for (auto value : {r.responses_received, r.fetch_failures, r.attempts_unresolved}) { require(value <= remainder, "attempt accounting disagrees"); remainder -= value; }
    require(remainder == 0, "attempt accounting incomplete");
    require(r.retries_attempted <= r.pages_attempted && r.recovery_attempts <= r.pages_attempted
        && r.recovery_retry_attempts <= r.recovery_attempts, "invalid retry/recovery counters");
    require(r.retries_scheduled <= r.pages_attempted && r.retries_attempted >= r.recovery_retry_attempts,
        "invalid retry scheduling counters");
    const auto ordinary_retries = r.retries_attempted - r.recovery_retry_attempts;
    require(ordinary_retries <= r.retries_scheduled && r.retries_cancelled == r.retries_scheduled - ordinary_retries,
        "retry cancellation counters disagree");
    require(r.robots_retries_attempted <= r.robots_retries_scheduled
        && r.robots_retries_cancelled == r.robots_retries_scheduled - r.robots_retries_attempted
        && r.robots_attempts_unresolved <= r.robots_fetches, "robots counters disagree");
    require(r.responses_recovered_after_retry <= r.resources_completed && r.retries_exhausted <= r.resources_completed,
        "terminal counters disagree");
    require(c.completed == (c.unfinished.empty() && c.deferred.count() == 0), "completed marker disagrees");
}
std::string CheckpointManager::serialize_metadata(const CrawlCheckpoint& c)
{
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17);
    out << "FLUXCRAWL_CHECKPOINT " << c.format_version << "\napplication " << std::quoted(c.application_version)
        << "\nseed " << std::quoted(c.seed.str()) << "\nsequence " << c.sequence << "\ncompleted " << c.completed << '\n';
    options(out, c.options);
    out << "http " << c.http.connect_timeout.count() << ' ' << c.http.request_timeout.count() << ' '
        << c.http.max_redirects << ' ' << c.http.max_body_bytes << ' ' << c.http.use_environment_proxy << ' ' << std::quoted(c.http.user_agent) << '\n';
    for (const auto& [name, member] : fields) { out << name << ' ' << c.counters.*member << '\n'; }
    if (c.format_version >= 2) {
        out << "pressure " << c.options.backpressure.enabled << ' ' << c.options.backpressure.high_watermark << ' ' << c.options.backpressure.low_watermark << '\n';
        out << "spool " << std::quoted(c.deferred.directory) << ' ' << c.deferred.extents.size() << '\n';
        for (const auto& e : c.deferred.extents) { out << "extent " << e.depth << ' ' << e.begin << ' ' << e.end << ' ' << e.count << ' ' << e.file_id << ' ' << std::quoted(e.origin) << '\n'; }
    }
    return out.str();
}
std::string CheckpointManager::serialize(const CrawlCheckpoint& c)
{
    validate(c);
    std::ostringstream out; out << serialize_metadata(c);
    auto sorted = c.seen;
    std::sort(sorted.begin(), sorted.end(), [](const Url& a, const Url& b) { return a.str() < b.str(); });
    out << "seen " << sorted.size() << '\n';
    for (const auto& url : sorted) { out << std::quoted(url.str()) << '\n'; }
    out << "unfinished " << c.unfinished.size() << '\n';
    for (const auto& work : c.unfinished) {
        out << "work " << std::quoted(work.item.url.str()) << ' ' << work.item.depth << ' ' << work.item.retry_count
            << ' ' << work.item.recovery << ' ' << work.was_active << ' ' << bool(work.eligible_unix_ms)
            << ' ' << work.eligible_unix_ms.value_or(0) << '\n';
    }
    out << "END\n";
    auto text = out.str(); require(text.size() <= max_file_bytes, "snapshot exceeds 64 MiB limit"); return text;
}
namespace {
CrawlCheckpoint read_metadata(Reader& r)
{
    r.key("FLUXCRAWL_CHECKPOINT"); const auto version_number = r.number();
    require((version_number == 1 || version_number == CrawlCheckpoint::supported_version), "unsupported format version");
    r.key("application"); auto app = r.string(); r.key("seed"); auto seed = r.url();
    CrawlCheckpoint c{.seed = seed}; c.application_version = app; c.format_version = static_cast<std::uint32_t>(version_number);
    r.key("sequence"); c.sequence = r.number(); r.key("completed"); c.completed = r.boolean();
    r.key("crawl"); c.options.max_depth = r.size(); c.options.max_pages = r.size(); c.options.same_host_only = r.boolean(); c.options.respect_robots = r.boolean();
    r.key("retry"); c.options.retry.max_retries = r.size(); c.options.retry.initial_backoff = std::chrono::milliseconds{r.integer()};
    c.options.retry.max_backoff = std::chrono::milliseconds{r.integer()}; c.options.retry.multiplier = r.real(); c.options.retry.jitter = r.boolean();
    r.key("host"); const bool host_limit = r.boolean(); const auto cap = r.size(); const bool rate = r.boolean(); const auto frequency = r.real(); const auto burst = r.size();
    require(host_limit || cap == 0, "disabled host limit must have zero value");
    require(rate || (frequency == 0 && burst == 0), "disabled rate must have zero values");
    if (host_limit) { c.options.scheduling.max_per_host = cap; } if (rate) { c.options.scheduling.rate = RateLimit{frequency, burst}; }
    r.key("http"); c.http.connect_timeout = std::chrono::milliseconds{r.integer()}; c.http.request_timeout = std::chrono::milliseconds{r.integer()};
    const auto redirects = r.number(); require(redirects <= static_cast<std::uint64_t>(std::numeric_limits<long>::max()), "redirect limit overflow");
    c.http.max_redirects = static_cast<long>(redirects); c.http.max_body_bytes = r.size(); c.http.use_environment_proxy = r.boolean(); c.http.user_agent = r.string();
    for (const auto& [name, member] : fields) { r.key(name); c.counters.*member = r.size(); }
    if (c.format_version >= 2) {
        r.key("pressure"); c.options.backpressure.enabled = r.boolean(); c.options.backpressure.high_watermark = r.size(); c.options.backpressure.low_watermark = r.size();
        r.key("spool"); c.deferred.directory = r.string(); const auto count = r.size(); require(count <= 10000, "too many deferred depth extents");
        for (std::size_t n=0; n<count; ++n) { r.key("extent"); const auto depth=r.size(); const auto begin=r.number(); const auto end=r.number(); const auto entries=r.number(); const auto id=r.number(); const auto origin=r.string(); c.deferred.extents.push_back({depth,begin,end,entries,id,origin}); }
    } else { c.options.backpressure.enabled = false; }
    return c;
}
}
CrawlCheckpoint CheckpointManager::deserialize_metadata(std::string_view text)
{
    require(text.size() <= 1024 * 1024, "metadata too large");
    Reader r{text}; auto c = read_metadata(r); r.in >> std::ws; require(r.in.eof(), "trailing metadata"); return c;
}
CrawlCheckpoint CheckpointManager::deserialize(std::string_view text)
{
    require(text.size() <= max_file_bytes, "file exceeds 64 MiB limit"); Reader r{text};
    auto c = read_metadata(r);
    r.key("seen"); auto count = r.size(); require(count <= 1000000, "too many seen URLs");
    for (std::size_t i = 0; i < count; ++i) { c.seen.push_back(r.url()); }
    r.key("unfinished"); count = r.size(); require(count <= c.seen.size(), "too many unfinished items");
    for (std::size_t i = 0; i < count; ++i) {
        r.key("work"); auto url = r.url(); const auto depth = r.size(); const auto retry = r.size(); const bool recovery = r.boolean();
        const bool active = r.boolean(); const bool delayed = r.boolean(); const auto deadline = r.integer();
        require(delayed || deadline == 0, "ready item has deadline");
        c.unfinished.push_back({{url, depth, retry, false, recovery}, delayed ? std::optional{deadline} : std::nullopt, active});
    }
    r.key("END"); r.in >> std::ws; require(r.in.eof(), "trailing data"); validate(c); return c;
}
std::int64_t CheckpointManager::wall_deadline(HostScheduler::Time deadline, HostScheduler::Time now, std::int64_t wall_ms)
{
    const auto remaining = deadline > now ? std::chrono::duration<long double, std::milli>(deadline - now).count() : 0;
    const auto value = static_cast<long double>(wall_ms) + std::ceil(remaining);
    return value >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ? std::numeric_limits<std::int64_t>::max() : static_cast<std::int64_t>(value);
}
HostScheduler::Time CheckpointManager::steady_deadline(std::int64_t deadline, HostScheduler::Time now, std::int64_t wall_ms)
{
    const long double delay = std::max(0.0L, static_cast<long double>(deadline) - static_cast<long double>(wall_ms));
    const auto room = std::chrono::duration<long double, std::milli>(HostScheduler::Time::max() - now).count();
    return delay >= room ? HostScheduler::Time::max() : now + std::chrono::duration_cast<HostScheduler::Clock::duration>(std::chrono::duration<long double, std::milli>{delay});
}
} // namespace fluxcrawl
