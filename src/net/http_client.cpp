#include "sctui/net/http_client.hpp"
#include <curl/curl.h>
#include <sstream>
#include <iostream>

namespace sctui::net {

struct HttpClient::Impl {
    std::string user_agent{"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"};
    long timeout_seconds{20};
    std::string bearer_token;
    std::string doh_url;
    std::string dns_servers;
    std::string proxy;

    static size_t write_string_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* str = static_cast<std::string*>(userdata);
        size_t total = size * nmemb;
        str->append(ptr, total);
        return total;
    }

    static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
        auto* headers = static_cast<std::unordered_map<std::string, std::string>*>(userdata);
        size_t total = size * nitems;
        std::string line(buffer, total);
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.pop_back();
            (*headers)[key] = val;
        }
        return total;
    }

    struct StreamContext {
        DataCallback callback;
        bool aborted{false};
    };

    static size_t stream_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* ctx = static_cast<StreamContext*>(userdata);
        size_t total = size * nmemb;
        if (ctx && ctx->callback) {
            bool keep_going = ctx->callback(ptr, total);
            if (!keep_going) {
                ctx->aborted = true;
                return 0; // Abort curl transfer
            }
        }
        return total;
    }
};

HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {
    static bool curl_initialized = false;
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_ALL);
        curl_initialized = true;
    }
}

HttpClient::~HttpClient() = default;
HttpClient::HttpClient(HttpClient&&) noexcept = default;
HttpClient& HttpClient::operator=(HttpClient&&) noexcept = default;

void HttpClient::set_user_agent(std::string user_agent) {
    impl_->user_agent = std::move(user_agent);
}

void HttpClient::set_timeout_seconds(long seconds) {
    impl_->timeout_seconds = seconds;
}

void HttpClient::set_bearer_token(std::string token) {
    impl_->bearer_token = std::move(token);
}

void HttpClient::set_doh_url(std::string doh_url) {
    impl_->doh_url = std::move(doh_url);
}

void HttpClient::set_dns_servers(std::string dns_servers) {
    impl_->dns_servers = std::move(dns_servers);
}

void HttpClient::set_proxy(std::string proxy) {
    impl_->proxy = std::move(proxy);
}

const std::string& HttpClient::get_doh_url() const noexcept {
    return impl_->doh_url;
}

const std::string& HttpClient::get_dns_servers() const noexcept {
    return impl_->dns_servers;
}

const std::string& HttpClient::get_proxy() const noexcept {
    return impl_->proxy;
}

std::string HttpClient::url_encode(const std::string& value) {
    CURL* curl = curl_easy_init();
    if (!curl) return value;
    char* output = curl_easy_escape(curl, value.c_str(), static_cast<int>(value.length()));
    std::string result;
    if (output) {
        result = output;
        curl_free(output);
    }
    curl_easy_cleanup(curl);
    return result;
}

core::Result<HttpResponse> HttpClient::get(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers,
    const std::unordered_map<std::string, std::string>& params
) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return core::MakeError(core::ErrorCode::NetworkError, "Failed to initialize libcurl");
    }

    std::string final_url = url;
    if (!params.empty()) {
        std::ostringstream qs;
        bool first = (url.find('?') == std::string::npos);
        for (const auto& [k, v] : params) {
            qs << (first ? '?' : '&') << url_encode(k) << '=' << url_encode(v);
            first = false;
        }
        final_url += qs.str();
    }

    HttpResponse response;
    struct curl_slist* chunk = nullptr;

    for (const auto& [k, v] : headers) {
        std::string header_line = k + ": " + v;
        chunk = curl_slist_append(chunk, header_line.c_str());
    }

    if (!impl_->bearer_token.empty() && headers.find("Authorization") == headers.end()) {
        std::string auth_header = "Authorization: OAuth " + impl_->bearer_token;
        chunk = curl_slist_append(chunk, auth_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, final_url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, impl_->user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, impl_->timeout_seconds);

    if (!impl_->doh_url.empty()) {
        curl_easy_setopt(curl, CURLOPT_DOH_URL, impl_->doh_url.c_str());
    }
    if (!impl_->dns_servers.empty()) {
        curl_easy_setopt(curl, CURLOPT_DNS_SERVERS, impl_->dns_servers.c_str());
    }
    if (!impl_->proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, impl_->proxy.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    if (!impl_->doh_url.empty()) {
        curl_easy_setopt(curl, CURLOPT_DOH_URL, impl_->doh_url.c_str());
    }
    if (!impl_->dns_servers.empty()) {
        curl_easy_setopt(curl, CURLOPT_DNS_SERVERS, impl_->dns_servers.c_str());
    }
    if (!impl_->proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, impl_->proxy.c_str());
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (!impl_->doh_url.empty()) {
        curl_easy_setopt(curl, CURLOPT_DOH_URL, impl_->doh_url.c_str());
    }
    if (!impl_->dns_servers.empty()) {
        curl_easy_setopt(curl, CURLOPT_DNS_SERVERS, impl_->dns_servers.c_str());
    }
    if (!impl_->proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, impl_->proxy.c_str());
    }

    if (chunk) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Impl::write_string_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, Impl::header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::string err_msg = curl_easy_strerror(res);
        if (chunk) curl_slist_free_all(chunk);
        curl_easy_cleanup(curl);
        return core::MakeError(core::ErrorCode::NetworkError, "CURL request failed: " + err_msg);
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    response.status_code = static_cast<int>(http_code);

    if (chunk) curl_slist_free_all(chunk);
    curl_easy_cleanup(curl);

    if (response.status_code == 429) {
        return core::MakeError(core::ErrorCode::RateLimited, "SoundCloud API rate limit exceeded", 429);
    }
    if (response.status_code >= 400) {
        return core::MakeError(core::ErrorCode::HttpError, "HTTP error: " + std::to_string(response.status_code), response.status_code);
    }

    return response;
}

