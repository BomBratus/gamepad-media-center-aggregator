#ifdef GMCA_TEST_HARNESS
#include "harness.hpp"
#endif
#include "api/http.hpp"
#include "utils/config.hpp"
#include <borealis/core/logger.hpp>
#include <curl/curl.h>
#include <mutex>

#ifndef CURL_PROGRESSFUNC_CONTINUE
#define CURL_PROGRESSFUNC_CONTINUE 0x10000001
#endif

class curl_error : public std::exception {
public:
    explicit curl_error(CURLcode code) : m(curl_easy_strerror(code)) {}
    explicit curl_error(const std::string& arg) : m(arg) {}
    const char* what() const noexcept override { return m.c_str(); }

private:
    std::string m;
};

static std::string user_agent =
    fmt::format("{}/{} ({})", AppVersion::getPackageName(), AppVersion::getVersion(), AppVersion::getPlatform());

/// @brief Verrous du cache partagé libcurl (CURLSH), indexés par curl_lock_data.
/// libcurl exige des callbacks lock/unlock dès qu'un objet partagé (ici le cache DNS)
/// est utilisé par des easy handles répartis sur plusieurs threads. Sans eux, les accès
/// concurrents corrompent la hash table du cache DNS (SIGABRT dans Curl_hash_delete /
/// Curl_resolv). Un mutex exclusif par type de donnée suffit : la section critique se
/// limite à la lecture/écriture du cache, les requêtes restent parallèles. Le garde
/// `data < CURL_LOCK_DATA_LAST` protège contre un index hors bornes.
static std::mutex share_locks[CURL_LOCK_DATA_LAST];

static void curl_share_lock_cb(CURL* /*handle*/, curl_lock_data data, curl_lock_access /*access*/,
                               void* /*userptr*/) {
    if (data < CURL_LOCK_DATA_LAST) share_locks[data].lock();
}

static void curl_share_unlock_cb(CURL* /*handle*/, curl_lock_data data, void* /*userptr*/) {
    if (data < CURL_LOCK_DATA_LAST) share_locks[data].unlock();
}

/// @brief curl context

