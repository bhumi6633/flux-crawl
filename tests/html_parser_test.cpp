#include <fluxcrawl/html_parser.hpp>

#include <libxml/parser.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Confined to this test process: fail before any external file or network I/O.
class ExternalLoadGuard {
public:
    ExternalLoadGuard() : previous_{xmlGetExternalEntityLoader()}
    {
        xmlSetExternalEntityLoader(reject);
    }
    ~ExternalLoadGuard() { xmlSetExternalEntityLoader(previous_); }
    ExternalLoadGuard(const ExternalLoadGuard&) = delete;
    ExternalLoadGuard& operator=(const ExternalLoadGuard&) = delete;

private:
    static xmlParserInputPtr reject(const char*, const char*, xmlParserCtxtPtr)
    {
        std::cerr << "FAIL: parser attempted external resource loading\n";
        std::abort();
    }
    xmlExternalEntityLoader previous_;
};

struct Case {
    std::string_view name;
    std::string_view html;
    std::vector<std::string_view> expected;
};

} // namespace

int main()
{
    const ExternalLoadGuard external_load_guard;
    const fluxcrawl::HtmlLinkExtractor extractor;
    const auto base = fluxcrawl::Url::parse("https://example.com/docs/index.html");
    if (!base) {
        std::cerr << "FAIL: test base URL\n";
        return 1;
    }

    const Case cases[] = {
        {"absolute anchors", R"html(<a href="https://example.com/a">A</a><a href="https://example.com/b">B</a>)html",
            {"https://example.com/a", "https://example.com/b"}},
        {"relative paths", R"html(<a href="guide.html">Guide</a><a href="../pricing">Price</a><a href="/api">API</a>)html",
            {"https://example.com/docs/guide.html", "https://example.com/pricing", "https://example.com/api"}},
        {"cross host and scheme", R"html(<a href="http://other.example:8080/a">A</a><a href="//OTHER.example/b">B</a>)html",
            {"http://other.example:8080/a", "https://other.example/b"}},
        {"canonical duplicates", R"html(<a href="/docs">1</a><a href="/docs">2</a><a href="/docs#intro">3</a><a href="https://EXAMPLE.com:443/docs">4</a><a href="/a/../docs">5</a>)html",
            {"https://example.com/docs"}},
        {"decoded trimmed repeats preserve first occurrence", R"html(<a href=" /one?a=1&amp;b=2 ">1</a><a href="/two">2</a><a href="&#47;one?a=1&#38;b=2">3</a><a href="/one?a=1&amp;b=2#x">4</a><a href="/three">5</a>)html",
            {"https://example.com/one?a=1&b=2", "https://example.com/two", "https://example.com/three"}},
        {"repeated invalid references do not suppress valid links", R"html(<a href="/%GG">1</a><a href="/one">2</a><a href="/%GG">3</a><a href="javascript:x">4</a><a href="javascript:x">5</a><a href="/two">6</a>)html",
            {"https://example.com/one", "https://example.com/two"}},
        {"fragment targets", R"html(<a href="/docs">1</a><a href="/docs#one">2</a><a href="/docs#two">3</a><a href="#intro">4</a><a href="#end">5</a>)html",
            {"https://example.com/docs", "https://example.com/docs/index.html"}},
        {"unsupported schemes", R"html(<a href="mailto:hello@example.com">M</a><a href="javascript:void(0)">J</a><a href="ftp://example.com/file">F</a><a href="data:text/plain,hello">D</a><a href="tel:+123456789">T</a><a href="/valid">V</a>)html",
            {"https://example.com/valid"}},
        {"invalid URLs", R"html(<a href="https:///bad">1</a><a href="http://">2</a><a href="http://host:bad">3</a><a href="/%GG">4</a><a href="/bad path">5</a><a href="https://user@host/">6</a><a href="/valid">7</a>)html",
            {"https://example.com/valid"}},
        {"empty document", "", {}},
        {"plain text", "Some plain text without any markup.", {}},
        {"no anchors", "<html><body><h1>Title</h1><p>Text</p></body></html>", {}},
        {"missing href", "<a name='here'>Named anchor</a>", {}},
        {"unrecoverable markup", "<", {}},
        {"recoverable markup", R"html(<html><body><a href="/one">One<a href="/two">Two</body>)html",
            {"https://example.com/one", "https://example.com/two"}},
        {"named entities", R"html(<a href="/search?a=1&amp;b=2">Search</a>)html",
            {"https://example.com/search?a=1&b=2"}},
        {"numeric entities", R"html(<a href="&#47;search?q=1&#38;x=&#x32;">Search</a>)html",
            {"https://example.com/search?q=1&x=2"}},
        {"case and attribute layout", "<A HREF = '/one'>One</A><a\n href=\"/two\">Two</a><a href=/three>Three</a>",
            {"https://example.com/one", "https://example.com/two", "https://example.com/three"}},
        {"surrounding href whitespace", "<a href=' \t/docs\r\n '>Docs</a><a href='&#32;/two&#9;'>Two</a>",
            {"https://example.com/docs", "https://example.com/two"}},
        {"distinct queries", R"html(<a href="/page?id=1">1</a><a href="/page?id=2">2</a><a href="/page?id=1#top">3</a>)html",
            {"https://example.com/page?id=1", "https://example.com/page?id=2"}},
        {"query order", R"html(<a href="/page?a=1&amp;b=2">1</a><a href="/page?b=2&amp;a=1">2</a>)html",
            {"https://example.com/page?a=1&b=2", "https://example.com/page?b=2&a=1"}},
        {"query only references", R"html(<a href="?q=1">1</a><a href="?">2</a><a href="">3</a><a href="  ">4</a>)html",
            {"https://example.com/docs/index.html?q=1", "https://example.com/docs/index.html?", "https://example.com/docs/index.html"}},
        {"discovery order", R"html(<a href="/a">A</a><div><a href="/b">B</a><a href="/a#x">A</a></div><a href="/c">C</a>)html",
            {"https://example.com/a", "https://example.com/b", "https://example.com/c"}},
        {"only anchors", R"html(<img src="/image"><script src="/script"></script><link href="/style"><iframe src="/frame"></iframe><form action="/form"></form><area href="/area"><a href="/anchor">A</a>)html",
            {"https://example.com/anchor"}},
        {"comments and script text", R"html(<!-- <a href="/comment">X</a> --><script>var a = '<a href="/script">X</a>';</script><a href="/real">R</a>)html",
            {"https://example.com/real"}},
        {"base element ignored", R"html(<html><head><base href="https://other.example/elsewhere/"></head><body><a href="guide">G</a></body></html>)html",
            {"https://example.com/docs/guide"}},
        {"external DTD ignored", R"html(<!DOCTYPE html SYSTEM "https://external.invalid/test.dtd"><html><body><a href="/local">L</a></body></html>)html",
            {"https://example.com/local"}},
        {"local DTD ignored", R"html(<!DOCTYPE html SYSTEM "file:///fluxcrawl-nonexistent.dtd"><html><body><a href="/local">L</a></body></html>)html",
            {"https://example.com/local"}},
        {"external entity declaration ignored", R"html(<!DOCTYPE html [<!ENTITY remote SYSTEM "https://external.invalid/entity">]><html><body>&remote;<a href="/local">L</a></body></html>)html",
            {"https://example.com/local"}},
        {"encoded URL bytes", R"html(<a href="/a%2Fb?q=%26">A</a><a href="/a/b?q=%26">B</a>)html",
            {"https://example.com/a%2Fb?q=%26", "https://example.com/a/b?q=%26"}},
        {"IPv6 anchor", R"html(<a href="http://[::1]:8080/docs">A</a>)html",
            {"http://[::1]:8080/docs"}},
    };

    int failures = 0;
    int checks = 0;
    const auto check = [&](std::string_view name, std::string_view html,
                           const std::vector<std::string_view>& expected) {
        ++checks;
        const auto links = extractor.extract_links(html, *base);
        bool matches = links.size() == expected.size();
        for (std::size_t index = 0; matches && index < links.size(); ++index) {
            matches = links[index].canonical() == expected[index];
        }
        if (!matches) {
            ++failures;
            std::cerr << "FAIL: " << name << '\n';
            for (const auto& url : links) {
                std::cerr << "  actual: " << url.canonical() << '\n';
            }
        }
    };
    for (const auto& test : cases) {
        check(test.name, test.html, test.expected);
    }

    const std::string first = "<a href='/included'>I</a>";
    const std::string buffer = first + "<a href='/excluded'>E</a>";
    check("bounded string_view", std::string_view{buffer}.substr(0, first.size()), {"https://example.com/included"});
    check("binary garbage", std::string(16, '\0'), {});
    std::string repeated;
    for (int index = 0; index < 1000; ++index) {
        repeated += "<a href='/repeat#top'>R</a>";
    }
    check("many duplicates", repeated, {"https://example.com/repeat"});
    for (int iteration = 0; iteration < 100; ++iteration) {
        check("independent calls", "<a href='/repeat'>R</a>", {"https://example.com/repeat"});
    }
    // Exercise owning-reference lifetimes beyond small-string storage, including rehashes.
    std::string unique_html;
    std::vector<std::string> canonical;
    for (int index = 0; index < 300; ++index) {
        const auto reference = "/" + std::string(80, 'a') + std::to_string(index);
        unique_html += "<a href='" + reference + "'>U</a>";
        canonical.push_back("https://example.com" + reference);
    }
    std::vector<std::string_view> expected_unique;
    for (const auto& value : canonical) { expected_unique.push_back(value); }
    check("unique long hrefs and repeated document preserve order", unique_html + unique_html, expected_unique);
    for (const auto host : {"first.example", "second.example"}) {
        ++checks;
        const auto other_base = fluxcrawl::Url::parse(std::string{"https://"} + host + "/dir/index").value();
        const auto links = extractor.extract_links("<a href='child'>1</a><a href='child'>2</a>", other_base);
        if (links.size() != 1 || links.front().canonical() != std::string{"https://"} + host + "/dir/child") {
            ++failures;
            std::cerr << "FAIL: references never leak across bases or documents\n";
        }
    }
    std::cout << checks << " HTML checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
