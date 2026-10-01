// Standalone tests for ordered asynchronous Stremio playback checkpoints.

#include <api/stremio/async_playback_history.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond)                                            \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                        \
        }                                                      \
    } while (0)

static media::Item makeItem(const std::string& id, const std::string& title) {
    media::Item item;
    item.ratingKey = id;
    item.type = media::mediaTypeEpisode;
    item.title = title;
    return item;
}

static media::Media makeSource(const std::string& url, const std::string& identity) {
    media::Media source;
    source.sourceIdentity = identity;
    media::Part part;
    part.key = url;
    source.parts.push_back(part);
    return source;
}

int main() {
    char directoryTemplate[] = "/tmp/gmca-async-playback-history-XXXXXX";
    char* directoryName = ::mkdtemp(directoryTemplate);
    CHECK(directoryName != nullptr);
    if (!directoryName) return 1;
    const std::filesystem::path directory(directoryName);
    const std::string path = (directory / "history.json").string();
    const auto source = makeSource("https://cdn.example/video", "addon/release/file");

    // FIFO order retains a backward seek. A clear drains earlier saves before
    // marking the entry complete, and a later save can resume it again.
    {
        stremio::AsyncPlaybackHistory history(path);
        history.save("account-a", makeItem("episode-1", "Episode 1"), source, 90000, 180000);
        history.save("account-a", makeItem("episode-1", "Episode 1"), source, 25000, 180000);
        history.save("account-a", makeItem("episode-2", "Episode 2"), source, 0, 190000);
        history.save("account-b", makeItem("episode-1", "Other account"), source, 7000, 180000);

        CHECK(history.load("account-a", "episode-1").value("position", int64_t{-1}) == 25000);
        CHECK(history.records("account-a").size() == 2);
        CHECK(history.load("account-b", "episode-1").value("position", int64_t{-1}) == 7000);

        history.save("account-a", makeItem("episode-1", "Episode 1"), source, 64000, 180000);
        CHECK(history.clear("account-a", "episode-1"));
        CHECK(history.load("account-a", "episode-1").value("position", int64_t{-1}) == 0);
        history.save("account-a", makeItem("episode-1", "Episode 1"), source, 5100, 180000);
        CHECK(history.load("account-a", "episode-1").value("position", int64_t{-1}) == 5100);
    }

    // Destruction drains the queue, including a final zero-position stop
    // snapshot, before the underlying store is destroyed.
    {
        stremio::AsyncPlaybackHistory history(path);
        history.save("account-a", makeItem("episode-stop", "Stop"), source, 25000, 100000);
        history.save("account-a", makeItem("episode-stop", "Stop"), source, 0, 100000);
    }
    // Explicit flush makes the final snapshot durable before process replacement.
    {
        stremio::AsyncPlaybackHistory history(path);
        history.save("account-a", makeItem("restart", "Restart"), source, 12345, 100000);
        history.flush();
        stremio::PlaybackHistory disk(path);
        CHECK(disk.load("account-a", "restart").value("position", int64_t{-1}) == 12345);
    }

    stremio::PlaybackHistory restarted(path);
    CHECK(restarted.load("account-a", "episode-stop").value("position", int64_t{-1}) == 0);

    // The worker can be held inside readRoot by a named pipe. Saves still
    // return before the pipe is opened and while the worker is blocked reading.
    const std::string fifoPath = (directory / "blocked.json").string();
    const bool fifoCreated = ::mkfifo(fifoPath.c_str(), 0600) == 0;
    CHECK(fifoCreated);
    if (fifoCreated) {
        stremio::AsyncPlaybackHistory history(fifoPath);
        media::Item captured = makeItem("fifo-item", "Captured title");
        auto firstSave = std::async(std::launch::async, [&] {
            history.save("fifo-account", captured, source, 10000, 20000);
        });
        const bool returnedBeforeDisk = firstSave.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        CHECK(returnedBeforeDisk);
        if (returnedBeforeDisk) captured.title = "Mutated after enqueue";

        std::mutex gateMutex;
        std::condition_variable gateChanged;
        bool allowPipeData = false;
        std::promise<bool> writerConnectedPromise;
        auto writerConnected = writerConnectedPromise.get_future();
        std::thread writer([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            int fd = -1;
            while (std::chrono::steady_clock::now() < deadline) {
                fd = ::open(fifoPath.c_str(), O_WRONLY | O_NONBLOCK);
                if (fd >= 0 || errno != ENXIO) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            writerConnectedPromise.set_value(fd >= 0);
            if (fd < 0) return;
            {
                std::unique_lock<std::mutex> lock(gateMutex);
                gateChanged.wait(lock, [&] { return allowPipeData; });
            }
            const char root[] = "{}";
            (void)::write(fd, root, sizeof(root) - 1);
            ::close(fd);
        });

        const bool readerConnected = writerConnected.wait_for(std::chrono::seconds(3)) == std::future_status::ready &&
            writerConnected.get();
        CHECK(readerConnected);
        auto secondSave = std::async(std::launch::async, [&] {
            history.save("fifo-account", makeItem("later-item", "Later title"), source, 5000, 20000);
        });
        const bool returnedWhileWorkerBlocked = secondSave.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        CHECK(returnedWhileWorkerBlocked);

        {
            std::lock_guard<std::mutex> lock(gateMutex);
            allowPipeData = true;
        }
        gateChanged.notify_one();
        writer.join();
        firstSave.get();
        secondSave.get();
        const auto capturedRecord = history.load("fifo-account", "fifo-item");
        CHECK(capturedRecord.value("item", nlohmann::json::object()).value("title", std::string()) == "Captured title");
        const auto saved = history.load("fifo-account", "later-item");
        CHECK(saved.value("position", int64_t{-1}) == 5000);
        CHECK(saved.value("item", nlohmann::json::object()).value("title", std::string()) == "Later title");
    }

    // A failing location is reported on the worker. Even a throwing callback
    // cannot terminate the thread or strand a synchronous barrier.
    const std::string failingPath = (directory / "missing" / "history.json").string();
    std::atomic<int> reportedFailures{0};
    {
        stremio::AsyncPlaybackHistory history(failingPath, [&] {
            ++reportedFailures;
            throw std::runtime_error("logger failure");
        });
        history.save("account-a", makeItem("failure", "Failure"), source, 1, 2);
        CHECK(history.records("account-a").empty());
    }
    CHECK(reportedFailures.load() > 0);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures == 0) {
        printf("test_async_playback_history: OK\n");
        return 0;
    }
    printf("test_async_playback_history: %d FAILURE(S)\n", failures);
    return 1;
}