core::Result<HttpResponse> HttpClient::get_stream(
    const std::string& url,
    DataCallback on_data,
    int64_t byte_start,
    int64_t byte_end,
    const std::unordered_map<std::string, std::string>& headers
) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return core::MakeError(core::ErrorCode::NetworkError, "Failed to initialize libcurl for streaming");
    }

    HttpResponse response;
    struct curl_slist* chunk = nullptr;

    for (const auto& [k, v] : headers) {
        std::string header_line = k + ": " + v;
        chunk = curl_slist_append(chunk, header_line.c_str());
    }

    if (byte_start >= 0) {
        std::string range_hdr = "bytes=" + std::to_string(byte_start) + "-";
        if (byte_end >= byte_start) {
            range_hdr += std::to_string(byte_end);
        }
        curl_easy_setopt(curl, CURLOPT_RANGE, range_hdr.c_str());
    }

    Impl::StreamContext ctx{std::move(on_data), false};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, impl_->user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L); // Streaming could take entire song length
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

    if (!impl_->doh_url.empty()) {
        curl_easy_setopt(curl, CURLOPT_DOH_URL, impl_->doh_url.c_str());
    }
    if (!impl_->dns_servers.empty()) {
        curl_easy_setopt(curl, CURLOPT_DNS_SERVERS, impl_->dns_servers.c_str());
    }
    if (!impl_->proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, impl_->proxy.c_str());
    }

    if (chunk) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Impl::stream_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, Impl::header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    response.status_code = static_cast<int>(http_code);

    if (chunk) curl_slist_free_all(chunk);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK && !ctx.aborted) {
        return core::MakeError(core::ErrorCode::NetworkError, "Stream transfer error: " + std::string(curl_easy_strerror(res)));
    }

    return response;
}

core::Result<HttpResponse> HttpClient::post(
    const std::string& url,
    const std::string& body,
    const std::unordered_map<std::string, std::string>& headers
) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return core::MakeError(core::ErrorCode::NetworkError, "Failed to initialize libcurl");
    }

    HttpResponse response;
    struct curl_slist* chunk = nullptr;

    for (const auto& [k, v] : headers) {
        std::string header_line = k + ": " + v;
        chunk = curl_slist_append(chunk, header_line.c_str());
    }

    if (!impl_->bearer_token.empty() && headers.find("Authorization") == headers.end()) {
        std::string auth_header = "Authorization: OAuth " + impl_->bearer_token;
        chunk = curl_slist_append(chunk, auth_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.length()));
    curl_easy_setopt(curl, CURLOPT_USERAGENT, impl_->user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, impl_->timeout_seconds);

    if (!impl_->doh_url.empty()) {
        curl_easy_setopt(curl, CURLOPT_DOH_URL, impl_->doh_url.c_str());
    }
    if (!impl_->dns_servers.empty()) {
        curl_easy_setopt(curl, CURLOPT_DNS_SERVERS, impl_->dns_servers.c_str());
    }
    if (!impl_->proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, impl_->proxy.c_str());
    }

    if (chunk) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Impl::write_string_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::string err_msg = curl_easy_strerror(res);
        if (chunk) curl_slist_free_all(chunk);
        curl_easy_cleanup(curl);
        return core::MakeError(core::ErrorCode::NetworkError, "POST request failed: " + err_msg);
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    response.status_code = static_cast<int>(http_code);

    if (chunk) curl_slist_free_all(chunk);
    curl_easy_cleanup(curl);

    return response;
}

} // namespace sctui::net
