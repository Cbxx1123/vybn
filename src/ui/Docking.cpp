#include "Docking.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ui {

namespace {

constexpr float kMinCard = 140.0f;  // a card's least length along a split
constexpr float kTearY = 14.0f;     // how far a tab is dragged out of its strip before it tears off
constexpr float kStripSlack = 10.0f; // a tab strip takes drops this far above and below its tabs
constexpr float kWindowMinW = 360.0f;
constexpr float kWindowMinH = 240.0f;
constexpr float kKnob = 34.0f;      // a grid-edge target's size
constexpr float kKnobInset = 22.0f; // its distance from the edge
constexpr float kLandSeconds = 0.32f;

const DockSide kSides[] = {DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom};

// A small round button: level until hovered, pressed in while held
void headerButton(DrawList &list, const Theme &theme, const Rect &r, const ClickState &click, Icon icon)
{
    if (click.held())
        list.add(theme.surface(r, Shape::Circle, Elevation::Inset, 0.4f));
    else if (click.hovered)
        list.add(theme.surface(r, Shape::Circle, Elevation::Raised, 0.4f));
    drawIcon(list, theme, icon, r.inset(r.w * 0.22f), click.hovered ? TextTone::Accent : TextTone::Secondary);
}

float headerHeight(const Theme &theme)
{
    return theme.controlHeight(Size::Small);
}

Rect sideOf(const Rect &box, DockSide s, float share)
{
    switch (s) {
    case DockSide::Left: return {box.x, box.y, box.w * share, box.h};
    case DockSide::Right: return {box.x + box.w * (1.0f - share), box.y, box.w * share, box.h};
    case DockSide::Top: return {box.x, box.y, box.w, box.h * share};
    case DockSide::Bottom: return {box.x, box.y + box.h * (1.0f - share), box.w, box.h * share};
    case DockSide::Center: break;
    }
    return box;
}

Rect circle(Vec2 c, float r)
{
    return {c.x - r, c.y - r, 2.0f * r, 2.0f * r};
}

Rect lerp(const Rect &a, const Rect &b, float t)
{
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}

float distance(const Rect &a, const Rect &b)
{
    return std::max({std::abs(a.x - b.x), std::abs(a.y - b.y), std::abs(a.w - b.w), std::abs(a.h - b.h)});
}

// A short accent bar inside a round target, on the side it docks to (a dot for the middle)
Rect edgeMark(Vec2 c, float r, DockSide side)
{
    const float thick = r * 0.3f, length = r * 1.05f, out = r * 0.42f;
    switch (side) {
    case DockSide::Left: return {c.x - out - thick * 0.5f, c.y - length * 0.5f, thick, length};
    case DockSide::Right: return {c.x + out - thick * 0.5f, c.y - length * 0.5f, thick, length};
    case DockSide::Top: return {c.x - length * 0.5f, c.y - out - thick * 0.5f, length, thick};
    case DockSide::Bottom: return {c.x - length * 0.5f, c.y + out - thick * 0.5f, length, thick};
    case DockSide::Center: break;
    }
    return circle(c, r * 0.36f);
}

// One round target: an inset dimple that rises and lights up as it becomes the one under the
// mouse
void drawTarget(DrawList &list, const Theme &theme, Vec2 c, float r, DockSide side, float hot, float shown)
{
    if (hot > 0.5f)
        list.add(theme.surface(circle(c, r), Shape::Circle, Elevation::Raised, 0.6f * shown, Finish::Convex));
    else
        list.add(theme.surface(circle(c, r), Shape::Circle, Elevation::Inset, 0.5f * shown));
    const Shape shape = side == DockSide::Center ? Shape::Circle : Shape::Pill;
    list.add(theme.accent(edgeMark(c, r * (1.0f + 0.12f * hot), side), shape, (0.4f + 0.6f * hot) * shown));
}

// Moves a node's contents (a split's children, or a card's panels) into another node
template <typename Node>
void moveContent(Node &to, Node &from)
{
    to.first = std::move(from.first);
    to.second = std::move(from.second);
    if (to.first) {
        to.first->parent = &to;
        to.second->parent = &to;
    }
    to.direction = from.direction;
    to.ratio = from.ratio;
    to.shownRatio = from.shownRatio;
    to.panels = std::move(from.panels);
    to.current = from.current;
    from.panels.clear();
    from.current = 0;
}

// A floating panel's header controls, in its window's title bar
class HeaderSlot : public Component {
public:
    explicit HeaderSlot(DockHost &host) : host_(host) {}

    Vec2 measure(const Theme &theme) const override
    {
        return host_.panel() ? host_.panel()->header().preferredSize(theme) : Vec2{};
    }
    void arrange(const Rect &rect, const Theme &theme) override
    {
        rect_ = rect;
        if (DockPanel *p = host_.panel())
            p->header().arrange(rect, theme);
    }
    void handleInput(const Input &input) override
    {
        if (DockPanel *p = host_.panel())
            p->header().handleInput(host_.busy() ? input.occluded() : input);
    }
    void draw(DrawList &list, const Theme &theme) const override
    {
        if (const DockPanel *p = host_.panel())
            p->header().draw(list, theme);
    }

private:
    DockHost &host_;
};

}

// DockPanel

DockPanel::DockPanel(std::string title)
    : Container(Direction::Column), title_(std::move(title)), header_(std::make_unique<Container>(Direction::Row))
{
    align_ = Align::Stretch;
    header_->setAlign(Align::Center);
}

// DockSpace

DockSpace::DockSpace(UISystem &system, Context &context)
    : system_(system), context_(context), root_(std::make_unique<Node>())
{
    grow_ = 1.0f;
    alignSelf_ = Align::Stretch;
}

