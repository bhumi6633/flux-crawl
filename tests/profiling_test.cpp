#include <fluxcrawl/async_crawler.hpp>
#include <fluxcrawl/sequential_crawler.hpp>
#include <fluxcrawl/thread_pool_crawler.hpp>
#include <iostream>
#include <map>

int main(int argc, char* argv[])
{
    if (argc != 3) { return 2; }
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) { ++failures; std::cerr << label << '\n'; }
    };
    const auto signature = [](const fluxcrawl::CrawlResult& result) {
        std::map<std::string, std::pair<std::size_t, long>> values;
        for (const auto& record : result.records) {
            const auto* response = std::get_if<fluxcrawl::CrawlResponseMetadata>(&record.outcome);
            values.emplace(record.item.url.canonical(), std::pair{record.item.depth, response ? response->status_code : 0L});
        }
        return values;
    };
    for (const auto limit : {1U, 4U, 8U}) {
        for (int engine = 0; engine != 3; ++engine) {
            fluxcrawl::HttpClientOptions http;
            http.use_environment_proxy = false;
            // Parity is not a connection-timeout test; tolerate a loaded CI host.
            http.connect_timeout = std::chrono::seconds{1};
            for (const auto path : {"/crawl/", "/crawl/failures"}) {
                const auto seed = fluxcrawl::Url::parse(std::string{argv[1]} + path).value();
                const auto crawl = [&](fluxcrawl::CrawlOptions options) {
                    if (engine == 0) { return fluxcrawl::SequentialCrawler{http}.crawl(seed, options); }
                    if (engine == 1) { return fluxcrawl::ThreadPoolCrawler{limit, http}.crawl(seed, options); }
                    return fluxcrawl::AsyncCrawler{limit, http}.crawl(seed, options);
                };
                for (const auto budget : {0U, 100U}) {
                    fluxcrawl::CrawlOptions options;
                    options.max_pages = budget;
                    const auto ordinary = crawl(options);
                    check(!ordinary.profile, "disabled profile absent");
                    options.profile = true;
                    const auto measured = crawl(options);
                    check(signature(ordinary) == signature(measured), "profile preserves URL/depth/status and failure set");
                    check(ordinary.pages_attempted == measured.pages_attempted
                          && ordinary.bytes_downloaded == measured.bytes_downloaded
                          && ordinary.urls_pending == measured.urls_pending, "profile preserves accounting");
                    check(measured.profile.has_value(), "enabled summary exists");
                    if (!measured.profile) { continue; }
                    const auto& profile = *measured.profile;
                    const auto& common = profile.common;
                    check(common.extractions == measured.html_pages_processed, "extraction count");
                    check(common.links_accepted + 1 == measured.urls_discovered, "accepted links exclude seed");
                    check(common.links_considered >= common.links_accepted, "discovery counts");
                    check(common.extraction.count() >= 0 && common.discovery.count() >= 0
                          && common.frontier.count() >= 0, "common nonnegative durations");
                    check(profile.threaded.has_value() == (engine == 1)
                          && profile.async.has_value() == (engine == 2), "only applicable engine section");
                    if (profile.threaded) {
                        const auto& threaded = *profile.threaded;
                        fluxcrawl::WorkerProfile sum;
                        for (const auto& worker : threaded.workers) {
                            check(worker.lock_wait.count() >= 0 && worker.idle.count() >= 0
                                  && worker.activity.count() >= 0, "worker durations valid");
                            check(worker.waits == worker.wakeups, "every normal wait returns");
                            sum.merge(worker);
                        }
                        const auto& total = threaded.total;
                        check(total.requests == measured.pages_attempted && sum.requests == total.requests, "worker requests merge");
                        check(sum.lock_wait == total.lock_wait && sum.idle == total.idle && sum.activity == total.activity
                              && sum.lock_acquisitions == total.lock_acquisitions && sum.waits == total.waits
                              && sum.wakeups == total.wakeups, "all worker coordination fields merge");
                        check(sum.common.extraction == common.extraction && sum.common.discovery == common.discovery
                              && sum.common.frontier == common.frontier && sum.common.extractions == common.extractions
                              && sum.common.links_considered == common.links_considered
                              && sum.common.links_accepted == common.links_accepted, "all common fields merge");
                        check(total.lock_acquisitions == 2 * measured.pages_attempted + threaded.workers.size(), "acquire/publish plus final acquire count");
                        check(threaded.max_active <= limit && (budget == 0 || threaded.max_active > 0), "active worker bounds");
                        check(threaded.workers.size() == (budget ? limit : 0), "only launched workers have records");
                    }
                    if (profile.async) {
                        const auto& async = *profile.async;
                        check(async.completions == measured.pages_attempted && async.refills == async.completions,
                              "fixture transfers complete including ordinary HTTP errors");
                        check(async.max_active <= limit && (budget == 0 || async.max_active > 0), "active transfer bounds");
                        check(budget == 0 ? async.iterations == 0 : async.iterations > 0, "loop counter");
                        check(async.multi_wait.count() >= 0 && async.completions_time.count() >= 0
                              && async.refill_time.count() >= 0 && async.waits <= async.iterations, "async timings and waits");
                    }
                }
                fluxcrawl::CrawlOptions shallow{0, 100, false, true};
                const auto root = crawl(shallow);
                check(root.pages_attempted == 1 && root.profile->common.extractions == 0, "depth boundary still skips parser");
            }
        }
    }
    std::cout << failures << " profiling failures\n";
    return failures ? 1 : 0;
}
