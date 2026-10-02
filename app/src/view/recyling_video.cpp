#include "view/recyling_video.hpp"
#include "view/h_recycling.hpp"
#include "view/video_card.hpp"
#include "view/video_source.hpp"
#include "view/more_card.hpp"
#include "api/backend.hpp"

const std::string recylingVideoContentXML = R"xml(
    <brls:Box
        width="auto"
        height="auto"
        axis="column"
        marginBottom="36">

        <brls:Header
            marginBottom="6"
            id="recycler/title" />

        <HRecyclerFrame
            id="recycler/videos" />

    </brls:Box>
)xml";

brls::View* RecylingVideo::create() { return new RecylingVideo(); }

RecylingVideo::RecylingVideo() {
    this->inflateFromXMLString(recylingVideoContentXML);

    this->registerStringXMLAttribute("title", [this](std::string value) { this->setTitle(value); });

    this->registerFloatXMLAttribute("frameHeight", [this](float value) { this->setFrameHeight(value); });

    this->registerFloatXMLAttribute("itemWidth", [this](float value) { this->setItemWidth(value); });

    this->registerFloatXMLAttribute("itemSpace", [this](float value) {
        this->recycler->estimatedRowSpace = value;
        this->recycler->reloadData();
    });

    this->registerFloatXMLAttribute("sidePadding", [this](float value) { this->setSidePadding(value); });

    this->registerFloatXMLAttribute("pageSize", [this](float value) { this->setPageSize(value); });


    this->recycler->registerCell("Cell", VideoCardCell::create);
    this->recycler->registerCell("More", MoreCardCell::create);
}

RecylingVideo::~RecylingVideo() {}

void RecylingVideo::setTitle(const std::string& text) { this->title->setTitle(text); }

void RecylingVideo::setSidePadding(float padding) {
    this->title->setMarginLeft(padding);
    this->title->setMarginRight(padding);
    // padding inside the HRecyclerFrame: the cards slide up to the edges
    this->recycler->setPaddingLeft(padding);
    this->recycler->setPaddingRight(padding);
}

void RecylingVideo::setFrameHeight(float height) { this->recycler->setHeight(height); }

void RecylingVideo::setItemWidth(float width) {
    this->recycler->estimatedRowWidth = width;
    this->recycler->reloadData();
}

void RecylingVideo::setPageSize(size_t pageSize) { this->pageSize = pageSize; }


void RecylingVideo::setItems(const std::vector<media::Item>& items) { this->setItems(items, "", ""); }

void RecylingVideo::setItems(
    const std::vector<media::Item>& items, const std::string& moreTitle, const std::string& moreKey) {
    if (items.empty()) {
        this->setVisibility(brls::Visibility::GONE);
        this->recycler->clearData();
    } else {
        this->setVisibility(brls::Visibility::VISIBLE);
        auto* source = new VideoDataSource(items);
        source->setStremioContinueWatching(this->stremioContinueWatching);
        if (!moreKey.empty()) source->setMore(moreTitle, moreKey);
        this->recycler->setDataSource(source);
    }
}