DockPanel &DockSpace::addPanel(std::string title)
{
    panels_.push_back(std::make_unique<DockPanel>(std::move(title)));
    return *panels_.back();
}

DockSpace::Node *DockSpace::leafOf(const DockPanel &panel) const
{
    std::vector<Node *> stack{root_.get()};
    while (!stack.empty()) {
        Node *n = stack.back();
        stack.pop_back();
        if (n->isSplit()) {
            stack.push_back(n->first.get());
            stack.push_back(n->second.get());
        } else if (std::find(n->panels.begin(), n->panels.end(), &panel) != n->panels.end()) {
            return n;
        }
    }
    return nullptr;
}

DockSpace::Node *DockSpace::firstLeaf(Node &node) const
{
    Node *n = &node;
    while (n->isSplit())
        n = n->first.get();
    return n;
}

const DockSpace::Floating *DockSpace::floating(const DockPanel &panel) const
{
    for (const Floating &f : floating_) {
        if (f.panel == &panel)
            return &f;
    }
    return nullptr;
}

void DockSpace::forgetPointers()
{
    // The tree is changing: nothing may point into it
    dragging_ = nullptr;
    hoveredHandle_ = nullptr;
    pressedTab_ = nullptr;
    pressedNode_ = nullptr;
    dialNode_ = nullptr;
    target_ = {};
}

void DockSpace::insert(DockPanel &panel, Node &node, DockSide side, float share)
{
    forgetPointers();
    // Into an empty grid, or as another tab
    if (side == DockSide::Center || (!node.isSplit() && node.panels.empty())) {
        Node &leaf = *firstLeaf(node);
        leaf.panels.push_back(&panel);
        leaf.current = leaf.panels.size() - 1;
        return;
    }
    // Beside it: the node becomes a split of what it held and the new card, which grows in
    auto old = std::make_unique<Node>();
    moveContent(*old, node);
    auto fresh = std::make_unique<Node>();
    fresh->panels.push_back(&panel);
    const bool before = side == DockSide::Left || side == DockSide::Top;
    node.direction = side == DockSide::Left || side == DockSide::Right ? Direction::Row : Direction::Column;
    share = std::clamp(share, 0.1f, 0.9f);
    node.ratio = before ? share : 1.0f - share;
    node.shownRatio = before ? 0.0f : 1.0f;
    node.first = before ? std::move(fresh) : std::move(old);
    node.second = before ? std::move(old) : std::move(fresh);
    node.first->parent = &node;
    node.second->parent = &node;
}

void DockSpace::remove(DockPanel &panel)
{
    Node *leaf = leafOf(panel);
    if (!leaf)
        return;
    forgetPointers();
    leaf->panels.erase(std::find(leaf->panels.begin(), leaf->panels.end(), &panel));
    leaf->current = std::min(leaf->current, leaf->panels.empty() ? 0 : leaf->panels.size() - 1);
    if (leaf->panels.empty())
        collapse(*leaf);
}

void DockSpace::collapse(Node &leaf)
{
    // An empty card goes; its sibling takes the split's place. An empty grid stays as one card.
    Node *parent = leaf.parent;
    if (!parent)
        return;
    const bool isFirst = parent->first.get() == &leaf;
    std::unique_ptr<Node> gone = std::move(isFirst ? parent->first : parent->second);
    std::unique_ptr<Node> sibling = std::move(isFirst ? parent->second : parent->first);
    moveContent(*parent, *sibling);
}

void DockSpace::releaseFloating(DockPanel &panel)
{
    for (auto it = floating_.begin(); it != floating_.end(); ++it) {
        if (it->panel != &panel)
            continue;
        // Its window goes (at the next tick); the panel comes back here at once
        it->host->detach();
        system_.releaseToolWindow(it->id);
        floating_.erase(it);
        break;
    }
}

void DockSpace::dock(DockPanel &panel, DockPanel *target, DockSide side, float share)
{
    releaseFloating(panel);
    if (leafOf(panel))
        return;
    Node *at = target ? leafOf(*target) : nullptr;
    insert(panel, at ? *at : *root_, side, share);
}

void DockSpace::dockTab(DockPanel &panel, DockPanel &beside, bool after)
{
    releaseFloating(panel);
    Node *leaf = leafOf(beside);
    if (!leaf || leafOf(panel))
        return;
    forgetPointers();
    const size_t at = static_cast<size_t>(std::find(leaf->panels.begin(), leaf->panels.end(), &beside) - leaf->panels.begin());
    const size_t index = at + (after ? 1 : 0);
    leaf->panels.insert(leaf->panels.begin() + static_cast<long>(index), &panel);
    leaf->current = index;
}

void DockSpace::finishTab(DockPanel &panel, DockPanel &beside, bool after)
{
    // After this frame, as finishDock does
    system_.post([this, p = &panel, b = &beside, after] {
        if (leafOf(*b))
            dockTab(*p, *b, after);
        else
            dock(*p, nullptr, DockSide::Bottom, 0.35f);
        show(*p);
    });
}

Rect DockSpace::toScreen(const Rect &r) const
{
    const Vec2 a = context_.toScreen({r.x, r.y});
    const Vec2 b = context_.toScreen({r.x + r.w, r.y + r.h});
    return {a.x, a.y, b.x - a.x, b.y - a.y};
}

void DockSpace::float_(DockPanel &panel, Rect frame)
{
    const Node *leaf = leafOf(panel);
    floatPanel(panel, leaf ? toScreen(leaf->rect) : frame, frame, false, {});
}

