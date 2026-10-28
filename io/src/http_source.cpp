#include "legato/io/http_source.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace legato {
namespace {

std::once_flag g_curl_once;

void init_curl() { curl_global_init(CURL_GLOBAL_DEFAULT); }

std::int64_t us_of(double seconds) {
    if (seconds <= 0) {
        return 0;
    }
    const auto value = static_cast<std::int64_t>(seconds * 1000000.0 + 0.5);
    return value > 0 ? value : 1;
}

struct Download {
    std::vector<std::uint8_t>* body = nullptr;
    int status = 0;
    bool accept_ranges = false;
};

std::size_t write_body(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* download = static_cast<Download*>(userdata);
    const std::size_t bytes = size * nmemb;
    download->body->insert(download->body->end(), reinterpret_cast<std::uint8_t*>(ptr),
                           reinterpret_cast<std::uint8_t*>(ptr) + bytes);
    return bytes;
}

bool header_has(const char* line, std::size_t length, const char* name) {
    const std::size_t name_len = std::strlen(name);
    if (length < name_len) {
        return false;
    }
    for (std::size_t i = 0; i < name_len; ++i) {
        if (std::tolower(static_cast<unsigned char>(line[i])) != name[i]) {
            return false;
        }
    }
    return true;
}

std::size_t write_header(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* download = static_cast<Download*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (header_has(ptr, bytes, "http/")) {
        const char* cursor = static_cast<const char*>(std::memchr(ptr, ' ', bytes));
        if (cursor != nullptr && cursor + 3 < ptr + bytes) {
            download->status = (cursor[1] - '0') * 100 + (cursor[2] - '0') * 10 + (cursor[3] - '0');
        }
    } else if (header_has(ptr, bytes, "accept-ranges:")) {
        const char* cursor = ptr;
        const char* end = ptr + bytes;
        while (cursor + 5 <= end) {
            if (std::tolower(static_cast<unsigned char>(cursor[0])) == 'b' &&
                std::tolower(static_cast<unsigned char>(cursor[1])) == 'y' &&
                std::tolower(static_cast<unsigned char>(cursor[2])) == 't' &&
                std::tolower(static_cast<unsigned char>(cursor[3])) == 'e' &&
                std::tolower(static_cast<unsigned char>(cursor[4])) == 's') {
                download->accept_ranges = true;
                break;
            }
            ++cursor;
        }
    }
    return bytes;
}

}  /* namespace */

HttpSource::HttpSource(const char* url) : url_(url != nullptr ? url : "") {}

HttpSource::~HttpSource() = default;

int HttpSource::open_io() {
    if (url_.empty()) {
        return -1;
    }
    std::call_once(g_curl_once, init_curl);
    CURL* easy = curl_easy_init();
    if (easy == nullptr) {
        return -1;
    }
    Download download;
    download.body = &body_;
    body_.clear();
    curl_easy_setopt(easy, CURLOPT_URL, url_.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_NOPROXY, "localhost,127.0.0.1");
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "legato");
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &download);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, &download);
    const CURLcode rc = curl_easy_perform(easy);
    double dns = 0;
    double connect = 0;
    double tls = 0;
    double first_byte = 0;
    curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME, &dns);
    curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME, &connect);
    curl_easy_getinfo(easy, CURLINFO_APPCONNECT_TIME, &tls);
    curl_easy_getinfo(easy, CURLINFO_STARTTRANSFER_TIME, &first_byte);
    curl_easy_cleanup(easy);
    if (rc != CURLE_OK || (download.status != 200 && download.status != 206) || body_.empty()) {
        body_.clear();
        return -1;
    }
    timing_.dns_us = us_of(dns);
    timing_.connect_us = us_of(connect > dns ? connect - dns : connect);
    timing_.tls_us = tls > connect ? us_of(tls - connect) : 0;
    timing_.first_byte_us = us_of(first_byte);
    have_timing_ = true;
    session_seekable_ = download.accept_ranges;
    pos_ = 0;
    return 0;
}

int HttpSource::read(std::uint8_t* dst, int size) {
    if (dst == nullptr || size <= 0) {
        return -1;
    }
    if (pos_ >= static_cast<std::int64_t>(body_.size())) {
        return 0;
    }
    const std::int64_t left = static_cast<std::int64_t>(body_.size()) - pos_;
    const int got = static_cast<int>(std::min<std::int64_t>(left, size));
    std::memcpy(dst, body_.data() + pos_, static_cast<std::size_t>(got));
    pos_ += got;
    return got;
}

std::int64_t HttpSource::seek(std::int64_t offset, int whence) {
    std::int64_t next = pos_;
    if (whence == SEEK_SET) {
        next = offset;
    } else if (whence == SEEK_CUR) {
        next = pos_ + offset;
    } else if (whence == SEEK_END) {
        next = static_cast<std::int64_t>(body_.size()) + offset;
    } else {
        return -1;
    }
    if (next < 0 || next > static_cast<std::int64_t>(body_.size())) {
        return -1;
    }
    pos_ = next;
    return pos_;
}

}  /* namespace legato */
