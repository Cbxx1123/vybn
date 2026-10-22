#include "daw/views/ShortcutsEditor.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "daw/widgets/Widgets.h"

namespace daw {

using namespace ui;

namespace {

std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// What the open editor shares between its rows and its key hook
struct EditorState {
    EditorState(Context &c, Keymap &k, std::function<void()> changed)
        : context(c), keymap(k), onChange(std::move(changed))
    {
    }

    Context &context;
    Keymap &keymap;
    std::function<void()> onChange;
    std::string capturing; // the action waiting for keys, or empty
    int index = -1;        // the chord being replaced, or -1 to add one
    std::string message;
    std::string filter;
    uint64_t serial = 0; // goes up whenever the list should be rebuilt
    env::Connection keys, text;

    void startCapture(const std::string &id, int chord)
    {
        capturing = id;
        index = chord;
        const ActionInfo *info = keymap.find(id);
        message = "Press the keys for \"" + (info ? info->label : id) + "\". Escape cancels.";
        context.signals().setFocus(nullptr); // so the search field doesn't take the keys
        ++serial;
    }

    void stopCapture(std::string why)
    {
        capturing.clear();
        index = -1;
        message = std::move(why);
        ++serial;
    }

    // The next key press while capturing, ahead of everything else (Priority::Capture)
    bool onKey(const KeyEvent &e)
    {
        if (capturing.empty())
            return false;
        if (!e.down || engine::isModifier(e.key))
            return true;
        if (e.key == Key::Escape && e.mods == 0) {
            stopCapture({});
            return true;
        }
        const Chord chord = e.chord();
        const std::string id = capturing;
        const std::vector<std::string> taken = keymap.assign(id, chord, index);
        const ActionInfo *info = keymap.find(id);
        std::string why = chord.toString() + " now runs \"" + (info ? info->label : id) + "\"";
        for (size_t i = 0; i < taken.size(); ++i)
            why += (i == 0 ? "; it was taken from \"" : "\", \"") + taken[i];
        why += taken.empty() ? "." : "\".";
        stopCapture(why);
        if (onChange)
            onChange();
        return true;
    }
};

// The list of actions, rebuilt (after the frame) whenever the keymap, the capture or the filter
// changes
class ShortcutList : public Container {
public:
    explicit ShortcutList(std::shared_ptr<EditorState> state) : Container(Direction::Column), state_(std::move(state))
    {
        setAlign(Align::Stretch);
    }