void DockSpace::floatPanel(DockPanel &panel, Rect from, Rect to, bool dragging, Vec2 grab)
{
    if (isFloating(panel))
        return;
    if (Node *leaf = leafOf(panel)) {
        // Remember where it was, for docking it back
        panel.homeAnchor_ = nullptr;
        panel.homeSide_ = DockSide::Center;
        panel.homeShare_ = 0.5f;
        if (leaf->panels.size() > 1) {
            panel.homeAnchor_ = leaf->panels[0] != &panel ? leaf->panels[0] : leaf->panels[1];
        } else if (Node *parent = leaf->parent) {
            const bool isFirst = parent->first.get() == leaf;
            Node &sibling = isFirst ? *parent->second : *parent->first;
            Node *other = firstLeaf(sibling);
            panel.homeAnchor_ = other->panels.empty() ? nullptr : other->panels[other->current];
            if (parent->direction == Direction::Row)
                panel.homeSide_ = isFirst ? DockSide::Left : DockSide::Right;
            else
                panel.homeSide_ = isFirst ? DockSide::Top : DockSide::Bottom;
            panel.homeShare_ = isFirst ? parent->ratio : 1.0f - parent->ratio;
        }
        remove(panel);
    }

    DockPanel *p = &panel;
    UISystem::WindowParts parts = system_.openToolWindow(
        panel.title(), static_cast<uint32_t>(std::max(1.0f, to.w)), static_cast<uint32_t>(std::max(1.0f, to.h)),
        [this, p](Container &) { windowGone(*p); },
        // Its close button docks it, flying back first (or closes a closable one)
        [this, p] {
            if (p->closable())
                removePanel(*p);
            else
                dockHome(*p);
            return false;
        });
    parts.window->setPosition(static_cast<int>(std::lround(from.x)), static_cast<int>(std::lround(from.y)));

    DockHost &host = parts.body->add<DockHost>(*this, panel, *parts.window, *parts.titleBar, from, to, dragging, grab);
    TitleBar &bar = *parts.titleBar;
    bar.add<Label>(panel.title(), TextStyle::Subtitle);
    bar.add<Spacer>();
    bar.add<HeaderSlot>(host);
    bar.add<Button>([this, p] { dockHome(*p); })
        .setIcon(Icon::Dock)
        .setVariant(ButtonVariant::Flat)
        .setShape(Shape::Circle)
        .setSize(Size::Small);
    floating_.push_back({&panel, parts.id, &host});
}

void DockSpace::dockHome(DockPanel &panel)
{
    const Floating *f = floating(panel);
    if (!f || f->host->landing())
        return;
    DockPanel *anchor = panel.homeAnchor_;
    if (anchor && !leafOf(*anchor))
        anchor = nullptr;
    DockSide side = panel.homeSide_;
    const bool empty = !root_->isSplit() && root_->panels.empty();
    // Its neighbour is gone: under the whole grid
    if (!anchor && side == DockSide::Center && !empty)
        side = DockSide::Bottom;
    const float share = anchor ? panel.homeShare_ : 0.35f;

    // Where it is going, for the window to fly to
    Rect into = rect_;
    if (const Node *leaf = anchor ? leafOf(*anchor) : nullptr)
        into = previewFor(*leaf, side, share);
    else if (!empty)
        into = previewFor(*root_, side, share);
    DockPanel *p = &panel;
    f->host->land(toScreen(into), [this, p, anchor, side, share] { finishDock(*p, anchor, side, share); });
}

void DockSpace::finishDock(DockPanel &panel, DockPanel *anchor, DockSide side, float share)
{
    // After this frame: the window's own input pass is still under way
    system_.post([this, p = &panel, anchor, side, share] {
        DockPanel *at = anchor && leafOf(*anchor) ? anchor : nullptr;
        dock(*p, at, side, share);
        show(*p);
    });
}

void DockSpace::windowGone(DockPanel &panel)
{
    // Closed some other way than docking (the app closing it, say): the panel comes home at once
    const auto it = std::find_if(floating_.begin(), floating_.end(), [&](const Floating &f) { return f.panel == &panel; });
    if (it == floating_.end())
        return;
    floating_.erase(it);
    if (panel.closable()) {
        removePanel(panel);
        return;
    }
    DockPanel *anchor = panel.homeAnchor_ && leafOf(*panel.homeAnchor_) ? panel.homeAnchor_ : nullptr;
    DockSide side = panel.homeSide_;
    if (!anchor && side == DockSide::Center && (root_->isSplit() || !root_->panels.empty()))
        side = DockSide::Bottom;
    dock(panel, anchor, side, anchor ? panel.homeShare_ : 0.35f);
}

void DockSpace::show(DockPanel &panel)
{
    if (Node *leaf = leafOf(panel)) {
        leaf->current = static_cast<size_t>(std::find(leaf->panels.begin(), leaf->panels.end(), &panel) - leaf->panels.begin());
        return;
    }
    if (const Floating *f = floating(panel))
        f->host->focus();
}

bool DockSpace::isFloating(const DockPanel &panel) const
{
    return floating(panel) != nullptr;
}

bool DockSpace::isShowing(const DockPanel &panel) const
{
    if (const Node *leaf = leafOf(panel))
        return leaf->panels[leaf->current] == &panel;
    return isFloating(panel);
}

void DockSpace::removePanel(DockPanel &panel)
{
    system_.post([this, p = &panel] {
        auto it = std::find_if(panels_.begin(), panels_.end(), [p](const std::unique_ptr<DockPanel> &q) { return q.get() == p; });
        if (it == panels_.end())
            return;
        releaseFloating(*p);
        remove(*p);
        for (auto &other : panels_)
            if (other->homeAnchor_ == p)
                other->homeAnchor_ = nullptr;
        if (p->onClose_)
            p->onClose_();
        // Destroyed after this frame, once nothing it holds is mid-callback
        std::shared_ptr<DockPanel> gone(std::move(*it));
        panels_.erase(it);
        system_.post([gone] {});
    });
}

