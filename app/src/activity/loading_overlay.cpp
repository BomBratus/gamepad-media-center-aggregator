#include "activity/loading_overlay.hpp"
#include <utility>

using namespace brls::literals;

// Probing a reachable server resolves in well under a second; if we are still
// waiting past this, surface a reassuring line so the wait does not read as a
// freeze (thcolin/pleNx#1).
static const brls::Time SLOW_HINT_DELAY = 5000;

LoadingOverlay::LoadingOverlay(std::string message, std::string slowHint)
    : messageText(std::move(message)), slowHintText(std::move(slowHint)) {
    brls::Logger::debug("LoadingOverlay: create");
}

LoadingOverlay::~LoadingOverlay() {
    // Stop before our views tear down so the end callback can't touch them.
    this->slowTimer.stop();
    brls::Logger::debug("LoadingOverlay: delete");
}

void LoadingOverlay::onContentAvailable() {
    if (!this->messageText.empty()) this->message->setText(this->messageText);
    if (!this->slowHintText.empty()) this->hint->setText(this->slowHintText);

    // A custom message without a custom slow hint should not reveal the
    // Plex-specific "taking longer" copy from the XML.
    if (!this->messageText.empty() && this->slowHintText.empty()) return;

    this->slowTimer.setEndCallback([this](bool finished) {
        // finished == false means the timer was stopped (overlay dismissed or
        // destroyed): the work completed in time, leave the hint hidden.
        if (!finished) return;
        this->hint->setVisibility(brls::Visibility::VISIBLE);
    });
    this->slowTimer.start(SLOW_HINT_DELAY);
}
