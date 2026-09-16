#include <fluxcrawl/sequential_crawler.hpp>

#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char* argv[])
{
    if (argc != 3) { return 2; }
    int checks = 0;
    int failures = 0;
    const auto check = [&](bool condition, std::string_view label) {
        ++checks;
        if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
    };
    const std::string origin{argv[1]};
    const auto url = [&](std::string_view path) {
        const auto parsed = fluxcrawl::Url::parse(origin + std::string{path});
        if (!parsed) { throw std::runtime_error("Invalid fixture URL"); }
        return *parsed;
    };
    fluxcrawl::HttpClientOptions http;
    http.connect_timeout = std::chrono::milliseconds{100};
    const fluxcrawl::SequentialCrawler crawler{http};
    const auto seed = url("/crawl/");
    const auto order = [&](const fluxcrawl::CrawlResult& result,
                           const std::vector<std::string_view>& paths) {
        check(result.pages_attempted == paths.size() && result.records.size() == paths.size(), "attempt count and records");
        check(result.responses_received + result.fetch_failures == result.pages_attempted, "accounting invariant");
        check(result.urls_discovered == result.pages_attempted + result.urls_pending, "seen equals attempted plus pending");
        for (std::size_t i = 0; i < paths.size() && i < result.records.size(); ++i) {
            check(result.records[i].item.url.path() == paths[i], paths[i]);
        }
    };
    const auto full = crawler.crawl(seed);
    order(full, {"/crawl/", "/crawl/a", "/crawl/b", "/crawl/shared", "/crawl/redirect-start", "/crawl/deep", "/crawl/redirected/child.html"});
    check(full.responses_received == 7 && full.fetch_failures == 0 && full.urls_pending == 0, "full crawl terminates");
    check(full.urls_discovered == 7 && full.html_pages_processed == 5, "dedup and processed HTML count");
    if (full.records.size() == 7) {
        for (std::size_t i = 0; i < full.records.size(); ++i) {
            check(full.records[i].item.depth == (i == 0 ? 0U : (i < 5 ? 1U : 2U)), "propagated depth");
        }
        const auto* redirected = std::get_if<fluxcrawl::CrawlResponseMetadata>(&full.records[4].outcome);
        check(redirected && redirected->effective_url == url("/crawl/redirected/index.html")
              && redirected->status_code == 200, "redirect metadata and effective base");
    }
    auto options = fluxcrawl::CrawlOptions{};
    options.max_depth = 1;
    const auto shallow = crawler.crawl(seed, options);
    order(shallow, {"/crawl/", "/crawl/a", "/crawl/b", "/crawl/shared", "/crawl/redirect-start"});
    check(shallow.html_pages_processed == 1 && shallow.urls_discovered == 5, "depth boundary skips extraction");
    options.max_depth = 0;
    const auto root = crawler.crawl(seed, options);
    order(root, {"/crawl/"});
    check(root.html_pages_processed == 0 && root.urls_discovered == 1, "zero depth");
    options = {};
    options.max_pages = 3;
    const auto limited = crawler.crawl(seed, options);
    order(limited, {"/crawl/", "/crawl/a", "/crawl/b"});
    check(limited.urls_discovered == 6 && limited.urls_pending == 3, "page limit retains pending count");
    options.max_pages = 0;
    const auto zero = crawler.crawl(seed, options);
    order(zero, {});
    check(zero.urls_discovered == 1 && zero.urls_pending == 1 && zero.responses_received == 0, "zero attempts still seed seen");

    const auto mixed = crawler.crawl(url("/crawl/failures"));
    order(mixed, {"/crawl/failures", "/", "/crawl/not-found", "/crawl/error", "/crawl/plain", "/crawl/binary", "/crawl/no-type", "/crawl/malformed", "/crawl/after"});
    check(mixed.fetch_failures == 1 && mixed.responses_received == 8 && mixed.html_pages_processed == 2, "failure continuation and MIME/status policy");
    if (mixed.records.size() == 9) {
        check(std::holds_alternative<fluxcrawl::HttpError>(mixed.records[1].outcome), "error metadata preserved");
        const auto* missing = std::get_if<fluxcrawl::CrawlResponseMetadata>(&mixed.records[2].outcome);
        const auto* unavailable = std::get_if<fluxcrawl::CrawlResponseMetadata>(&mixed.records[3].outcome);
        check(missing && missing->status_code == 404 && unavailable && unavailable->status_code == 503, "HTTP failures remain responses");
    }
    options = {};
    options.max_pages = 2;
    const auto failed_limit = crawler.crawl(url("/crawl/failures"), options);
    order(failed_limit, {"/crawl/failures", "/"});
    check(failed_limit.fetch_failures == 1 && failed_limit.responses_received == 1, "failure consumes attempt budget");
    options = {};
    options.same_host_only = true;
    const auto same_host = crawler.crawl(url("/crawl/host-policy"), options);
    order(same_host, {"/crawl/host-policy", "/", "/crawl/shared"});
    check(same_host.fetch_failures == 1, "same host allows different port but rejects localhost alias");
    options.same_host_only = false;
    const auto all_hosts = crawler.crawl(url("/crawl/host-policy"), options);
    order(all_hosts, {"/crawl/host-policy", "/crawl/shared", "/", "/crawl/shared"});
    check(all_hosts.responses_received == 3, "default allows different hosts");
    for (const auto type : {"text/html", "text/html; charset=UTF-8", " TEXT/HTML\t; charset=x"}) {
        check(fluxcrawl::is_html_content_type(type), "HTML MIME match");
    }
    for (const auto type : {"", "text/htmlx", "application/xhtml+xml", "text/plain", "image/png", "application/pdf", "application/json"}) {
        check(!fluxcrawl::is_html_content_type(type), "non-HTML MIME rejection");
    }
    std::cout << checks << " sequential crawler checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