void DockSpace::resizeFloating(const DockPanel &panel, float width, float height)
{
    if (const Floating *f = floating(panel))
        f->host->resizeTo(width, height);
}

bool DockSpace::floatingFrame(const DockPanel &panel, Rect &frame) const
{
    const Floating *f = floating(panel);
    if (!f)
        return false;
    frame = f->host->shown();
    return true;
}

void DockSpace::setShare(const DockPanel &panel, float share)
{
    Node *leaf = leafOf(panel);
    if (!leaf || !leaf->parent)
        return;
    share = std::clamp(share, 0.0f, 1.0f);
    leaf->parent->ratio = leaf->parent->first.get() == leaf ? share : 1.0f - share;
}

// Layout

Vec2 DockSpace::measure(const Theme &) const
{
    // The cards share whatever the parent gives
    return {2.0f * kMinCard, 2.0f * kMinCard};
}

void DockSpace::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    arrangeNode(*root_, rect, theme);
}

void DockSpace::arrangeNode(Node &node, const Rect &rect, const Theme &theme)
{
    node.rect = rect;
    if (node.isSplit()) {
        const bool row = node.direction == Direction::Row;
        const float handle = theme.spacing();
        node.available = std::max(0.0f, (row ? rect.w : rect.h) - handle);
        float a = node.shownRatio * node.available;
        // Minimums hold once a new split has opened
        const bool opening = std::abs(node.shownRatio - node.ratio) > 0.01f;
        if (!opening && node.available >= 2.0f * kMinCard)
            a = std::clamp(a, kMinCard, node.available - kMinCard);
        a = std::round(std::clamp(a, 0.0f, node.available));
        const float b = node.available - a;
        if (row) {
            arrangeNode(*node.first, {rect.x, rect.y, a, rect.h}, theme);
            node.handle = {rect.x + a, rect.y, handle, rect.h};
            arrangeNode(*node.second, {rect.x + a + handle, rect.y, b, rect.h}, theme);
        } else {
            arrangeNode(*node.first, {rect.x, rect.y, rect.w, a}, theme);
            node.handle = {rect.x, rect.y + a, rect.w, handle};
            arrangeNode(*node.second, {rect.x, rect.y + a + handle, rect.w, b}, theme);
        }
        return;
    }

    // A card: the tabs and the current panel's controls over its content
    const float pad = theme.padding();
    const float h = headerHeight(theme);
    node.header = {rect.x + pad, rect.y + pad, std::max(0.0f, rect.w - 2.0f * pad), h};
    const float button = h * 0.8f;
    node.popOut = node.panels.empty() ? Rect{}
                                      : Rect{node.header.x + node.header.w - button, node.header.y + (h - button) * 0.5f, button, button};
    node.tabs.clear();
    node.closes.clear();
    float x = node.header.x;
    for (const DockPanel *p : node.panels) {
        const float close = p->closable() ? h * 0.55f : 0.0f;
        const float w = theme.measureText(p->title(), Size::Small).x + 28.0f + close;
        node.tabs.push_back({x, node.header.y, w, h});
        node.closes.push_back(p->closable() ? Rect{x + w - 10.0f - close, node.header.y + (h - close) * 0.5f, close, close} : Rect{});
        x += w;
    }
    const float top = node.header.y + h + theme.spacing();
    node.content = {rect.x + pad, top, std::max(0.0f, rect.w - 2.0f * pad), std::max(0.0f, rect.y + rect.h - pad - top)};
    if (node.panels.empty())
        return;

    DockPanel &panel = *node.panels[node.current];
    Container &controls = panel.header();
    const Vec2 size = controls.preferredSize(theme);
    const float right = node.popOut.x - theme.spacing();
    const float w = std::max(0.0f, std::min(size.x, right - (x + theme.spacing())));
    controls.arrange({right - w, node.header.y + (h - size.y) * 0.5f, w, size.y}, theme);
    panel.arrange(node.content, theme);
}

// Input