HTTP::HTTP() : chunk(nullptr) {
    static struct Global {
        Global() {
            CURLcode rc = curl_global_init(CURL_GLOBAL_ALL);
            brls::Logger::debug("curl global init {}", std::to_string(rc));
            this->share = curl_share_init();
            // Callbacks de verrouillage obligatoires pour un partage inter-threads sûr
            curl_share_setopt(share, CURLSHOPT_LOCKFUNC, curl_share_lock_cb);
            curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, curl_share_unlock_cb);
            curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        }
        ~Global() {
            curl_share_cleanup(this->share);
            curl_global_cleanup();
            brls::Logger::debug("curl cleanup");
        }
        // Avoids initalization order problems
        std::mutex init_lock;
        CURLSH* share;
    } global;

    global.init_lock.lock();
    this->easy = curl_easy_init();
    global.init_lock.unlock();

    curl_easy_setopt(this->easy, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(this->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(this->easy, CURLOPT_SHARE, global.share);
    // enable all supported built-in compressions
    curl_easy_setopt(this->easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(this->easy, CURLOPT_VERBOSE, 0L);
    curl_easy_setopt(this->easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(this->easy, CURLOPT_SSL_VERIFYHOST, 0L);
    // Every request runs from a background thread (ThreadPool / brls::async), so
    // libcurl must never reach for SIGALRM to time out a name resolve — signals
    // only work on the main thread and are unsafe multi-threaded (curl docs:
    // "libcurl cannot function properly multi-threaded unless CURLOPT_NOSIGNAL
    // is set"). With NOSIGNAL, DNS timeouts rely solely on the async (threaded)
    curl_easy_setopt(this->easy, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x071900 && !defined(__PS4__)
    curl_easy_setopt(this->easy, CURLOPT_TCP_KEEPALIVE, 1L);
#endif
}

HTTP::~HTTP() {
    if (this->chunk != nullptr) curl_slist_free_all(this->chunk);
    if (this->easy != nullptr) curl_easy_cleanup(this->easy);
}

void HTTP::set_user_agent(const std::string& agent) { curl_easy_setopt(this->easy, CURLOPT_USERAGENT, agent.c_str()); }

void HTTP::add_header(const std::string& header) { this->chunk = curl_slist_append(this->chunk, header.c_str()); }

void HTTP::set_option(const Header& hs) {
    curl_slist* chunk = nullptr;
    for (auto& h : hs) {
        chunk = curl_slist_append(chunk, h.c_str());
    }
    curl_slist_free_all(this->chunk);
    this->chunk = chunk;
    if (chunk != nullptr) curl_easy_setopt(this->easy, CURLOPT_HTTPHEADER, chunk);
}

void HTTP::set_option(const Range& r) {
    const std::string range_str = std::to_string(r.start) + "-" + std::to_string(r.end);
    curl_easy_setopt(this->easy, CURLOPT_RANGE, range_str.c_str());
}

void HTTP::set_option(const Timeout& t) {
    curl_easy_setopt(this->easy, CURLOPT_TIMEOUT_MS, t.timeout);
    curl_easy_setopt(this->easy, CURLOPT_CONNECTTIMEOUT_MS, t.connect > 0 ? t.connect : t.timeout);
}

int HTTP::easy_progress_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    HTTP* ctx = reinterpret_cast<HTTP*>(clientp);
    ctx->event.fire(dltotal, dlnow);
    return ctx->is_cancel->load() ? 1 : CURL_PROGRESSFUNC_CONTINUE;
}

void HTTP::set_option(const Cancel& c) {
    this->is_cancel = std::move(c);
    curl_easy_setopt(this->easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(this->easy, CURLOPT_XFERINFOFUNCTION, easy_progress_cb);
    curl_easy_setopt(this->easy, CURLOPT_XFERINFODATA, this);
}

void HTTP::set_option(Progress::Callback p) {
    this->event.subscribe(p);
    curl_easy_setopt(this->easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(this->easy, CURLOPT_XFERINFOFUNCTION, easy_progress_cb);
    curl_easy_setopt(this->easy, CURLOPT_XFERINFODATA, this);
}

void HTTP::set_option(const Cookies& cookies) {
    std::stringstream ss;
    char* escaped;
    for (auto& c : cookies) {
        ss << c.name << "=";
        escaped = curl_easy_escape(this->easy, c.value.c_str(), c.value.size());
        if (escaped) {
            ss << escaped;
            curl_free(escaped);
        }
        ss << "; ";
    }
    curl_easy_setopt(this->easy, CURLOPT_COOKIE, ss.str().c_str());
}

size_t HTTP::easy_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::ostream* ctx = reinterpret_cast<std::ostream*>(userdata);
    size_t count = size * nmemb;
    ctx->write(ptr, count);
    return ctx->good() ? count : 0;
}

int HTTP::perform(std::ostream* body) {
    curl_easy_setopt(this->easy, CURLOPT_WRITEFUNCTION, easy_write_cb);
    curl_easy_setopt(this->easy, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(this->easy, CURLOPT_PROXY, PROXY_STATUS ? PROXY.c_str() : nullptr);

    CURLcode res = curl_easy_perform(this->easy);
    if (res != CURLE_OK) throw curl_error(res);

    long status_code = 0;
    curl_easy_getinfo(this->easy, CURLINFO_RESPONSE_CODE, &status_code);
    return status_code;
}

std::string HTTP::encode_form(const Form& form) {
    std::ostringstream ss;
    char* escaped;
    for (auto it = form.begin(); it != form.end(); ++it) {
        if (it->second.empty()) continue;
        if (it != form.begin()) ss << '&';
        escaped = curl_escape(it->second.c_str(), it->second.size());
        ss << it->first << '=' << escaped;
        curl_free(escaped);
    }
    return ss.str();
}

void HTTP::_get(const std::string& url, std::ostream* out) {
#ifdef GMCA_TEST_HARNESS
    const auto target = gmca::test::requestUrl(url, false);
#else
    const auto& target = url;
#endif
    curl_easy_setopt(this->easy, CURLOPT_URL, target.c_str());
    curl_easy_setopt(this->easy, CURLOPT_HTTPGET, 1L);
    int code = this->perform(out);
    if (code >= 400) throw curl_error(fmt::format("http status {}", code));
}

bool HTTP::getinfo(char** arg) { return curl_easy_getinfo(this->easy, CURLINFO_CONTENT_TYPE, arg) == CURLE_OK; }

std::string HTTP::_post(const std::string& url, const std::string& data) {
#ifdef GMCA_TEST_HARNESS
    const auto target = gmca::test::requestUrl(url, true);
#else
    const auto& target = url;
#endif
    std::ostringstream body;
    curl_easy_setopt(this->easy, CURLOPT_URL, target.c_str());
    curl_easy_setopt(this->easy, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(this->easy, CURLOPT_POSTFIELDSIZE, data.size());
    int code = this->perform(&body);
    if (code >= 400) throw curl_error(fmt::format("http status {}", code));
    return body.str();
}

std::string HTTP::_put(const std::string& url, const std::string& data) {
#ifdef GMCA_TEST_HARNESS
    const auto target = gmca::test::requestUrl(url, true);
#else
    const auto& target = url;
#endif
    std::ostringstream body;
    curl_easy_setopt(this->easy, CURLOPT_URL, target.c_str());
    curl_easy_setopt(this->easy, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(this->easy, CURLOPT_POSTFIELDSIZE, data.size());
    curl_easy_setopt(this->easy, CURLOPT_CUSTOMREQUEST, "PUT");
    int code = this->perform(&body);
    if (code >= 400) throw curl_error(fmt::format("http status {}", code));
    return body.str();
}

void HTTP::_delete(const std::string& url, std::ostream* out) {
#ifdef GMCA_TEST_HARNESS
    const auto target = gmca::test::requestUrl(url, true);
#else
    const auto& target = url;
#endif
    curl_easy_setopt(this->easy, CURLOPT_URL, target.c_str());
    curl_easy_setopt(this->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
    int code = this->perform(out);
    if (code >= 400) throw curl_error(fmt::format("http status {}", code));
}
