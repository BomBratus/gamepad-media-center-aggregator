// Standalone lifecycle-state tests for safe playback position checkpoints.

#include <utils/playback_checkpoint.hpp>

#include <cstdio>

static int failures = 0;
#define CHECK(cond)                                            \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                        \
        }                                                      \
    } while (0)

int main() {
    utils::PlaybackCheckpoint checkpoint;

    // Loading a saved offset does not authorize samples until playback emits
    // its restart event; a load followed only by ordinary pause events is safe.
    checkpoint.begin(12000);
    checkpoint.loaded();
    CHECK(!checkpoint.ready());
    CHECK(!checkpoint.observe(5000));
    CHECK(checkpoint.position() == 12000);

    // A loaded restart permits samples, including a position moving backward.
    checkpoint.restart();
    CHECK(checkpoint.ready());
    CHECK(checkpoint.observe(9500));
    CHECK(checkpoint.position() == 9500);

    // Suspend invalidates samples until a later restart confirms the player is
    // active again. Seek/restart may move the position backward all the way to 0.
    checkpoint.suspend();
    CHECK(!checkpoint.ready());
    CHECK(!checkpoint.observe(9300));
    CHECK(checkpoint.position() == 9500);
    checkpoint.restart();
    CHECK(checkpoint.ready());
    CHECK(checkpoint.observe(4200));
    CHECK(checkpoint.observe(0));
    CHECK(checkpoint.position() == 0);

    // Starting another item resets lifecycle readiness and preserves its
    // requested offset instead of inheriting the previous item's sample.
    checkpoint.begin(12345);
    CHECK(!checkpoint.ready());
    CHECK(checkpoint.position() == 12345);
    CHECK(!checkpoint.observe(100));
    CHECK(checkpoint.position() == 12345);
    checkpoint.loaded();
    CHECK(!checkpoint.observe(200));
    checkpoint.restart();
    CHECK(checkpoint.observe(12300));

    // Stop/reset invalidates readiness but retains the last safe sample; zero
    // and negative values cannot overwrite it until a new item begins.
    checkpoint.stop();
    CHECK(!checkpoint.ready());
    CHECK(checkpoint.position() == 12300);
    CHECK(!checkpoint.observe(0));
    CHECK(!checkpoint.observe(-1));
    CHECK(checkpoint.position() == 12300);

    checkpoint.begin(0);
    checkpoint.loaded();
    checkpoint.restart();
    CHECK(!checkpoint.observe(-1));
    CHECK(checkpoint.position() == 0);
    CHECK(checkpoint.observe(0));
    CHECK(checkpoint.position() == 0);

    if (failures == 0) {
        printf("test_playback_checkpoint: OK\n");
        return 0;
    }
    printf("test_playback_checkpoint: %d FAILURE(S)\n", failures);
    return 1;
}
