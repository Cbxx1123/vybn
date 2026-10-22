#include "DevicePicker.h"

#include <algorithm>
#include <cctype>

namespace daw {

using namespace ui;

namespace {

constexpr float kWidth = 620.0f;
constexpr float kListHeight = 340.0f;

std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Every word of the search somewhere in the entry's name, category or source
bool matches(const CatalogueEntry &e, const std::string &search)
{
    const std::string hay = lower(e.device.name + " " + e.category + " " + e.source);
    size_t start = 0;
    while (start < search.size()) {
        while (start < search.size() && search[start] == ' ')
            ++start;
        size_t end = search.find(' ', start);
        if (end == std::string::npos)
            end = search.size();
        if (end > start && hay.find(search.substr(start, end - start)) == std::string::npos)
            return false;
        start = end;
    }
    return true;
}

Icon iconFor(const CatalogueEntry &e)
{
    switch (e.device.kind) {
    case audio::DeviceKind::Sampler: return Icon::Keys;
    case audio::DeviceKind::Effect: return Icon::Wave;
    case audio::DeviceKind::Synth: return Icon::Wave;
    default: return Icon::Sliders;
    }
}

}

DevicePicker::DevicePicker(Context &context, DeviceCatalogue &catalogue, DevicePickerOptions options)
    : context_(context), catalogue_(catalogue), options_(std::move(options)), panel_(std::make_unique<Panel>(Direction::Column))
{
    panel_->setAlign(Align::Stretch);
    panel_->setDepth(Depth::Deep);

    auto &head = panel_->add<Container>(Direction::Row);
    head.setAlign(Align::Center);
    auto &titles = head.add<Container>(Direction::Column);
    titles.setAlign(Align::Start);
    titles.add<Label>(options_.title.empty() ? (options_.instruments ? "Choose an instrument" : "Add an effect") : options_.title,
                      TextStyle::Subtitle);
    titles.add<Label>(options_.instruments ? "Your library's instruments and your instrument plugins"
                                           : "Built-in effects and your effect plugins",
                      TextStyle::Caption, TextTone::Muted);
    head.add<Spacer>();
    head.add<Button>([this] { close(); }).setIcon(Icon::Close).setVariant(ButtonVariant::Flat).setShape(Shape::Circle).setSize(Size::Small);

    search_ = &panel_->add<TextField>();
    search_->setPlaceholder(options_.instruments ? "Search instruments" : "Search effects");
    search_->onChange([this](const std::string &) {
        highlighted_ = 0;
        requestRebuild();
    });
    search_->onSubmit([this](const std::string &) {
        if (highlighted_ >= 0 && static_cast<size_t>(highlighted_) < shownKeys_.size())
            if (const CatalogueEntry *e = catalogue_.find(shownKeys_[static_cast<size_t>(highlighted_)]))
                pick(*e);
    });

    auto &body = panel_->add<Container>(Direction::Row);
    body.setAlign(Align::Stretch);
    auto &categoryScroll = body.add<ScrollView>();
    categoryScroll.setViewHeight(kListHeight);
    categoryScroll.setMinSize({170.0f, 0.0f});
    categories_ = &categoryScroll.content().add<ListView>();
    listScroll_ = &body.add<ScrollView>();
    listScroll_->setViewHeight(kListHeight);
    listScroll_->setGrow(1.0f);
    list_ = &listScroll_->content().add<ListView>();
    empty_ = &listScroll_->content().add<Label>("", TextStyle::Body, TextTone::Muted);

    auto &foot = panel_->add<Container>(Direction::Row);
    foot.setAlign(Align::Center);
    foot.add<Label>("Up and Down move, Enter chooses, Esc closes", TextStyle::Caption, TextTone::Muted);
    foot.add<Spacer>();
    if (options_.browse) {
        foot.add<Button>([this] {
                auto browse = options_.browse;
                close();
                browse();
            })
            .setLabel("Browser...")
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small);
    }
    if (options_.managePlugins) {
        foot.add<Button>([this] {
                auto manage = options_.managePlugins;
                close();
                manage();
            })
            .setLabel("Manage plugins...")
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small);
    }

    // Up, Down and Escape, ahead of the focused search field (which would take them)
    keys_ = context.signals().key.connect(
        [this](const engine::KeyEvent &e) {
            if (!e.down)
                return false;
            if (e.key == engine::Key::Escape) {
                close();
                return true;
            }
            if (e.key == engine::Key::Up || e.key == engine::Key::Down) {
                move(e.key == engine::Key::Up ? -1 : 1);
                return true;
            }
            return false;
        },
        Priority::Focus + 10);
    rebuild();
}

DevicePicker::~DevicePicker() = default;

DevicePicker &DevicePicker::open(Context &context, DeviceCatalogue &catalogue, DevicePickerOptions options)
{
    OverlayOptions overlay;
    overlay.closeOnOutsideClick = true;
    return context.openOverlay<DevicePicker>(std::move(overlay), context, catalogue, std::move(options));
}

void DevicePicker::close()
{
    keys_ = {};
    context_.closeOverlay(*this);
}

void DevicePicker::pick(const CatalogueEntry &entry)
{
    // Choosing the instrument already in use keeps it (and its settings) as it is
    if (options_.instruments && entry.key() == options_.current) {
        close();
        return;
    }
    // The entry may go when the catalogue changes: keep a copy for after the overlay has closed
    const CatalogueEntry chosen = entry;
    auto onPick = options_.onPick;
    close();
    if (onPick)
        context_.post([onPick, chosen] { onPick(chosen); });
}