void DockSpace::handleInput(const Input &input)
{
    hoveredHandle_ = nullptr;
    if (!input.down)
        dragging_ = nullptr;
    if (dragging_ && dragging_->available > 0.0f) {
        const bool row = dragging_->direction == Direction::Row;
        const float along = row ? input.mouse.x : input.mouse.y;
        const float start = row ? dragging_->rect.x : dragging_->rect.y;
        dragging_->ratio = std::clamp((along - dragOffset_ - start) / dragging_->available, 0.0f, 1.0f);
        dragging_->shownRatio = dragging_->ratio;
    }
    easeNode(*root_, input.ease(12.0f));

    inputNode(*root_, input);

    // A tab dragged along its strip moves along it; one dragged out of the strip lifts out into a
    // window of its own that follows the mouse
    const bool tearing = pressedTab_ && pressedNode_ && input.down &&
                         (input.mouse.y < pressedNode_->header.y - kTearY ||
                          input.mouse.y > pressedNode_->header.y + pressedNode_->header.h + kTearY);
    if (pressedTab_ && !input.down) {
        pressedTab_ = nullptr;
        pressedNode_ = nullptr;
    } else if (tearing) {
        DockPanel *panel = pressedTab_;
        const Rect bounds = context_.bounds();
        const Vec2 size{std::clamp(pressedCard_.w, kWindowMinW + 200.0f, std::max(kWindowMinW, bounds.w * 0.62f)),
                        std::clamp(pressedCard_.h, kWindowMinH + 160.0f, std::max(kWindowMinH, bounds.h * 0.55f))};
        const Vec2 grab{std::clamp(pressedAt_.x - pressedCard_.x, 60.0f, size.x - 160.0f), 22.0f};
        const Rect from = toScreen(pressedCard_);
        const Rect to = toScreen({input.mouse.x - grab.x, input.mouse.y - grab.y, size.x, size.y});
        const Vec2 mouse = context_.toScreen(input.mouse);
        const Vec2 screenGrab{mouse.x - to.x, mouse.y - to.y};
        pressedTab_ = nullptr;
        pressedNode_ = nullptr;
        context_.post([this, panel, from, to, screenGrab] { floatPanel(*panel, from, to, true, screenGrab); });
    } else if (pressedTab_ && pressedNode_) {
        reorderTab(input);
    }

    // The drop targets follow the window being dragged. Over a tab strip the strip is the target,
    // so the dial stays down.
    const bool empty = !root_->isSplit() && root_->panels.empty();
    const Node *card = showTargets_ && !empty && !target_.onTab ? leafAt(*root_, mouse_) : nullptr;
    if (card) {
        if (card != dialNode_) {
            dialNode_ = card;
            dialShown_ = 0.0f; // grows in on each card it moves to
        }
        dialCenter_ = card->rect.center();
        dialShown_ += (1.0f - dialShown_) * input.ease(16.0f);
    } else {
        dialNode_ = nullptr;
        dialShown_ += (0.0f - dialShown_) * input.ease(22.0f);
    }
    const float hotRate = input.ease(22.0f);
    for (size_t s = 0; s < hot_.size(); ++s) {
        const bool hot = card && target_.node == card && !target_.onTab && static_cast<size_t>(target_.side) == s;
        hot_[s] += ((hot ? 1.0f : 0.0f) - hot_[s]) * hotRate;
    }
    const bool edges = showTargets_ && root_->isSplit();
    edgesShown_ += ((edges ? 1.0f : 0.0f) - edgesShown_) * input.ease(16.0f);
    for (size_t i = 0; i < edgeHot_.size(); ++i) {
        const bool hot = edges && target_.node == root_.get() && target_.side == kSides[i];
        edgeHot_[i] += ((hot ? 1.0f : 0.0f) - edgeHot_[i]) * hotRate;
    }
    if (target_.node) {
        previewShown_ = previewAlpha_ < 0.02f ? target_.preview : lerp(previewShown_, target_.preview, input.ease(20.0f));
        previewAlpha_ += (1.0f - previewAlpha_) * input.ease(18.0f);
    } else {
        previewAlpha_ += (0.0f - previewAlpha_) * input.ease(22.0f);
    }
}

void DockSpace::easeNode(Node &node, float amount)
{
    if (!node.isSplit())
        return;
    node.shownRatio += (node.ratio - node.shownRatio) * amount;
    if (std::abs(node.ratio - node.shownRatio) < 0.001f)
        node.shownRatio = node.ratio;
    easeNode(*node.first, amount);
    easeNode(*node.second, amount);
}

void DockSpace::inputNode(Node &node, const Input &input)
{
    // While a handle or a tab is dragged, the panels only see the release
    const bool busy = dragging_ != nullptr || pressedTab_ != nullptr;
    if (node.isSplit()) {
        if (!busy && node.handle.contains(input.mouse)) {
            hoveredHandle_ = &node;
            if (input.pressed) {
                dragging_ = &node;
                const bool row = node.direction == Direction::Row;
                dragOffset_ = row ? input.mouse.x - node.handle.x : input.mouse.y - node.handle.y;
            }
        }
        inputNode(*node.first, input);
        inputNode(*node.second, input);
        return;
    }
    if (node.panels.empty())
        return;

    node.hoveredTab = -1;
    for (size_t i = 0; i < node.tabs.size(); ++i) {
        if (!node.tabs[i].contains(input.mouse))
            continue;
        node.hoveredTab = static_cast<int>(i);
        if (input.pressed && !busy && i < node.closes.size() && node.closes[i].w > 0.0f && node.closes[i].inset(-3.0f).contains(input.mouse)) {
            removePanel(*node.panels[i]);
            continue;
        }
        if (input.pressed && !busy) {
            node.current = i;
            pressedTab_ = node.panels[i];
            pressedAt_ = input.mouse;
            pressedCard_ = node.rect;
            pressedNode_ = &node;
            tabGrab_ = input.mouse.x - node.tabs[i].x;
            tabX_ = node.tabs[i].x;
        }
    }
    if (node.popOutClick.update(node.popOut, input, !busy)) {
        DockPanel *panel = node.panels[node.current];
        const Rect from = toScreen(node.rect);
        const Rect to = toScreen({node.rect.x + 40.0f, node.rect.y + 40.0f, std::max(kWindowMinW, node.rect.w),
                                  std::max(kWindowMinH, node.rect.h)});
        context_.post([this, panel, from, to] { floatPanel(*panel, from, to, false, {}); });
    }

    DockPanel &panel = *node.panels[node.current];
    const Input &forwarded = dragging_ || pressedTab_ ? input.occluded() : input;
    panel.header().handleInput(forwarded);
    panel.handleInput(forwarded);
}

void DockSpace::reorderTab(const Input &input)
{
    Node &node = *pressedNode_;
    tabX_ = input.mouse.x - tabGrab_;
    const auto it = std::find(node.panels.begin(), node.panels.end(), pressedTab_);
    if (it == node.panels.end() || node.tabs.size() != node.panels.size())
        return;
    size_t i = static_cast<size_t>(it - node.panels.begin());
    // Past a neighbour's middle: they swap, one step a frame
    if (i > 0 && input.mouse.x < node.tabs[i - 1].center().x) {
        std::swap(node.panels[i], node.panels[i - 1]);
        --i;
    } else if (i + 1 < node.panels.size() && input.mouse.x > node.tabs[i + 1].center().x) {
        std::swap(node.panels[i], node.panels[i + 1]);
        ++i;
    }
    node.current = i;
}