    void handleInput(const Input &input) override
    {
        const std::string stamp = std::to_string(state_->keymap.version()) + "/" + std::to_string(state_->serial) +
                                  "/" + state_->filter;
        if (stamp != built_ && !pending_) {
            pending_ = true;
            state_->context.post([this, stamp] {
                rebuild();
                built_ = stamp;
                pending_ = false;
            });
        }
        Container::handleInput(input);
    }

private:
    void rebuild()
    {
        clear();
        EditorState &st = *state_;
        const std::string filter = lower(st.filter);
        std::string category;
        size_t shown = 0;
        for (const ActionInfo &info : st.keymap.actions()) {
            const auto &chords = st.keymap.chords(info.id);
            std::string keysText;
            for (const Chord &c : chords)
                keysText += " " + c.toString();
            if (!filter.empty() && lower(info.label + " " + info.category + keysText + " " + info.id).find(filter) ==
                                       std::string::npos)
                continue;
            if (info.category != category) {
                category = info.category;
                add<Label>(category, TextStyle::Subtitle).setMinSize({0.0f, 30.0f});
            }
            ++shown;
            auto &line = add<Container>(Direction::Row);
            line.setAlign(Align::Center);
            line.add<Label>(info.label, TextStyle::Body).setMinSize({230.0f, 0.0f});
            line.add<Spacer>();
            const std::string id = info.id;
            const bool capturingThis = st.capturing == id;
            for (size_t i = 0; i < chords.size(); ++i) {
                const bool waiting = capturingThis && st.index == static_cast<int>(i);
                auto weak = std::weak_ptr<EditorState>(state_);
                line.add<Button>([weak, id, i] {
                        if (auto s = weak.lock())
                            s->startCapture(id, static_cast<int>(i));
                    })
                    .setLabel(waiting ? "Press keys..." : chords[i].toString())
                    .setVariant(waiting ? ButtonVariant::Accent : ButtonVariant::Inset)
                    .setSize(Size::Small);
                const Chord chord = chords[i];
                line.add<Button>([weak, id, chord] {
                        if (auto s = weak.lock()) {
                            s->keymap.removeChord(id, chord);
                            s->stopCapture(chord.toString() + " removed.");
                            if (s->onChange)
                                s->onChange();
                        }
                    })
                    .setIcon(Icon::Close)
                    .setVariant(ButtonVariant::Flat)
                    .setShape(Shape::Circle)
                    .setSize(Size::Small);
            }
            const bool adding = capturingThis && st.index < 0;
            auto weak = std::weak_ptr<EditorState>(state_);
            auto &plus = line.add<Button>([weak, id] {
                if (auto s = weak.lock())
                    s->startCapture(id, -1);
            });
            if (adding)
                plus.setLabel("Press keys...").setVariant(ButtonVariant::Accent);
            else
                plus.setIcon(Icon::Plus).setShape(Shape::Circle);
            plus.setSize(Size::Small);
            line.add<Button>([weak, id] {
                    if (auto s = weak.lock()) {
                        s->keymap.reset(id);
                        s->stopCapture("Back to the default keys.");
                        if (s->onChange)
                            s->onChange();
                    }
                })
                .setLabel("Reset")
                .setVariant(ButtonVariant::Flat)
                .setSize(Size::Small)
                .setEnabled(!st.keymap.isDefault(id));
        }
        if (shown == 0)
            add<Label>("No shortcuts match \"" + st.filter + "\"", TextStyle::Body, TextTone::Muted);
    }

    std::shared_ptr<EditorState> state_;
    std::string built_;
    bool pending_ = false;
};

}

void openShortcutsEditor(Context &context, Keymap &keymap, std::function<void()> onChange)
{
    auto state = std::make_shared<EditorState>(context, keymap, std::move(onChange));
    // The capture takes the next key from everything, the dialog's own sink included
    EditorState *raw = state.get();
    state->keys = context.signals().key.connect([raw](const KeyEvent &e) { return raw->onKey(e); }, Priority::Capture);
    state->text = context.signals().text.connect([raw](const TextEvent &) { return !raw->capturing.empty(); },
                                                 Priority::Capture);

    Dialog &dialog = Dialog::open(context, "Keyboard shortcuts",
                                  "Click a shortcut to change it, then press the new keys. Changes are saved as you make them.");
    dialog.setWidth(680.0f);
    Container &body = dialog.body();

    auto &search = body.add<TextField>("Search shortcuts");
    search.setSize(Size::Small);
    search.onChange([state](const std::string &text) {
        state->filter = text;
        ++state->serial;
    });

    auto &scroll = body.add<ScrollView>();
    scroll.setViewHeight(420.0f);
    scroll.content().setAlign(Align::Stretch);
    scroll.content().add<ShortcutList>(state);

    auto &footer = body.add<Container>(Direction::Row);
    footer.setAlign(Align::Center);
    footer.add<LiveLabel>([state] { return state->message; }, TextStyle::Caption, TextTone::Secondary).setGrow(1.0f);
    footer.add<Button>([state] {
        state->keymap.resetAll();
        state->stopCapture("Every shortcut is back on its default keys.");
        if (state->onChange)
            state->onChange();
    }).setLabel("Reset all").setVariant(ButtonVariant::Flat).setSize(Size::Small);

    dialog.addButton("Done", {}, true);
}

}
