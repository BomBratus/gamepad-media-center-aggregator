#include "activity/local_player.hpp"
#include "view/video_view.hpp"
#include "view/video_profile.hpp"
#include "view/mpv_core.hpp"
#include "view/player_setting.hpp"

LocalPlayer::LocalPlayer(const std::string& name, const std::string& method) : view(new VideoView()) {
    const float width = brls::Application::contentWidth, height = brls::Application::contentHeight;
    this->setDimensions(width, height);
    view->setDimensions(width, height);
    view->setWidthPercentage(100);
    view->setHeightPercentage(100);
    view->setId("video");
    view->setTitie(name);
    view->hideVideoQuality();
    view->registerVideoSubtitle([](...) { PlayerSetting::showSubtitleMenu(nullptr); return true; });
    view->registerVideoAudio([](...) { PlayerSetting::showAudioMenu(nullptr); return true; });
    this->addView(view);
    eventSubscription = MPVCore::instance().getEvent()->subscribe([this, method](MpvEventEnum event) {
        if (event == MpvEventEnum::MPV_LOADED) view->getProfile()->init(method);
    });
    settingSubscription = view->getSettingEvent()->subscribe([]() {
        brls::Application::pushActivity(new brls::Activity(new PlayerSetting()));
    });
    playSubscription = view->getPlayEvent()->subscribe([](int) { return VideoView::close(true); });
}
LocalPlayer::~LocalPlayer() {
    MPVCore::instance().getEvent()->unsubscribe(eventSubscription);
    view->getPlayEvent()->unsubscribe(playSubscription);
    view->getSettingEvent()->unsubscribe(settingSubscription);
    MPVCore::instance().command("write-watch-later-config");
}
void LocalPlayer::play(const std::string& path, const std::string& name, const std::string& method) {
    auto* player = new LocalPlayer(name, method);
    brls::Application::pushActivity(new brls::Activity(player), brls::TransitionAnimation::NONE);
    MPVCore::instance().setUrl(path);
}