Rect DockSpace::tabRect(const Node &node, size_t i) const
{
    Rect r = node.tabs[i];
    if (pressedTab_ && &node == pressedNode_ && node.panels[i] == pressedTab_)
        r.x = std::clamp(tabX_, node.header.x, std::max(node.header.x, node.popOut.x - r.w));
    return r;
}

// Drawing

void DockSpace::draw(DrawList &list, const Theme &theme) const
{
    drawNode(*root_, list, theme);
    drawTargets(list, theme);
}

void DockSpace::drawNode(const Node &node, DrawList &list, const Theme &theme) const
{
    if (node.isSplit()) {
        drawNode(*node.first, list, theme);
        drawNode(*node.second, list, theme);
        // The handle: a short groove across the gap, raised into a grip when hovered or dragged
        const bool row = node.direction == Direction::Row;
        const Vec2 c = node.handle.center();
        const float length = 44.0f, thickness = 6.0f;
        const Rect grip = row ? Rect{c.x - thickness * 0.5f, c.y - length * 0.5f, thickness, length}
                              : Rect{c.x - length * 0.5f, c.y - thickness * 0.5f, length, thickness};
        const bool active = dragging_ == &node || hoveredHandle_ == &node;
        if (active)
            list.add(theme.surface(grip.inset(-1.0f), Shape::Pill, Elevation::Raised, 0.4f, Finish::Convex));
        else
            list.add(theme.surface(grip, Shape::Pill, Elevation::Inset, 0.3f));
        if (dragging_ == &node)
            list.add(theme.accent(grip.inset(2.0f), Shape::Pill, 0.8f));
        return;
    }

    if (node.panels.empty()) {
        // An empty grid: a well to drop into
        list.add(theme.surface(node.rect.inset(theme.padding() * 0.5f), Shape::Rounded, Elevation::Inset, 0.5f));
        list.add(theme.text("Drag a panel here to dock it", node.rect, theme.fontSize(TextStyle::Body), TextAlign::Center,
                            theme.textColor(TextTone::Muted)));
        return;
    }
    // Too small to draw while a new split opens
    if (node.rect.w < 24.0f || node.rect.h < 24.0f)
        return;

    list.add(theme.surface(node.rect, Shape::Rounded, Elevation::Raised, theme.depthScale(Depth::Deep)));

    list.pushClip(node.rect);
    // The tabs, in one inset track with the current one lifted out of it
    if (!node.tabs.empty()) {
        const Rect &a = node.tabs.front(), &b = node.tabs.back();
        list.add(theme.surface({a.x, a.y, b.x + b.w - a.x, a.h}, Shape::Pill, Elevation::Inset, 0.6f));
        list.add(theme.surface(tabRect(node, node.current).inset(2.0f), Shape::Pill, Elevation::Raised, 0.5f, Finish::Convex));
    }
    for (size_t i = 0; i < node.tabs.size(); ++i) {
        const TextTone tone = i == node.current                       ? TextTone::Accent
                              : node.hoveredTab == static_cast<int>(i) ? TextTone::Primary
                                                                       : TextTone::Secondary;
        Rect label = tabRect(node, i);
        if (i < node.closes.size() && node.closes[i].w > 0.0f) {
            // The title beside a small close cross
            const Rect &c = node.closes[i];
            label.w -= c.w + 6.0f;
            const Rect cross{c.x + (tabRect(node, i).x - node.tabs[i].x), c.y, c.w, c.h};
            drawIcon(list, theme, Icon::Close, cross, node.hoveredTab == static_cast<int>(i) ? TextTone::Primary : TextTone::Muted);
        }
        list.add(theme.text(node.panels[i]->title(), label, theme.fontSize(Size::Small), TextAlign::Center, theme.textColor(tone)));
    }
    headerButton(list, theme, node.popOut, node.popOutClick, Icon::PopOut);

    const DockPanel &panel = *node.panels[node.current];
    panel.header().draw(list, theme);
    panel.draw(list, theme);
    list.popClip();
}

