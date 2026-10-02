#pragma once
#include <borealis.hpp>
#include "utils/event.hpp"

class LocalPlayer : public brls::Box {
public:
    LocalPlayer(const std::string& name, const std::string& method);
    ~LocalPlayer() override;
    static void play(const std::string& path, const std::string& name, const std::string& method = "Local");
private:
    class VideoView* view;
    MPVEvent::Subscription eventSubscription;
    brls::Event<int>::Subscription playSubscription;
    brls::VoidEvent::Subscription settingSubscription;
};
