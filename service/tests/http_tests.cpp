#include <string>

#include <catch2/catch_test_macros.hpp>

#include "api/http.hpp"

using namespace houseguest::service::api;

namespace {
ParseResult parse(const std::string& text) {
    return parse_request(text);
}
}  // namespace

TEST_CASE("a plain request parses with headers and query", "[http]") {
    const auto r =
        parse("GET /v1/status?x=1 HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer abc\r\n\r\n");
    REQUIRE(r.status == ParseStatus::Complete);
    CHECK(r.request.method == "GET");
    CHECK(r.request.target == "/v1/status");
    CHECK(r.request.query == "x=1");
    CHECK(r.request.header("AUTHORIZATION") == "Bearer abc");
}

TEST_CASE("a body is read by Content-Length and waits for missing bytes", "[http]") {
    const std::string head = "POST /v1/x HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\n";
    CHECK(parse(head + "ab").status == ParseStatus::NeedMore);
    const auto r = parse(head + "abcdeEXTRA");
    REQUIRE(r.status == ParseStatus::Complete);
    CHECK(r.request.body == "abcde");
    CHECK(r.consumed == head.size() + 5);
}

TEST_CASE("an incomplete header section needs more data", "[http]") {
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\n").status == ParseStatus::NeedMore);
}

TEST_CASE("request smuggling shapes are rejected", "[http][security]") {
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n").http_status == 501);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx").status ==
          ParseStatus::Invalid);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n Folded: x\r\n\r\nx").status ==
          ParseStatus::Invalid);
    CHECK(parse("GET / HTTP/1.1\nHost: a\r\n\r\n").status == ParseStatus::Invalid);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\rX: y\r\n\r\n").status == ParseStatus::Invalid);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nX: y\x01z\r\n\r\n").status == ParseStatus::Invalid);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n").status == ParseStatus::Invalid);
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1x\r\n\r\n").status == ParseStatus::Invalid);
    CHECK(parse("GET / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n\r\n").http_status == 417);
}

TEST_CASE("targets cannot traverse, encode or use absolute form", "[http][security]") {
    CHECK(parse("GET /a/../b HTTP/1.1\r\nHost: a\r\n\r\n").status == ParseStatus::Invalid);
    CHECK(parse("GET /a%2e%2e/b HTTP/1.1\r\nHost: a\r\n\r\n").status == ParseStatus::Invalid);
    CHECK(parse("GET http://evil/ HTTP/1.1\r\nHost: a\r\n\r\n").status == ParseStatus::Invalid);
    CHECK(parse("GET //evil HTTP/1.1\r\nHost: a\r\n\r\n").status == ParseStatus::Invalid);
    CHECK(parse("GET /a\\b HTTP/1.1\r\nHost: a\r\n\r\n").status == ParseStatus::Invalid);
}

TEST_CASE("versions other than HTTP/1.1 are refused", "[http]") {
    CHECK(parse("GET / HTTP/1.0\r\nHost: a\r\n\r\n").http_status == 505);
    CHECK(parse("GET / FTP/1.1\r\nHost: a\r\n\r\n").status == ParseStatus::Invalid);
}

TEST_CASE("size limits are enforced before allocation", "[http][security]") {
    CHECK(parse("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 999999999\r\n\r\n").http_status == 413);
    CHECK(parse("GET / HTTP/1.1\r\n" + std::string(kMaxHeaderBytes + 1, 'a')).status ==
          ParseStatus::TooLarge);
    std::string many = "GET / HTTP/1.1\r\n";
    for (std::size_t i = 0; i <= kMaxHeaders; ++i) many += "X-" + std::to_string(i) + ": v\r\n";
    CHECK(parse(many + "\r\n").http_status == 431);
}

TEST_CASE("responses carry no-store and nosniff", "[http][security]") {
    Response response;
    response.body = "{}";
    const auto text = serialize(response);
    CHECK(text.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(text.find("Cache-Control: no-store") != std::string::npos);
    CHECK(text.find("X-Content-Type-Options: nosniff") != std::string::npos);
    CHECK(text.find("Content-Length: 2") != std::string::npos);
    CHECK(text.ends_with("\r\n\r\n{}"));
}