void DockSpace::drawTargets(DrawList &list, const Theme &theme) const
{
    // Where a tab goes in a strip: a bar between the tabs
    if (target_.onTab)
        list.add(theme.accent(target_.marker, Shape::Pill, 0.9f));

    // Where the panel will go
    if (previewAlpha_ > 0.01f) {
        const Rect &p = previewShown_;
        const float a = previewAlpha_;
        list.add(theme.accent(p, Shape::Rounded, 0.16f * a));
        const float t = 3.0f;
        list.add(theme.accent({p.x + 10.0f, p.y, p.w - 20.0f, t}, Shape::Pill, 0.7f * a));
        list.add(theme.accent({p.x + 10.0f, p.y + p.h - t, p.w - 20.0f, t}, Shape::Pill, 0.7f * a));
        list.add(theme.accent({p.x, p.y + 10.0f, t, p.h - 20.0f}, Shape::Pill, 0.7f * a));
        list.add(theme.accent({p.x + p.w - t, p.y + 10.0f, t, p.h - 20.0f}, Shape::Pill, 0.7f * a));
    }

    // The dial: a raised disc that grows out of the card, with a dimple in the middle for a tab
    // and four around it for the sides
    if (dialShown_ > 0.01f) {
        const float e = dialShown_;
        const float r = dialRadius() * (0.6f + 0.4f * e);
        const Vec2 c = dialCenter_;
        list.add(theme.surface(circle(c, r), Shape::Circle, Elevation::Raised, e, Finish::Convex));
        list.add(theme.surface(circle(c, r * 0.47f), Shape::Circle, Elevation::Inset, 0.25f * e));
        const float ring = r * 0.69f, small = r * 0.2f;
        const Vec2 at[] = {{c.x - ring, c.y}, {c.x + ring, c.y}, {c.x, c.y - ring}, {c.x, c.y + ring}};
        for (int i = 0; i < 4; ++i)
            drawTarget(list, theme, at[i], small, kSides[i], hot_[static_cast<size_t>(kSides[i])], e);
        drawTarget(list, theme, c, r * 0.24f, DockSide::Center, hot_[static_cast<size_t>(DockSide::Center)], e);
    }

    // The grid's edges: a knob at the middle of each
    if (edgesShown_ > 0.01f) {
        const std::array<Vec2, 4> knobs = edgeKnobs();
        for (size_t i = 0; i < 4; ++i) {
            const float r = kKnob * 0.5f * (0.6f + 0.4f * edgesShown_);
            list.add(theme.surface(circle(knobs[i], r + 5.0f), Shape::Circle, Elevation::Raised, edgesShown_, Finish::Convex));
            drawTarget(list, theme, knobs[i], r * 0.8f, kSides[i], edgeHot_[i], edgesShown_);
        }
    }
}

// Drop targets

float DockSpace::dialRadius() const
{
    return 62.0f;
}

std::array<Vec2, 4> DockSpace::edgeKnobs() const
{
    const Rect &r = rect_;
    const Vec2 c = r.center();
    const float in = kKnobInset + kKnob * 0.5f;
    return {Vec2{r.x + in, c.y}, Vec2{r.x + r.w - in, c.y}, Vec2{c.x, r.y + in}, Vec2{c.x, r.y + r.h - in}};
}

Rect DockSpace::previewFor(const Node &node, DockSide side, float share) const
{
    return (side == DockSide::Center ? node.rect : sideOf(node.rect, side, share)).inset(6.0f);
}

DockSpace::Node *DockSpace::leafAt(Node &node, Vec2 p) const
{
    if (!node.rect.contains(p))
        return nullptr;
    if (!node.isSplit())
        return &node;
    if (Node *n = leafAt(*node.first, p))
        return n;
    return leafAt(*node.second, p);
}

DockSpace::Target DockSpace::targetAt(Vec2 p) const
{
    Target t;
    if (!rect_.contains(p))
        return t;
    Node &root = *root_;
    // An empty grid takes it anywhere
    if (!root.isSplit() && root.panels.empty()) {
        t.node = &root;
        t.preview = rect_.inset(6.0f);
        return t;
    }
    if (root.isSplit()) {
        const std::array<Vec2, 4> knobs = edgeKnobs();
        for (size_t i = 0; i < 4; ++i) {
            if (std::hypot(p.x - knobs[i].x, p.y - knobs[i].y) <= kKnob * 0.5f + 6.0f) {
                t.node = &root;
                t.side = kSides[i];
                t.preview = previewFor(root, t.side, 0.35f);
                return t;
            }
        }
    }
    // The dial on the card under the mouse: its middle, or the side the mouse leans toward
    Node *leaf = leafAt(root, p);
    if (!leaf)
        return t;
    // Its tab strip: in as a tab, before or after the tab the mouse is nearest
    const Rect &strip = leaf->header;
    if (!leaf->panels.empty() && leaf->tabs.size() == leaf->panels.size() && p.y >= strip.y - kStripSlack &&
        p.y <= strip.y + strip.h + kStripSlack) {
        const std::vector<Rect> &tabs = leaf->tabs;
        const size_t n = tabs.size();
        size_t k = 0;
        while (k < n && p.x > tabs[k].center().x)
            ++k;
        float x = 0.0f;
        if (k == 0)
            x = tabs[0].x - 3.0f;
        else if (k == n)
            x = tabs[n - 1].x + tabs[n - 1].w + 3.0f;
        else
            x = (tabs[k - 1].x + tabs[k - 1].w + tabs[k].x) * 0.5f;
        t.node = leaf;
        t.onTab = true;
        t.after = k == n;
        t.beside = leaf->panels[k < n ? k : n - 1];
        t.marker = {x - 1.5f, strip.y + 3.0f, 3.0f, strip.h - 6.0f};
        t.preview = previewFor(*leaf, DockSide::Center, 0.5f);
        return t;
    }
    const Vec2 c = leaf->rect.center();
    const float dx = p.x - c.x, dy = p.y - c.y;
    const float d = std::hypot(dx, dy);
    if (d > dialRadius())
        return t;
    t.node = leaf;
    if (d < dialRadius() * 0.36f)
        t.side = DockSide::Center;
    else if (std::abs(dx) > std::abs(dy))
        t.side = dx < 0.0f ? DockSide::Left : DockSide::Right;
    else
        t.side = dy < 0.0f ? DockSide::Top : DockSide::Bottom;
    t.preview = previewFor(*leaf, t.side, 0.5f);
    return t;
}

bool DockSpace::dragWindow(Vec2 screen, bool allowed)
{
    mouse_ = context_.fromScreen(screen);
    const bool over = rect_.contains(mouse_);
    showTargets_ = allowed && over;
    target_ = showTargets_ ? targetAt(mouse_) : Target{};
    return over;
}

