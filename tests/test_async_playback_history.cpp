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
#include <memory>
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

class WriterGate {
public:
    WriterGate() : releaseFuture_(release_.get_future().share()), startedFuture_(started_.get_future()) {}

    void block(stremio::AsyncPlaybackHistory& history) {
        history.enqueue([this] {
            started_.set_value();
            releaseFuture_.wait();
        });
        startedFuture_.wait();
    }

    void release() { release_.set_value(); }

private:
    std::promise<void> started_;
    std::promise<void> release_;
    std::shared_future<void> releaseFuture_;
    std::future<void> startedFuture_;
};

int main() {
    char directoryTemplate[] = "/tmp/gmca-async-playback-history-XXXXXX";
    char* directoryName = ::mkdtemp(directoryTemplate);
    CHECK(directoryName != nullptr);
    if (!directoryName) return 1;
    const std::filesystem::path directory(directoryName);
    const std::string path = (directory / "history.json").string();
    const auto source = makeSource("https://cdn.example/video", "addon/release/file");

    // Hold the writer while a burst queues. Adjacent snapshots for one key
    // collapse to the last complete state, while scope and episode keys stay
    // independent. The final flush makes the inspection deterministic.
    {
        std::atomic<int> reportedFailures{0};
        stremio::AsyncPlaybackHistory history(path, [&] { ++reportedFailures; });
        WriterGate gate;
        gate.block(history);

        const auto firstSource = makeSource("https://cdn.example/old", "addon/old-source");
        const auto finalSource = makeSource("https://cdn.example/new", "addon/new-source");
        for (int i = 0; i < 256; ++i) {
            history.save("coalesce-a", makeItem("coalesced", "Queued " + std::to_string(i)),
                firstSource, 100000 - i, 180000);
        }
        // A backward seek and changed source must survive as one last snapshot.
        history.save("coalesce-a", makeItem("coalesced", "After seek"), finalSource, 25000, 180000);
        history.save("coalesce-a", makeItem("backward", "Backward seek"), firstSource, 90000, 180000);
        history.save("coalesce-a", makeItem("backward", "Backward seek"), finalSource, 25000, 180000);
        history.save("coalesce-a", makeItem("stopped", "Stopped"), source, 45000, 90000);
        history.save("coalesce-a", makeItem("stopped", "Stopped"), finalSource, 0, 90000);
        history.save("coalesce-b", makeItem("coalesced", "Other scope"), source, 7000, 180000);
        history.save("coalesce-a", makeItem("next-episode", "Next episode"), source, 8000, 100000);
        // Returning to the earlier episode after another key change is a new
        // ordered save whose zero position and source identity must win.
        history.save("coalesce-a", makeItem("coalesced", "Final"), finalSource, 0, 180000);

        gate.release();
        history.flush();

        stremio::PlaybackHistory disk(path);
        const auto seek = disk.load("coalesce-a", "coalesced");
        CHECK(seek.value("position", int64_t{-1}) == 0);
        CHECK(seek.value("item", nlohmann::json::object()).value("title", std::string()) == "Final");
        CHECK(seek.value("source", nlohmann::json::object()).value("url", std::string()) == "https://cdn.example/new");
        CHECK(seek.value("source", nlohmann::json::object()).value("identity", std::string()) == "addon/new-source");
        CHECK(disk.load("coalesce-a", "backward").value("position", int64_t{-1}) == 25000);
        CHECK(disk.load("coalesce-a", "backward").value("source", nlohmann::json::object())
                  .value("identity", std::string()) == "addon/new-source");
        CHECK(disk.load("coalesce-a", "stopped").value("position", int64_t{-1}) == 0);
        CHECK(disk.load("coalesce-a", "next-episode").value("position", int64_t{-1}) == 8000);
        CHECK(disk.load("coalesce-b", "coalesced").value("position", int64_t{-1}) == 7000);
        CHECK(reportedFailures.load() == 0);
    }

    // A missing parent directory makes each attempted write observable through
    // the existing failure callback. The blocked worker keeps all submissions
    // in the queue, so the expected count proves coalescing and its fences.
    {
        const std::string missingPath = (directory / "coalescing-missing" / "history.json").string();
        std::atomic<int> reportedFailures{0};
        stremio::AsyncPlaybackHistory history(missingPath, [&] { ++reportedFailures; });
        WriterGate gate;
        gate.block(history);

        for (int i = 0; i < 256; ++i)
            history.save("coalesce-a", makeItem("episode-1", "Burst"), source, i, 1000);
        history.clearAsync("coalesce-a", "episode-1");
        history.save("coalesce-a", makeItem("episode-1", "After clear"), source, 200, 1000);
        history.enqueue([] {});
        history.save("coalesce-a", makeItem("episode-1", "After callback"), source, 300, 1000);
        history.save("coalesce-b", makeItem("episode-1", "Other scope"), source, 400, 1000);
        history.save("coalesce-b", makeItem("episode-2", "Other episode"), source, 500, 1000);

        gate.release();
        history.flush();
        CHECK(reportedFailures.load() == 5);
    }

    // Callback jobs share FIFO ordering with saves and fence coalescing. The
    // callback observes the predecessor before the later snapshot runs.
    {
        stremio::AsyncPlaybackHistory history(path);
        std::promise<int64_t> observedPromise;
        auto observed = observedPromise.get_future();
        history.save("callback-order", makeItem("item", "Before"), source, 100, 1000);
        history.enqueue([&] {
            stremio::PlaybackHistory disk(path);
            observedPromise.set_value(disk.load("callback-order", "item").value("position", int64_t{-1}));
        });
        history.save("callback-order", makeItem("item", "After"), source, 200, 1000);
        history.flush();
        CHECK(observed.get() == 100);
        CHECK(history.load("callback-order", "item").value("position", int64_t{-1}) == 200);
    }

    // The in-memory view follows queued saves and async clears immediately.
    // Disk inspection between jobs proves clearAsync stays ordered on the
    // writer, while the held worker proves cachedLoad does not await it.
    {
        stremio::PlaybackHistory seed(path);
        CHECK(seed.save("unseen-cache", makeItem("item", "Disk only"), source, 500, 1000));

        stremio::AsyncPlaybackHistory history(path);
        CHECK(history.cachedLoad("unseen-cache", "item").empty());
        CHECK(history.records("unseen-cache").size() == 1);
        CHECK(history.cachedLoad("unseen-cache", "item").value("position", int64_t{-1}) == 500);
        WriterGate gate;
        gate.block(history);

        const auto previousSource = makeSource("https://cdn.example/cache-old", "addon/cache-old");
        const auto currentSource = makeSource("https://cdn.example/cache-new", "addon/cache-new");
        history.save("cached", makeItem("item", "Before seek"), previousSource, 90000, 100000);
        history.save("cached", makeItem("item", "Backward seek"), currentSource, 25000, 100000);

        // This direct lookup succeeds while the only worker is known to be
        // held above, and its key exists only in memory at this point.
        const auto cachedBeforeClear = history.cachedLoad("cached", "item");
        CHECK(cachedBeforeClear.value("position", int64_t{-1}) == 25000);
        CHECK(cachedBeforeClear.value("source", nlohmann::json::object())
                  .value("identity", std::string()) == "addon/cache-new");

        history.clearAsync("cached", "item");
        const auto cachedCleared = history.cachedLoad("cached", "item");
        CHECK(cachedCleared.value("position", int64_t{-1}) == 0);
        CHECK(cachedCleared.value("source", nlohmann::json::object())
                  .value("identity", std::string()) == "addon/cache-new");

        std::promise<int64_t> observedPromise;
        auto observed = observedPromise.get_future();
        history.enqueue([&] {
            stremio::PlaybackHistory disk(path);
            observedPromise.set_value(disk.load("cached", "item").value("position", int64_t{-1}));
        });
        const auto afterClearSource = makeSource("https://cdn.example/cache-after-clear", "addon/cache-after-clear");
        history.save("cached", makeItem("item", "After clear"), afterClearSource, 12000, 100000);
        CHECK(history.cachedLoad("cached", "item").value("position", int64_t{-1}) == 12000);

        gate.release();
        history.flush();
        CHECK(observed.get() == 0);
        const auto finalRecord = history.load("cached", "item");
        CHECK(finalRecord.value("position", int64_t{-1}) == 12000);
        CHECK(finalRecord.value("source", nlohmann::json::object())
                  .value("identity", std::string()) == "addon/cache-after-clear");
    }

    // A failed callback is reported without stopping later queued jobs or a
    // synchronous flush barrier.
    {
        std::atomic<int> reportedFailures{0};
        stremio::AsyncPlaybackHistory history(path, [&] { ++reportedFailures; });
        history.enqueue([] { throw std::runtime_error("queued mutation failed"); });
        history.save("callback-failure", makeItem("item", "Still saved"), source, 300, 1000);
        history.flush();
        CHECK(reportedFailures.load() == 1);
        CHECK(history.load("callback-failure", "item").value("position", int64_t{-1}) == 300);
    }

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
        auto history = std::make_unique<stremio::AsyncPlaybackHistory>(path);
        WriterGate gate;
        gate.block(*history);
        history->save("account-a", makeItem("episode-stop", "Stop"), source, 25000, 100000);
        history->save("account-a", makeItem("episode-stop", "Stop"), source, 0, 100000);
        std::promise<void> destroyedPromise;
        auto destroyed = destroyedPromise.get_future();
        std::thread destroyer([history = std::move(history), &destroyedPromise]() mutable {
            history.reset();
            destroyedPromise.set_value();
        });
        gate.release();
        destroyed.wait();
        destroyer.join();
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
