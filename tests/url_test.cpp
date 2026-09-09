#include <fluxcrawl/url.hpp>

#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace {

struct Case {
    std::string_view input;
    std::string_view expected;
};

class Checks {
public:
    void require(bool condition, std::string_view label)
    {
        ++total_;
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << label << '\n';
        }
    }

    int result() const
    {
        std::cout << total_ << " checks, " << failures_ << " failures\n";
        return failures_ == 0 ? 0 : 1;
    }

private:
    int total_ = 0;
    int failures_ = 0;
};

void parsing(Checks& checks)
{
    constexpr Case cases[] = {
        {"http://example.com", "http://example.com/"},
        {"https://example.com", "https://example.com/"},
        {"http://localhost:8080/path", "http://localhost:8080/path"},
        {"https://example.com/path?q=hello", "https://example.com/path?q=hello"},
        {"HTTPS://EXAMPLE.COM", "https://example.com/"},
        {"https://example.com/docs#intro", "https://example.com/docs"},
        {"https://example.com/docs#installation", "https://example.com/docs"},
        {"http://example.com:80/", "http://example.com/"},
        {"https://example.com:443/", "https://example.com/"},
        {"https://example.com:0443/", "https://example.com/"},
        {"https://example.com:80/", "https://example.com:80/"},
        {"http://example.com:443/", "http://example.com:443/"},
        {"http://localhost:0/", "http://localhost:0/"},
        {"http://localhost:65535/", "http://localhost:65535/"},
        {"https://example.com/a/b/../c", "https://example.com/a/c"},
        {"https://example.com/a/./b", "https://example.com/a/b"},
        {"https://example.com/a/..", "https://example.com/"},
        {"https://example.com/a/.", "https://example.com/a/"},
        {"https://example.com/../../x", "https://example.com/x"},
        {"https://example.com/a//b/", "https://example.com/a//b/"},
        {"https://example.com/a/%2e%2e/b", "https://example.com/a/%2e%2e/b"},
        {"https://example.com/A%2fb", "https://example.com/A%2fb"},
        {"https://example.com?b=2&a=1&a=3#top", "https://example.com/?b=2&a=1&a=3"},
        {"https://example.com/?", "https://example.com/?"},
        {"https://example.com/?#top", "https://example.com/?"},
        {"https://example.com/#?ignored", "https://example.com/"},
        {"https://example.com/?q=%2F+%20&x=/../?", "https://example.com/?q=%2F+%20&x=/../?"},
        {"http://127.0.0.1:8080/", "http://127.0.0.1:8080/"},
        {"https://[::1]:443/a", "https://[::1]/a"},
        {"https://[2001:DB8::1]:8443/", "https://[2001:db8::1]:8443/"},
        {"https://xn--bcher-kva.example/", "https://xn--bcher-kva.example/"},
    };
    for (const auto& test : cases) {
        const auto url = fluxcrawl::Url::parse(test.input);
        checks.require(url && url->canonical() == test.expected, test.input);
        if (url) {
            checks.require(fluxcrawl::Url::parse(url->canonical()) == url, "canonical round trip");
            checks.require(url->str() == url->canonical(), "str is canonical");
        }
    }
    const auto url = fluxcrawl::Url::parse("HTTP://LOCALHOST:8080/docs?q=test#top");
    checks.require(url.has_value(), "component URL parses");
    if (url) {
        checks.require(url->scheme() == "http" && url->is_http() && !url->is_https(), "scheme accessors");
        checks.require(url->host() == "localhost" && url->port() == 8080, "authority accessors");
        checks.require(url->path() == "/docs" && url->query() == "q=test" && url->has_query(), "path/query accessors");
    }
    const auto default_port = fluxcrawl::Url::parse("https://example.com:443/");
    checks.require(default_port && !default_port->port() && !default_port->has_query()
                   && default_port->query().empty() && default_port->is_https(), "default components");
    const std::string padded = "https://example.com/unwanted";
    checks.require(fluxcrawl::Url::parse(std::string_view{padded}.substr(0, 19))
                   == fluxcrawl::Url::parse("https://example.com"), "bounded string_view input");
}

void invalid_inputs(Checks& checks)
{
    constexpr std::string_view invalid[] = {
        "", "hello", "://bad", "http://", "https:///missing-host", "https:////host",
        "https:/host", "//example.com", "ftp://example.com", "file:///tmp/file",
        "mailto:user@example.com", "javascript:alert(1)", "data:text/plain,hello",
        "http://example.com:", "http://example.com:abc", "http://example.com:-1",
        "http://example.com:+80", "http://example.com:65536", "http://example.com:9999999999999999",
        "http://example.com:80x", "http://example.com:80:90", "http://:80/",
        "http://[::1", "http://::1/", "http://[invalid]/", "http://[::1]extra/",
        "https://user:password@example.com/", "https://@example.com/",
        "https://exa mple.com/", " https://example.com", "https://example.com/ ",
        "https://example.com/\n", "https://example.com/\t", "https://example.com/\\path",
        "https://example.com/%", "https://example.com/%0", "https://example.com/%GG",
        "https://example.com/?q=%xy", "https://example.com/#%xx",
        "https://%65xample.com/", "http://[fe80::1%25en0]/", "https://bücher.example/",
    };
    for (const auto input : invalid) {
        checks.require(!fluxcrawl::Url::parse(input), input);
    }
    const std::string embedded_nul{"https://example.com/\0hidden", 27};
    checks.require(!fluxcrawl::Url::parse(embedded_nul), "embedded NUL rejected");
}