void DockSpace::dropWindow(DockHost &host, Vec2 screen, bool allowed)
{
    dragWindow(screen, allowed);
    const Target t = target_;
    showTargets_ = false;
    target_ = {};
    DockPanel *panel = host.panel();
    if (!t.node || !panel)
        return;
    if (t.onTab) {
        // It flies into the card, then goes in as a tab
        DockPanel *beside = t.beside;
        const bool after = t.after;
        host.land(toScreen(t.preview), [this, panel, beside, after] { finishTab(*panel, *beside, after); });
        return;
    }
    // It flies into place, then docks
    DockPanel *anchor = t.node->isSplit() || t.node->panels.empty() ? nullptr : t.node->panels[t.node->current];
    const DockSide side = t.side;
    const float share = side == DockSide::Center ? 0.5f : (anchor ? 0.5f : 0.35f);
    host.land(toScreen(t.preview), [this, panel, anchor, side, share] { finishDock(*panel, anchor, side, share); });
}

// DockHost

DockHost::DockHost(DockSpace &space, DockPanel &panel, engine::Window &window, TitleBar &titleBar, Rect from, Rect to,
                   bool dragging, Vec2 grab)
    : space_(space), panel_(&panel), window_(window), titleBar_(titleBar), shown_{from.x, from.y, to.w, to.h}, target_(to),
      dragging_(dragging), grab_(grab)
{
    // Resizing a window rebuilds its swapchain, which is too slow to do every frame of an
    // animation: the window opens at its final size and only slides and fades
    grow_ = 1.0f;
    alignSelf_ = Align::Stretch;
}

void DockHost::land(Rect target, std::function<void()> then)
{
    landing_ = true;
    dragging_ = false;
    morphing_ = false;
    landingTime_ = 0.0f;
    // Flies to the middle of where it is docking, at the size it has
    target_ = {target.x + (target.w - shown_.w) * 0.5f, target.y + (target.h - shown_.h) * 0.5f, shown_.w, shown_.h};
    landed_ = std::move(then);
}

void DockHost::resizeTo(float width, float height)
{
    morphing_ = false;
    shown_.w = target_.w = std::max(1.0f, width);
    shown_.h = target_.h = std::max(1.0f, height);
    apply();
}

void DockHost::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    if (panel_)
        panel_->arrange(rect, theme);
}

void DockHost::apply()
{
    const int x = static_cast<int>(std::lround(shown_.x)), y = static_cast<int>(std::lround(shown_.y));
    const int w = std::max(1, static_cast<int>(std::lround(shown_.w)));
    const int h = std::max(1, static_cast<int>(std::lround(shown_.h)));
    if (x != appliedX_ || y != appliedY_) {
        window_.setPosition(x, y);
        appliedX_ = x;
        appliedY_ = y;
    }
    if (w != appliedW_ || h != appliedH_) {
        window_.setSize(w, h);
        appliedW_ = w;
        appliedH_ = h;
    }
    if (std::abs(opacity_ - appliedOpacity_) > 0.004f) {
        window_.setOpacity(opacity_);
        appliedOpacity_ = opacity_;
    }
}

void DockHost::handleInput(const Input &input)
{
    ++frames_;
    int cx = 0, cy = 0;
    const bool cursor = engine::Window::desktopCursor(cx, cy);
    const Vec2 mouse{static_cast<float>(cx), static_cast<float>(cy)};
    const bool allowed = (input.mods & engine::ModCtrl) == 0;

    // Dragged by the title bar's empty parts. The mouse is read on the desktop, so the window
    // can move under it.
    if (!dragging_ && !landing_ && cursor && input.pressed && titleBar_.isDragArea(input.mouse)) {
        dragging_ = true;
        morphing_ = false;
        grab_ = {mouse.x - shown_.x, mouse.y - shown_.y};
    }
    if (!dragging_ && !landing_ && !morphing_) {
        // At rest the window is wherever the user put it (resized at its edges, maximised)
        int x = 0, y = 0, w = 0, h = 0;
        window_.position(x, y);
        window_.size(w, h);
        shown_ = target_ = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
        appliedX_ = x;
        appliedY_ = y;
        appliedW_ = w;
        appliedH_ = h;
    }

    if (dragging_) {
        if (!cursor || !engine::Window::primaryButtonHeld()) {
            dragging_ = false;
            opacityTarget_ = 1.0f;
            space_.dropWindow(*this, mouse, allowed);
        } else {
            target_.x = mouse.x - grab_.x;
            target_.y = mouse.y - grab_.y;
            // See-through over the grid, so the targets under it show
            opacityTarget_ = space_.dragWindow(mouse, allowed) && allowed ? 0.5f : 1.0f;
            if (!morphing_) {
                shown_.x = target_.x;
                shown_.y = target_.y;
            }
        }
    }

    if (landing_) {
        landingTime_ += input.dt;
        shown_ = lerp(shown_, target_, input.ease(16.0f));
        opacity_ += (0.0f - opacity_) * input.ease(9.0f);
        if (landingTime_ >= kLandSeconds) {
            landing_ = false;
            opacity_ = opacityTarget_ = 0.0f; // gone until its window closes
            apply();
            auto landed = std::move(landed_);
            landed_ = nullptr;
            if (landed)
                landed();
            return;
        }
    } else {
        if (morphing_) {
            shown_ = lerp(shown_, target_, input.ease(14.0f));
            if (distance(shown_, target_) < 1.0f) {
                shown_ = target_;
                morphing_ = false;
            }
        }
        opacity_ += (opacityTarget_ - opacity_) * input.ease(14.0f);
    }
    apply();
    // Placed and drawn while hidden, then shown
    if (frames_ == 2)
        window_.show();

    if (panel_)
        panel_->handleInput(busy() ? input.occluded() : input);
}

void DockHost::draw(DrawList &list, const Theme &theme) const
{
    if (panel_)
        panel_->draw(list, theme);
}

}
