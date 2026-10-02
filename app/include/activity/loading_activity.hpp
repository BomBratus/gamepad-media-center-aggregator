/*
    Full-screen loading screen: spinner + "Connecting to server...".
    Shown while probing the server URLs (Stremio account
    servers often advertise 10+ connections including unreachable local IPs, so
    the candidates are raced in parallel), both at startup (AppConfig::checkLogin)
    and when selecting a profile (ServerList).
*/

#pragma once

#include <borealis.hpp>

class LoadingActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("activity/loading.xml");

    LoadingActivity();

    ~LoadingActivity() override;
};
