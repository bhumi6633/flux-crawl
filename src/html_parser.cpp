#include <fluxcrawl/html_parser.hpp>

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

namespace fluxcrawl {
namespace {

struct XmlStringDeleter {
    void operator()(xmlChar* value) const noexcept { xmlFree(value); }
};

std::string_view trim_html_space(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\n\r\f";
    const auto start = text.find_first_not_of(whitespace);
    if (start == std::string_view::npos) {
        return {};
    }
    return text.substr(start, text.find_last_not_of(whitespace) - start + 1);
}

} // namespace

HtmlLinkExtractor::HtmlLinkExtractor()
{
    // Initialize once before worker creation; never clean up globals per document.
    static const bool initialized = [] { xmlInitParser(); return true; }();
    (void)initialized;
}

std::vector<Url> HtmlLinkExtractor::extract_links(std::string_view html, const Url& base_url) const
{
    // libxml2's memory API takes an int length. Do not truncate oversized input.
    if (html.empty() || html.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    constexpr int options = HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING
        | HTML_PARSE_NONET | HTML_PARSE_NODEFDTD;
    const std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document{
        htmlReadMemory(html.data(), static_cast<int>(html.size()), nullptr, nullptr, options), xmlFreeDoc};
    if (!document) {
        return {};
    }
    const std::unique_ptr<xmlXPathContext, decltype(&xmlXPathFreeContext)> context{
        xmlXPathNewContext(document.get()), xmlXPathFreeContext};
    if (!context) {
        return {};
    }
    const std::unique_ptr<xmlXPathObject, decltype(&xmlXPathFreeObject)> matches{
        xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>("//a/@href"), context.get()),
        xmlXPathFreeObject};
    if (!matches || matches->type != XPATH_NODESET || !matches->nodesetval) {
        return {};
    }

    auto* nodes = matches->nodesetval;
    xmlXPathNodeSetSort(nodes);
    std::unordered_set<Url> seen;
    // The base is fixed for this document. Own decoded/trimmed references: href
    // storage is freed each iteration, and distinct references can still canonicalize equally.
    std::unordered_set<std::string> references;
    std::vector<Url> links;
    for (int index = 0; index < nodes->nodeNr; ++index) {
        const std::unique_ptr<xmlChar, XmlStringDeleter> href{xmlNodeGetContent(nodes->nodeTab[index])};
        if (!href) {
            continue;
        }
        const auto reference = trim_html_space(reinterpret_cast<const char*>(href.get()));
        if (!references.emplace(reference).second) { continue; }
        auto url = Url::resolve(base_url, reference);
        if (url && seen.insert(*url).second) {
            links.push_back(std::move(*url));
        }
    }
    return links;
}

} // namespace fluxcrawl
