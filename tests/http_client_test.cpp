#include <fluxcrawl/http_client.hpp>

#include <curl/curl.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

int main(int argc, char* argv[])
{
    if (argc != 3) { return 2; }
    int checks = 0;
    int failures = 0;
    const auto check = [&](bool condition, std::string_view name) {
        ++checks;
        if (!condition) {
            ++failures;
            std::cerr << "FAIL: " << name << '\n';
        }
    };
    const std::string origin{argv[1]};
    const auto parse = [](const std::string& text) {
        const auto url = fluxcrawl::Url::parse(text);
        if (!url) { throw std::runtime_error("Invalid fixture URL"); }
        return *url;
    };
    const fluxcrawl::HttpClient client;
    const auto response = [&](std::string_view path, long status, std::string_view body,
                              std::string_view type) {
        const auto requested = parse(origin + std::string{path});
        const auto result = client.fetch(requested);
        const auto* value = std::get_if<fluxcrawl::HttpResponse>(&result);
        check(value != nullptr, path);
        if (value) {
            check(value->status_code == status, "HTTP status");
            check(value->body == body, "exact body bytes");
            check(value->downloaded_bytes == body.size(), "decoded byte count");
            check(value->content_type == type, "content type");
            check(value->requested_url == requested, "requested URL");
            check(value->effective_url == parse(origin + (path == "/redirect" ? "/html" : std::string{path})), "effective URL");
        } else {
            std::cerr << std::get<fluxcrawl::HttpError>(result).message << '\n';
        }
    };
    response("/ok", 200, "hello", "text/plain");
    response("/html", 200, "<a href='next'>Next</a>", "text/html; charset=UTF-8");
    response("/redirect", 200, "<a href='next'>Next</a>", "text/html; charset=UTF-8");
    response("/not-found", 404, "not found", "text/plain");
    response("/server-error", 503, "unavailable", "text/plain");
    const char binary[] = {0, 1, static_cast<char>(255), 'A', 0, 'B'};
    response("/binary", 200, std::string_view{binary, sizeof(binary)}, "application/octet-stream");
    response("/no-content-type", 200, "hello", "");
    response("/chunked", 200, std::string_view{"abc\0defghi", 10}, "");
    response("/empty", 204, "", "text/plain");
    response("/large", 200, std::string(65536, 'x'), "text/plain");
    response("/agent", 200, "FluxCrawl/" + std::string{fluxcrawl::version}, "text/plain");
    std::string decoded;
    for (int i = 0; i < 1024; ++i) { decoded.append("decoded\0body", 12); }
    response("/gzip", 200, decoded, "text/plain");

    const auto error = [&](const fluxcrawl::HttpClient& configured, const fluxcrawl::Url& target,
                           fluxcrawl::HttpErrorCode category, int curl_code) {
        const auto result = configured.fetch(target);
        const auto* value = std::get_if<fluxcrawl::HttpError>(&result);
        check(value != nullptr, "failure returns error variant");
        if (value) {
            check(value->code == category, "error category");
            check(!value->message.empty(), "useful diagnostic");
            check(value->curl_code == curl_code, "original curl code");
            if (value->curl_code != curl_code) {
                std::cerr << target.canonical() << ": expected " << curl_code
                          << ", got " << value->curl_code << " (" << value->message << ")\n";
            }
        }
    };
    fluxcrawl::HttpClientOptions options;
    options.max_body_bytes = 5;
    fluxcrawl::HttpClient limited{options};
    check(std::holds_alternative<fluxcrawl::HttpResponse>(limited.fetch(parse(origin + "/ok"))), "exact size boundary accepted");
    error(limited, parse(origin + "/large"), fluxcrawl::HttpErrorCode::BodyTooLarge, CURLE_WRITE_ERROR);
    error(limited, parse(origin + "/gzip"), fluxcrawl::HttpErrorCode::BodyTooLarge, CURLE_WRITE_ERROR);
    options.max_body_bytes = 0;
    fluxcrawl::HttpClient zero{options};
    check(std::holds_alternative<fluxcrawl::HttpResponse>(zero.fetch(parse(origin + "/empty"))), "zero limit accepts empty body");
    error(zero, parse(origin + "/ok"), fluxcrawl::HttpErrorCode::BodyTooLarge, CURLE_WRITE_ERROR);
    options = {};
    options.connect_timeout = std::chrono::milliseconds{100};
    const auto unavailable = fluxcrawl::HttpClient{options}.fetch(parse(argv[2]));
    const auto* connection_error = std::get_if<fluxcrawl::HttpError>(&unavailable);
    check(connection_error && connection_error->code == fluxcrawl::HttpErrorCode::CurlError
          && !connection_error->message.empty()
          && (connection_error->curl_code == CURLE_COULDNT_CONNECT
              || connection_error->curl_code == CURLE_OPERATION_TIMEDOUT),
          "reserved non-listening endpoint fails (refused or timed out by OS)");
    error(client, parse(origin + "/loop"), fluxcrawl::HttpErrorCode::CurlError, CURLE_TOO_MANY_REDIRECTS);
    error(client, parse(origin + "/forbidden"), fluxcrawl::HttpErrorCode::CurlError, CURLE_UNSUPPORTED_PROTOCOL);
    error(client, parse(origin + "/invalid-effective"), fluxcrawl::HttpErrorCode::InvalidEffectiveUrl, 0);
    error(client, parse(origin + "/truncated"), fluxcrawl::HttpErrorCode::CurlError, CURLE_PARTIAL_FILE);
    options = {};
    options.max_redirects = 0;
    error(fluxcrawl::HttpClient{options}, parse(origin + "/redirect"), fluxcrawl::HttpErrorCode::CurlError, CURLE_TOO_MANY_REDIRECTS);
    options = {};
    options.user_agent = "FluxCrawl-test";
    const auto custom_agent = fluxcrawl::HttpClient{options}.fetch(parse(origin + "/agent"));
    const auto* agent_response = std::get_if<fluxcrawl::HttpResponse>(&custom_agent);
    check(agent_response && agent_response->body == "FluxCrawl-test", "custom user agent");
    for (int invalid = 0; invalid < 5; ++invalid) {
        options = {};
        if (invalid == 0) { options.connect_timeout = std::chrono::milliseconds{0}; }
        if (invalid == 1) { options.request_timeout = std::chrono::milliseconds{-1}; }
        if (invalid == 2) { options.max_redirects = -1; }
        if (invalid == 3) { options.user_agent = "bad\r\nheader"; }
        if (invalid == 4) { options.user_agent = std::string{"bad\0agent", 9}; }
        bool rejected = false;
        try { const fluxcrawl::HttpClient invalid_client{options}; }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "invalid options rejected");
    }
    // Last request: the sequential fixture intentionally pauses before replying.
    options = {};
    options.request_timeout = std::chrono::milliseconds{50};
    error(fluxcrawl::HttpClient{options}, parse(origin + "/slow"), fluxcrawl::HttpErrorCode::CurlError, CURLE_OPERATION_TIMEDOUT);
    std::cout << checks << " HTTP checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
