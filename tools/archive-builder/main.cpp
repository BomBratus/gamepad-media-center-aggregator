#include "api/stremio/imdb_index.hpp"
#include <sqlite3.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <array>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }
std::string sha256(const std::string& path) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Cannot initialize SHA256");
    std::ifstream file(path, std::ios::binary);
    std::array<char, 64 * 1024> buffer;
    while (file) {
        file.read(buffer.data(), buffer.size());
        if (EVP_DigestUpdate(digest.get(), buffer.data(), file.gcount()) != 1)
            throw std::runtime_error("Cannot update SHA256");
    }
    if (!file.eof()) throw std::runtime_error("Cannot read output index");
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash;
    unsigned size;
    if (EVP_DigestFinal_ex(digest.get(), hash.data(), &size) != 1)
        throw std::runtime_error("Cannot finish SHA256");
    std::ostringstream result;
    for (unsigned i = 0; i < size; ++i) result << std::hex << std::setw(2) << std::setfill('0') << unsigned(hash[i]);
    return result.str();
}
void validate(const std::string& path) {
    sqlite3* raw = nullptr;
    const auto code = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    if (code != SQLITE_OK) throw std::runtime_error("Cannot open completed index");
    auto check = [&](const char* sql, const std::string& expected) {
        sqlite3_stmt* rawStatement = nullptr;
        const auto prepared = sqlite3_prepare_v2(db.get(), sql, -1, &rawStatement, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(rawStatement, sqlite3_finalize);
        if (prepared != SQLITE_OK || sqlite3_step(statement.get()) != SQLITE_ROW)
            throw std::runtime_error("Invalid Archive schema");
        const auto text = sqlite3_column_text(statement.get(), 0);
        if (!text || reinterpret_cast<const char*>(text) != expected)
            throw std::runtime_error("Archive validation failed");
    };
    check("SELECT value FROM settings WHERE key='version'", "2");
    check("SELECT value FROM settings WHERE key='complete'", "1");
    check("PRAGMA quick_check", "ok");
}
}
int main(int argc, char** argv) {
    using namespace stremio::archive;
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "gmca-archive-builder OUTPUT.sqlite DATASET_DIRECTORY\n"
                "gmca-archive-builder --validate INDEX.sqlite\n"
                "Uses your local title.ratings.tsv.gz/title.basics.tsv.gz/title.akas.tsv.gz; no distribution or upload.\n";
            return 0;
        }
        if (argc != 3) throw std::invalid_argument("Use --help for local dataset build/validation syntax");
        const bool validationOnly = std::string(argv[1]) == "--validate";
        const auto output = std::filesystem::absolute(validationOnly ? argv[2] : argv[1]).string();
        auto cancel = std::make_shared<std::atomic_bool>(false);
        auto checkpoint = [&] { if (interrupted) cancel->store(true); };
        std::signal(SIGINT, interrupt); std::signal(SIGTERM, interrupt);
        if (!validationOnly) {
            const auto datasets = std::filesystem::absolute(argv[2]);
            std::filesystem::create_directories(std::filesystem::path(output).parent_path());
            const bool built = buildImdbIndex(output, cancel,
                [&](const std::string& name, const std::string& target, const IndexCancel&) {
                    std::ifstream source(datasets / name, std::ios::binary);
                    if (!source) throw std::runtime_error("Missing local dataset " + name);
                    std::ofstream copy(target + ".download", std::ios::binary);
                    std::array<char, 64 * 1024> buffer;
                    while (source && !cancel->load()) {
                        checkpoint(); source.read(buffer.data(), buffer.size());
                        copy.write(buffer.data(), source.gcount());
                    }
                    copy.close();
                    if (cancel->load() || !source.eof() || !copy)
                        throw std::runtime_error("Dataset copy interrupted or failed");
                    std::filesystem::rename(target + ".download", target);
                }, {}, checkpoint, [](const std::string& phase, double ms) {
                    std::cerr << "build-phase name=" << phase << " ms=" << ms << '\n';
                });
            if (!built) return 130;
        }
        validate(output);
        ImdbIndex index(output);
        const auto result = index.query({}, {}, 0, false, cancel);
        std::cout << nlohmann::json{{"schema", 2}, {"titles", result.indexed},
            {"browsable", result.total}, {"refreshed", result.refreshed}, {"path", output},
            {"bytes", std::filesystem::file_size(output)}, {"sha256", sha256(output)}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "archive-builder: " << error.what() << '\n'; return interrupted ? 130 : 1;
    }
}