void resolution(Checks& checks)
{
    const auto base = fluxcrawl::Url::parse("http://a/b/c/d;p?q");
    checks.require(base.has_value(), "RFC example base");
    if (!base) {
        return;
    }
    // RFC 3986 section 5.4, restricted to HTTP(S), with fragments removed.
    constexpr Case cases[] = {
        {"g", "http://a/b/c/g"}, {"./g", "http://a/b/c/g"},
        {"g/", "http://a/b/c/g/"}, {"/g", "http://a/g"}, {"//g", "http://g/"},
        {"?y", "http://a/b/c/d;p?y"}, {"g?y", "http://a/b/c/g?y"},
        {"#s", "http://a/b/c/d;p?q"}, {"g#s", "http://a/b/c/g"},
        {"g?y#s", "http://a/b/c/g?y"}, {";x", "http://a/b/c/;x"},
        {"g;x", "http://a/b/c/g;x"}, {"g;x?y#s", "http://a/b/c/g;x?y"},
        {"", "http://a/b/c/d;p?q"}, {".", "http://a/b/c/"}, {"./", "http://a/b/c/"},
        {"..", "http://a/b/"}, {"../", "http://a/b/"}, {"../g", "http://a/b/g"},
        {"../..", "http://a/"}, {"../../", "http://a/"}, {"../../g", "http://a/g"},
        {"../../../g", "http://a/g"}, {"../../../../g", "http://a/g"},
        {"/./g", "http://a/g"}, {"/../g", "http://a/g"},
        {"g.", "http://a/b/c/g."}, {".g", "http://a/b/c/.g"},
        {"g..", "http://a/b/c/g.."}, {"..g", "http://a/b/c/..g"},
        {"./../g", "http://a/b/g"}, {"./g/.", "http://a/b/c/g/"},
        {"g/./h", "http://a/b/c/g/h"}, {"g/../h", "http://a/b/c/h"},
        {"g;x=1/./y", "http://a/b/c/g;x=1/y"}, {"g;x=1/../y", "http://a/b/c/y"},
        {"g?y/./x", "http://a/b/c/g?y/./x"}, {"g?y/../x", "http://a/b/c/g?y/../x"},
        {"g#s/./x", "http://a/b/c/g"}, {"g#s/../x", "http://a/b/c/g"},
        {"?", "http://a/b/c/d;p?"}, {"?#top", "http://a/b/c/d;p?"},
        {"#", "http://a/b/c/d;p?q"}, {"g?", "http://a/b/c/g?"},
        {"https://OTHER.example:443/a/../test#top", "https://other.example/test"},
        {"//OTHER.example:8080/path?x#top", "http://other.example:8080/path?x"},
        {"./a:b", "http://a/b/c/a:b"}, {"%2e%2e/g", "http://a/b/c/%2e%2e/g"},
    };
    for (const auto& test : cases) {
        const auto url = fluxcrawl::Url::resolve(*base, test.input);
        checks.require(url && url->canonical() == test.expected, std::string{"resolve "} + std::string{test.input});
    }
    constexpr std::string_view invalid[] = {
        "ftp://example.com", "javascript:alert(1)", "http:g", "https:///bad", "//",
        "///missing-host", "//host:bad", "//user@host", "bad path", "%xy", "#%xy",
    };
    for (const auto reference : invalid) {
        checks.require(!fluxcrawl::Url::resolve(*base, reference), reference);
    }
    struct ResolutionCase { std::string_view base; std::string_view reference; std::string_view expected; };
    constexpr ResolutionCase examples[] = {
        {"https://example.com/products/item/index.html", "../help.html", "https://example.com/products/help.html"},
        {"https://example.com/docs/index.html", "/api", "https://example.com/api"},
        {"https://example.com/docs/", "guide.html", "https://example.com/docs/guide.html"},
        {"https://example.com", "guide", "https://example.com/guide"},
        {"https://[::1]:8443/docs/?", "#top", "https://[::1]:8443/docs/?"},
        {"https://[::1]:8443/docs/?", "../guide", "https://[::1]:8443/guide"},
    };
    for (const auto& test : examples) {
        const auto example_base = fluxcrawl::Url::parse(test.base);
        const auto result = example_base ? fluxcrawl::Url::resolve(*example_base, test.reference) : std::nullopt;
        checks.require(result && result->canonical() == test.expected, test.reference);
    }
    checks.require(base->canonical() == "http://a/b/c/d;p?q", "resolution leaves base unchanged");
}

void identity(Checks& checks)
{
    std::unordered_set<fluxcrawl::Url> urls;
    constexpr std::string_view equivalent[] = {
        "https://EXAMPLE.com", "https://example.com/", "https://example.com/#top",
        "https://example.com:443/", "HTTPS://example.com/a/..",
    };
    for (const auto input : equivalent) {
        const auto url = fluxcrawl::Url::parse(input);
        checks.require(url.has_value(), input);
        if (url) {
            urls.insert(*url);
            checks.require(*url == *urls.begin(), "canonical equality");
            checks.require(std::hash<fluxcrawl::Url>{}(*url) == std::hash<fluxcrawl::Url>{}(*urls.begin()), "equal hashes");
        }
    }
    checks.require(urls.size() == 1, "unordered_set deduplicates");
    constexpr std::string_view distinct[] = {
        "https://example.com/a", "https://example.com/A", "https://example.com/a/",
        "https://example.com/?a=1", "https://example.com/?a=2", "https://example.com/?",
        "https://example.com:8443/", "https://example.com:8444/", "http://example.com/",
        "https://other.example/", "https://example.com/?a=1&b=2", "https://example.com/?b=2&a=1",
    };
    for (const auto input : distinct) {
        const auto url = fluxcrawl::Url::parse(input);
        checks.require(url && urls.insert(*url).second, input);
    }
}

} // namespace

int main()
{
    Checks checks;
    parsing(checks);
    invalid_inputs(checks);
    resolution(checks);
    identity(checks);
    return checks.result();
}
