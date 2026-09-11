#include <fluxcrawl/crawl_state.hpp>
#include <fluxcrawl/html_parser.hpp>

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

fluxcrawl::Url url(std::string_view text)
{
    const auto parsed = fluxcrawl::Url::parse(text);
    if (!parsed) {
        throw std::runtime_error("Invalid test fixture URL: " + std::string{text});
    }
    return *parsed;
}

class Checks {
public:
    void require(bool condition, std::string_view name)
    {
        ++total_;
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << name << '\n';
        }
    }
    void item(const std::optional<fluxcrawl::CrawlItem>& actual,
              const fluxcrawl::Url& expected, std::size_t depth)
    {
        require(actual && actual->url == expected && actual->depth == depth, expected.canonical());
    }
    int result() const
    {
        std::cout << total_ << " crawl-state checks, " << failures_ << " failures\n";
        return failures_ == 0 ? 0 : 1;
    }
private:
    int total_ = 0;
    int failures_ = 0;
};

void frontier(Checks& check)
{
    fluxcrawl::CrawlFrontier queue;
    const auto a = url("https://example.com/a");
    const auto b = url("https://example.com/b");
    check.require(queue.empty() && queue.size() == 0 && !queue.pop(), "empty frontier");
    queue.push({a, 2});
    queue.push({b, 1});
    queue.push({a, 5});
    check.require(queue.size() == 3 && !queue.empty(), "frontier stores work without dedup or depth sorting");
    check.item(queue.pop(), a, 2);
    check.require(queue.size() == 2, "pop size");
    queue.push({b, 9});
    check.item(queue.pop(), b, 1);
    check.item(queue.pop(), a, 5);
    check.item(queue.pop(), b, 9);
    check.require(queue.empty() && queue.size() == 0 && !queue.pop() && !queue.pop(), "repeated empty pop");
    queue.push({a, 0});
    check.item(queue.pop(), a, 0);
}

void discovery(Checks& check)
{
    const auto seed = url("https://example.com");
    fluxcrawl::CrawlState state{seed, 3};
    check.require(state.seen_count() == 1 && state.frontier_size() == 1 && !state.empty(), "seed counters");
    check.require(state.has_seen(url("https://EXAMPLE.com:443/#top")), "seed marked seen immediately");
    check.item(state.next(), seed, 0);
    check.require(state.empty() && state.seen_count() == 1 && !state.next(), "popping keeps seed seen");
    check.require(!state.discover(seed, 1), "processed seed not requeued");
    const auto docs = url("https://EXAMPLE.com/docs");
    check.require(state.discover(docs, 2), "new URL accepted");
    check.require(!state.discover(url("https://example.com/docs#intro"), 1), "fragment duplicate");
    check.require(!state.discover(url("https://example.com:443/docs"), 3), "default port duplicate");
    check.require(state.frontier_size() == 1 && state.seen_count() == 2, "duplicates leave counters unchanged");
    check.item(state.next(), docs, 2);
    check.require(!state.discover(docs, 0), "first discovery remains seen after pop");
    const auto q1 = url("https://example.com/page?id=1");
    const auto q2 = url("https://example.com/page?id=2");
    const auto other = url("http://other.example/page");
    check.require(state.discover(q1, 1) && state.discover(q2, 1), "queries distinct");
    check.require(state.discover(other, 1), "cross-domain and scheme accepted");
    check.item(state.next(), q1, 1);
    check.item(state.next(), q2, 1);
    check.item(state.next(), other, 1);
    check.require(state.seen_count() == 5 && state.frontier_size() == 0, "seen count survives drain");
    fluxcrawl::CrawlState independent{seed, 3};
    check.require(independent.discover(docs, 1), "seen set belongs to one crawl");
}