std::vector<const CatalogueEntry *> DevicePicker::filtered()
{
    const std::string search = lower(search_->text());
    std::vector<const CatalogueEntry *> out;
    for (const CatalogueEntry &e : catalogue_.entries()) {
        if (e.instrument != options_.instruments)
            continue;
        if (!category_.empty() && e.category != category_)
            continue;
        if (!search.empty() && !matches(e, search))
            continue;
        out.push_back(&e);
    }
    // The instrument in use goes first, so it is the one highlighted when the picker opens
    if (options_.instruments)
        std::stable_partition(out.begin(), out.end(), [this](const CatalogueEntry *e) { return e->key() == options_.current; });
    return out;
}

void DevicePicker::requestRebuild()
{
    if (rebuildPosted_)
        return;
    rebuildPosted_ = true;
    context_.post([this] {
        rebuildPosted_ = false;
        rebuild();
    });
}

void DevicePicker::rebuild()
{
    builtFor_ = catalogue_.version();
    // The categories, with how many each has
    categories_->clear();
    size_t total = 0;
    for (const CatalogueEntry &e : catalogue_.entries())
        total += e.instrument == options_.instruments;
    auto addCategory = [this](const std::string &name, const std::string &label, size_t count) {
        auto &item = categories_->add<ListItem>(label, [this, name] {
            category_ = name;
            highlighted_ = 0;
            requestRebuild();
        });
        item.setTrailing(std::to_string(count)).setSize(Size::Small).setSelected(category_ == name);
    };
    addCategory("", "All", total);
    for (const std::string &c : catalogue_.categories(options_.instruments)) {
        size_t n = 0;
        for (const CatalogueEntry &e : catalogue_.entries())
            n += e.instrument == options_.instruments && e.category == c;
        addCategory(c, c, n);
    }

    // The devices
    list_->clear();
    shownKeys_.clear();
    const std::vector<const CatalogueEntry *> shown = filtered();
    highlighted_ = shown.empty() ? 0 : std::clamp(highlighted_, 0, static_cast<int>(shown.size()) - 1);
    for (size_t i = 0; i < shown.size() && i < 300; ++i) {
        const CatalogueEntry &e = *shown[i];
        const std::string key = e.key();
        shownKeys_.push_back(key);
        auto &item = list_->add<ListItem>(e.device.name, [this, key] {
            if (const CatalogueEntry *entry = catalogue_.find(key))
                pick(*entry);
        });
        const bool current = key == options_.current;
        item.setSubtitle(e.category + "  /  " + e.source)
            .setTrailing(current ? "In use" : e.plugin() ? "Plugin" : e.device.kind == audio::DeviceKind::Sampler ? "Library" : "Built-in")
            .setIcon(iconFor(e))
            .setSelected(static_cast<int>(i) == highlighted_);
    }
    if (shown.empty()) {
        if (!search_->text().empty())
            empty_->setText("Nothing matches \"" + search_->text() + "\"");
        else if (options_.instruments)
            empty_->setText("No instruments yet: import one, download one in the browser, or install a CLAP plugin");
        else
            empty_->setText("No effects here");
    } else {
        empty_->setText("");
    }
}

void DevicePicker::move(int by)
{
    if (shownKeys_.empty())
        return;
    highlighted_ = std::clamp(highlighted_ + by, 0, static_cast<int>(shownKeys_.size()) - 1);
    for (size_t i = 0; i < list_->childCount(); ++i)
        if (auto *item = dynamic_cast<ListItem *>(&list_->child(i)))
            item->setSelected(static_cast<int>(i) == highlighted_);
    // Keep it in view
    if (list_->childCount() > static_cast<size_t>(highlighted_)) {
        const Rect r = list_->child(static_cast<size_t>(highlighted_)).rect();
        const Rect view = listScroll_->rect();
        if (r.y < view.y)
            listScroll_->scrollTo(listScroll_->offsetY() - (view.y - r.y) - 8.0f);
        else if (r.y + r.h > view.y + view.h)
            listScroll_->scrollTo(listScroll_->offsetY() + (r.y + r.h - view.y - view.h) + 8.0f);
    }
}

Vec2 DevicePicker::measure(const Theme &theme) const
{
    const Vec2 size = panel_->preferredSize(theme);
    return {std::max(size.x, kWidth), size.y};
}

void DevicePicker::arrange(const Rect &window, const Theme &theme)
{
    const Vec2 size = measure(theme);
    const Rect &a = options_.anchor;
    // Below the anchor, else above it; inside the window either way
    float y = a.y + a.h + 8.0f;
    if (y + size.y > window.y + window.h - 8.0f)
        y = a.y - size.y - 8.0f;
    y = std::clamp(y, window.y + 8.0f, std::max(window.y + 8.0f, window.y + window.h - size.y - 8.0f));
    const float x = std::clamp(a.x, window.x + 8.0f, std::max(window.x + 8.0f, window.x + window.w - size.x - 8.0f));
    // Unfolds slightly into place as it opens
    rect_ = {x, y - (1.0f - shown_) * 10.0f, size.x, size.y};
    panel_->arrange(rect_, theme);
}

void DevicePicker::handleInput(const Input &input)
{
    shown_ += (1.0f - shown_) * input.ease(18.0f);
    if (catalogue_.version() != builtFor_)
        requestRebuild(); // a scan finished, the library changed
    panel_->handleInput(input);
    if (!focusedOnce_) {
        focusedOnce_ = true;
        search_->focus();
    }
}

void DevicePicker::draw(DrawList &list, const Theme &theme) const { panel_->draw(list, theme); }

}
