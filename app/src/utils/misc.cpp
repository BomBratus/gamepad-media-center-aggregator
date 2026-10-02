#include "utils/misc.hpp"
#include <borealis/core/logger.hpp>
#include <borealis/core/i18n.hpp>
#include <fmt/chrono.h>
#include <random>
#include <sstream>
#include <iomanip>
#include <regex>

using namespace brls::literals;  // for _i18n

#ifdef GMCA_LINUX_TEST_BENCH
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif

inline std::string pre0(size_t num, size_t length) {
    std::string str = std::to_string(num);
    if (length <= str.length()) {
        return str;
    }
    return std::string(length - str.length(), '0') + str;
}

std::string misc::sec2Time(int64_t t) {
    size_t hour = t / 3600;
    size_t minute = t / 60 % 60;
    size_t sec = t % 60;
    if (hour == 0) {
        return pre0(minute, 2) + ":" + pre0(sec, 2);
    }
    return pre0(hour, 2) + ":" + pre0(minute, 2) + ":" + pre0(sec, 2);
}

std::string misc::formatSize(uint64_t s) {
    if (s == 0) return "-";
    if (s < (1 << 20)) return fmt::format("{}KB", s / 1024);
    if (s < (1 << 30)) return fmt::format("{:.2f}MB", (s >> 10) / 1024.0f);
    // TB tier: disk capacities (downloads storage header) commonly exceed
    // 1 TB — "1862.65GB" is not readable
    if (s < (1ULL << 40)) return fmt::format("{:.2f}GB", (s >> 20) / 1024.0f);
    return fmt::format("{:.2f}TB", (s >> 30) / 1024.0f);
}

std::string misc::markdownToText(const std::string& md) {
    std::string out;
    std::istringstream stream(md);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF

        // ATX heading: drop the leading #'s, keep the text
        size_t h = line.find_first_not_of('#');
        if (h > 0 && h <= 6 && h < line.size() && line[h] == ' ') line = line.substr(h + 1);

        // list item: normalize "-"/"*"/"+" markers to a bullet
        size_t i = line.find_first_not_of(' ');
        if (i != std::string::npos && (line[i] == '-' || line[i] == '*' || line[i] == '+') &&
            i + 1 < line.size() && line[i + 1] == ' ') {
            line = line.substr(0, i) + "•" + line.substr(i + 1);
        }

        out += line;
        out += '\n';
    }
    // inline: [text](url) -> text, then strip ** __ ` marks
    out = std::regex_replace(out, std::regex(R"(\[([^\]]+)\]\([^)]*\))"), "$1");
    out = std::regex_replace(out, std::regex(R"(\*\*|__|`)"), "");
    return out;
}

std::string misc::randHex(const int len) {
    std::stringstream ss;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    for (int i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << dis(gen);
    }
    return ss.str();
}

std::string misc::hexEncode(const unsigned char* data, size_t len) {
    std::stringstream ss;
    for (size_t i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    }
    return ss.str();
}

void misc::split(const std::string& data, std::vector<std::string>& result, char seq) {
    std::string s;
    std::stringstream ss(data);
    while (std::getline(ss, s, seq)) result.push_back(s);
}

bool misc::sendIPC(const std::string& sock, const std::string& payload) {
#ifdef GMCA_LINUX_TEST_BENCH
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(fd);
        return false;
    }
    if (write(fd, payload.c_str(), payload.size()) == -1)
        brls::Logger::warning("sendIPC `{}` failed: {}", payload, errno);
    close(fd);
    return true;
#else
    (void)sock;
    (void)payload;
    return false;
#endif
}

namespace base64 {

const char kEncodeLookup[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kPadCharacter = '=';

using byte = std::uint8_t;

std::string encode(const std::string& input) {
    std::string encoded;
    encoded.reserve(((input.size() / 3) + (input.size() % 3 > 0)) * 4);

    std::uint32_t temp{};
    auto it = input.begin();

    for (std::size_t i = 0; i < input.size() / 3; ++i) {
        temp = (*it++) << 16;
        temp += (*it++) << 8;
        temp += (*it++);
        encoded.append(1, kEncodeLookup[(temp & 0x00FC0000) >> 18]);
        encoded.append(1, kEncodeLookup[(temp & 0x0003F000) >> 12]);
        encoded.append(1, kEncodeLookup[(temp & 0x00000FC0) >> 6]);
        encoded.append(1, kEncodeLookup[(temp & 0x0000003F)]);
    }

    switch (input.size() % 3) {
    case 1:
        temp = (*it++) << 16;
        encoded.append(1, kEncodeLookup[(temp & 0x00FC0000) >> 18]);
        encoded.append(1, kEncodeLookup[(temp & 0x0003F000) >> 12]);
        encoded.append(2, kPadCharacter);
        break;
    case 2:
        temp = (*it++) << 16;
        temp += (*it++) << 8;
        encoded.append(1, kEncodeLookup[(temp & 0x00FC0000) >> 18]);
        encoded.append(1, kEncodeLookup[(temp & 0x0003F000) >> 12]);
        encoded.append(1, kEncodeLookup[(temp & 0x00000FC0) >> 6]);
        encoded.append(1, kPadCharacter);
        break;
    }

    return encoded;
}

}  // namespace base64