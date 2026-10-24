#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include "sctui/core/types.hpp"

namespace sctui::net {

struct HttpResponse {
    int status_code{0};
    std::string body;
    std::unordered_map<std::string, std::string> headers;

    [[nodiscard]] bool is_2xx() const noexcept {
        return status_code >= 200 && status_code < 300;
    }
};

using DataCallback = std::function<bool(const char* data, size_t size)>;

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) noexcept;
    HttpClient& operator=(HttpClient&&) noexcept;

    void set_user_agent(std::string user_agent);
    void set_timeout_seconds(long seconds);
    void set_bearer_token(std::string token);
    void set_doh_url(std::string doh_url);
    void set_dns_servers(std::string dns_servers);
    void set_proxy(std::string proxy);

    [[nodiscard]] const std::string& get_doh_url() const noexcept;
    [[nodiscard]] const std::string& get_dns_servers() const noexcept;
    [[nodiscard]] const std::string& get_proxy() const noexcept;

    core::Result<HttpResponse> get(
        const std::string& url,
        const std::unordered_map<std::string, std::string>& headers = {},
        const std::unordered_map<std::string, std::string>& params = {}
    );

    core::Result<HttpResponse> get_stream(
        const std::string& url,
        DataCallback on_data,
        int64_t byte_start = -1,
        int64_t byte_end = -1,
        const std::unordered_map<std::string, std::string>& headers = {}
    );

    core::Result<HttpResponse> post(
        const std::string& url,
        const std::string& body,
        const std::unordered_map<std::string, std::string>& headers = {}
    );

    static std::string url_encode(const std::string& value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sctui::net