void breadth_first(Checks& check)
{
    const auto a = url("https://example.com/A");
    const auto b = url("https://example.com/B");
    const auto c = url("https://example.com/C");
    const auto d = url("https://example.com/D");
    const auto e = url("https://example.com/E");
    const auto f = url("https://example.com/F");
    const auto g = url("https://example.com/G");
    fluxcrawl::CrawlState state{a, 2};
    const auto first = state.next();
    check.item(first, a, 0);
    if (!first) { return; }
    check.require(state.discover_from(*first, b), "child B");
    check.require(state.discover_from(*first, c), "child C");
    check.require(state.discover_from(*first, d), "child D");
    check.require(state.frontier_size() == 3 && state.seen_count() == 4, "first wave counts");
    const auto second = state.next();
    check.item(second, b, 1);
    if (!second) { return; }
    check.require(state.discover_from(*second, e) && state.discover_from(*second, f), "grandchildren depth two");
    check.require(!state.discover_from(*second, c), "shallower queued C retains first discovery");
    const auto third = state.next();
    check.item(third, c, 1);
    if (!third) { return; }
    check.require(state.discover_from(*third, g), "second parent discovers G");
    check.require(!state.discover_from(*third, e), "cross-parent global dedup");
    check.require(!state.discover_from(*third, a), "cycle to seed rejected");
    check.item(state.next(), d, 1);
    check.item(state.next(), e, 2);
    check.item(state.next(), f, 2);
    check.item(state.next(), g, 2);
    check.require(state.empty() && state.seen_count() == 7 && !state.next(), "BFS complete");
}

void depth_limits(Checks& check)
{
    const auto seed = url("https://example.com/");
    const auto child = url("https://example.com/child");
    const auto grandchild = url("https://example.com/grandchild");
    fluxcrawl::CrawlState zero{seed, 0};
    check.item(zero.next(), seed, 0);
    check.require(!zero.discover_from({seed, 0}, child), "zero maximum blocks children");
    check.require(!zero.has_seen(child) && zero.seen_count() == 1 && zero.empty(), "rejection leaves state intact");
    fluxcrawl::CrawlState state{seed, 2};
    check.require(!state.discover(child, 3), "over-depth rejected");
    check.require(!state.has_seen(child) && state.seen_count() == 1 && state.frontier_size() == 1, "rejected URL not seen");
    check.require(state.discover(child, 2), "same URL accepted later at inclusive limit");
    check.require(!state.discover_from({child, 2}, grandchild), "child of maximum depth rejected");
    check.require(!state.has_seen(grandchild), "blocked descendant not seen");
    check.require(state.discover_from({child, 1}, grandchild), "later in-range descendant accepted");
    check.item(state.next(), seed, 0);
    check.item(state.next(), child, 2);
    check.item(state.next(), grandchild, 2);
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    fluxcrawl::CrawlState unlimited{seed, maximum};
    check.require(!unlimited.discover_from({seed, maximum}, child), "child depth cannot overflow");
    check.require(!unlimited.has_seen(child), "overflow leaves seen intact");
    check.require(unlimited.discover_from({seed, maximum - 1}, child), "SIZE_MAX itself is accepted");
    check.item(unlimited.next(), seed, 0);
    check.item(unlimited.next(), child, maximum);
}

void parser_composition(Checks& check)
{
    fluxcrawl::CrawlState state{url("https://example.com/"), 2};
    const fluxcrawl::HtmlLinkExtractor parser;
    const auto seed = state.next();
    check.require(seed.has_value(), "composition seed");
    if (!seed) { return; }
    const auto links = parser.extract_links("<a href='/a'>A</a><a href='/b'>B</a>", seed->url);
    check.require(links.size() == 2, "parser yields first wave");
    for (const auto& link : links) {
        check.require(state.discover_from(*seed, link), "accept parsed URL");
    }
    const auto a = state.next();
    const auto b = state.next();
    check.item(a, url("https://example.com/a"), 1);
    check.item(b, url("https://example.com/b"), 1);
    if (!a || !b) { return; }
    const auto from_a = parser.extract_links("<a href='/shared'>S</a>", a->url);
    const auto from_b = parser.extract_links("<a href='https://EXAMPLE.com:443/shared#top'>S</a>", b->url);
    check.require(from_a.size() == 1 && from_b.size() == 1, "both pages report same resource");
    if (from_a.empty() || from_b.empty()) { return; }
    check.require(state.discover_from(*a, from_a.front()), "first page discovery accepted");
    check.require(!state.discover_from(*b, from_b.front()), "second page discovery deduplicated");
    check.item(state.next(), url("https://example.com/shared"), 2);
    check.require(state.empty() && state.seen_count() == 4, "composition counts");
}

} // namespace

int main()
{
    Checks checks;
    frontier(checks);
    discovery(checks);
    breadth_first(checks);
    depth_limits(checks);
    parser_composition(checks);
    return checks.result();
}
