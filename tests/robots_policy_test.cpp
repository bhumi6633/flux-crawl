#include <fluxcrawl/robots_policy.hpp>
#include <fluxcrawl/crawl_policy.hpp>
#include <fluxcrawl/crawler_identity.hpp>
#include <iostream>
#include <stdexcept>
using namespace fluxcrawl;
void check(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
Url url(std::string_view path) { return Url::parse(std::string{"http://example.test"} + std::string{path}).value(); }
bool allowed(std::string_view text, std::string_view path, std::string_view agent = "FluxCrawl")
{ return RobotsPolicy::parse(text).allowed(agent, url(path)); }
int main()
{
    try {
        check(CrawlerIdentity{}.user_agent == "FluxCrawl/" + std::string{version}, "versioned identity");
        check(allowed("", "/x"), "empty");
        check(allowed("# comment\nmalformed\nDisallow: /\n", "/x"), "rules outside groups ignored");
        const auto rules = " # hello\r\n uSeR-aGeNt : *\r\n Disallow: /private/ # internal\r\nAllow: /private/public/\nSitemap: /sitemap.xml\nCrawl-delay: 100\n";
        check(!allowed(rules, "/private/secret"), "prefix disallow");
        check(allowed(rules, "/private/public/info") && allowed(rules, "/public"), "longest allow");
        check(allowed(rules, "/Private/secret"), "case-sensitive paths");
        check(allowed("User-agent: *\nDisallow:\n", "/"), "empty disallow");
        check(!allowed("User-agent: *\nDisallow: /\n", "/any?q=1"), "root disallow");
        check(allowed("User-agent: *\nDisallow: /x\nAllow: /x", "/xyz"), "allow wins tie");
        check(allowed("User-agent: *\nAllow: /x\nDisallow: /x", "/xyz"), "tie independent of order");
        const auto groups = "User-agent: *\nDisallow: /\nUser-agent: other\nUser-agent: FLUXCRAWL\nDisallow: /only\nUser-agent: fluxcrawl\nDisallow: /also\n";
        check(allowed(groups, "/public") && !allowed(groups, "/only") && !allowed(groups, "/also"), "specific groups merged, wildcard excluded");
        check(!allowed(groups, "/public", "unknown"), "wildcard fallback");
        check(allowed("User-agent: other\nDisallow: /", "/x"), "no matching group");
        check(!allowed("User-agent: Flux\nDisallow: /", "/x", "Flux"), "exact group");
        check(allowed("User-agent: Flux\nDisallow: /", "/x"), "tokens are exact, not substring matches");
        check(!allowed("User-agent: *\nDisallow: /search?q=secret", "/search?q=secret&n=1"), "query participates");
        check(allowed("User-agent: *\nDisallow: /search?q=secret", "/search?q=public"), "query specificity");
        check(!allowed("User-agent: *\nDisallow: /%7Eprivate", "/~private"), "unreserved decoding");
        check(!allowed("User-agent: *\nDisallow: /encoded%23name # comment", "/encoded%23name"), "encoded hash is not comment");
        check(!allowed("User-agent: *\nDisallow: /caf\xC3\xA9", "/caf%C3%A9"), "UTF-8 percent comparison");
        check(allowed("User-agent: *\nDisallow: /a%2Fb", "/a/b"), "reserved escapes remain distinct");
        check(allowed("User-agent: *\nDisallow: /wild*\nDisallow: /end$", "/wildthing"), "no wildcard expansion");
        check(allowed("User-agent: *\nDisallow: /end$", "/end"), "no dollar expansion");
        bool rejected = false;
        try { (void)RobotsPolicy::parse(std::string(RobotsPolicy::max_bytes + 1, 'x')); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "parser bound");
        // One origin, globally deduplicated discoveries, one bootstrap.
        CrawlState state{url("/"), 2};
        for (int i = 0; i < 100; ++i) { (void)state.discover(url("/page/" + std::to_string(i)), 1); }
        (void)state.discover(url("/private/secret"), 1);
        CrawlPolicy policy{true}; HostScheduler scheduler{8}; CrawlResult result;
        policy.schedule(state, scheduler, result);
        check(result.robots_origins == 1 && policy.held() == 102 && scheduler.pending() == 1, "single bootstrap for 102 discoveries");
        check(scheduler.resource_pending() == 0 && !state.discover(url("/private/secret"), 1), "held URLs globally seen");
        auto bootstrap = scheduler.acquire({});
        check(bootstrap && bootstrap->item.policy_request && bootstrap->item.url.path() == "/robots.txt", "URL-based bootstrap construction");
        CrawlRecord record{bootstrap->item, CrawlResponseMetadata{bootstrap->item.url, 200, "text/plain", 0}, {}};
        policy.resolve(record, RobotsPolicy::parse(rules), scheduler, result);
        bootstrap.reset();
        check(policy.held() == 0 && result.urls_blocked_by_robots == 1 && result.robots_allowed == 101, "resolve held resources");
        check(!state.discover(url("/private/secret"), 1), "blocked stays seen");
        (void)state.discover(url("/robots.txt"), 1); policy.schedule(state, scheduler, result);
        check(result.robots_resources_reused == 1, "bootstrap response reused for direct discovery");
        auto other = Url::parse("http://example.test:8443/x").value();
        (void)state.discover(other, 1); policy.schedule(state, scheduler, result);
        check(result.robots_origins == 2 && policy.held() == 1, "nondefault port has independent cache");
        CrawlState ignored{url("/"), 0}; CrawlPolicy ignore{false}; HostScheduler unrestricted{1}; CrawlResult ordinary;
        ignore.schedule(ignored, unrestricted, ordinary);
        check(unrestricted.resource_pending() == 1 && ordinary.robots_origins == 0, "disabled policy bypass");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Robots parser, identity, precedence, origin cache and held-work checks passed\n";
}
