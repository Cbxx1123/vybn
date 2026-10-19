#include "StepMenus.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "audio/SampleSet.h"

namespace daw {

using namespace ui;

namespace {

// A fresh seed for each random fill: the device's entropy, and a counter in case it gives the same
// numbers twice (some MinGW builds do)
uint32_t freshSeed()
{
    static std::random_device device;
    static uint32_t counter = 0;
    return device() + ++counter * 2654435761u;
}

// The Euclidean dialog's values, shared by its steppers' callbacks and its preview
struct EuclidState {
    int count = 16;
    int pulses = 4;
    int rotation = 0;
};

constexpr float kDotCell = 14.0f;
constexpr float kDotRow = 22.0f;
constexpr float kDot = 10.0f;
constexpr float kPreviewWidth = 448.0f; // long grids (1/32 over a few bars) shrink their dots to fit

// The pattern as a row of dots: a raised small circle for each step, the pulses accented
class EuclidPreview : public Component {
public:
    explicit EuclidPreview(std::shared_ptr<EuclidState> state) : state_(std::move(state)) {}

    Vec2 measure(const Theme &) const override
    {
        return {static_cast<float>(state_->count) * cell(), kDotRow};
    }

    void draw(DrawList &list, const Theme &theme) const override
    {
        const std::vector<bool> pattern = euclidean(state_->pulses, state_->count, state_->rotation);
        const float w = cell();
        const float size = std::min(kDot, w * 0.72f);
        for (int i = 0; i < state_->count; ++i) {
            const Rect r{rect_.x + static_cast<float>(i) * w, rect_.y, w, kDotRow};
            const Rect dot{r.x + (w - size) * 0.5f, r.y + (kDotRow - size) * 0.5f, size, size};
            list.add(theme.surface(dot, Shape::Circle, Elevation::Raised, 0.5f, Finish::Convex));
            if (static_cast<size_t>(i) < pattern.size() && pattern[static_cast<size_t>(i)])
                list.add(theme.accent(dot.inset(size * 0.25f), Shape::Circle, 0.9f));
        }
    }

private:
    float cell() const { return std::min(kDotCell, kPreviewWidth / static_cast<float>(std::max(1, state_->count))); }

    std::shared_ptr<EuclidState> state_;
};

// The menu items for a list of keys
std::vector<MenuItem> keyItems(StepSequencerView &view, const std::vector<int> &keys)
{
    auto shown = [&view](int key) {
        const std::vector<int> rows = view.rows();
        return std::find(rows.begin(), rows.end(), key) != rows.end();
    };
    std::vector<MenuItem> items;
    for (int key : keys) {
        const std::string label = drumName(key) + " (" + audio::keyName(key) + ")";
        items.push_back(MenuItem::toggle(label, [shown, key] { return shown(key); }, [&view, shown, key] {
            if (shown(key))
                view.selectRow(key);
            else
                view.addRow(key);
        }));
    }
    return items;
}

std::vector<int> range(int first, int last)
{
    std::vector<int> keys;
    for (int key = first; key <= last; ++key)
        keys.push_back(key);
    return keys;
}

}

// --- The row's menu -------------------------------------------------------------------------

std::vector<MenuItem> stepRowMenu(ui::Context &context, StepSequencerView &view, int key)
{
    auto random = [&view, key](const char *label, double density) {
        return MenuItem::item(label, [&view, key, density] { view.randomRow(key, density, freshSeed()); });
    };
    auto shift = [&view, key](const char *label, int steps) {
        return MenuItem::item(label, [&view, key, steps] { view.shiftRow(key, steps); });
    };
    return {
        MenuItem::item("Select row", [&view, key] { view.selectRow(key); }),
        MenuItem::sub("Fill every", {
            MenuItem::item("Every step", [&view, key] { view.fillRow(key, 1, 0); }),
            MenuItem::item("Every 2 steps", [&view, key] { view.fillRow(key, 2, 0); }),
            MenuItem::item("Every 3 steps", [&view, key] { view.fillRow(key, 3, 0); }),
            MenuItem::item("Every 4 steps", [&view, key] { view.fillRow(key, 4, 0); }),
            MenuItem::item("Every 8 steps", [&view, key] { view.fillRow(key, 8, 0); }),
        }),
        MenuItem::item("Euclidean...", [&context, &view, key] { openEuclidDialog(context, view, key); }),
        MenuItem::sub("Random", {
            random("Sparse", 0.25),
            random("Medium", 0.5),
            random("Dense", 0.75),
        }),
        MenuItem::divider(),
        shift("Shift left", -1),
        shift("Shift right", 1),
        MenuItem::divider(),
        MenuItem::item("Copy row", [&view, key] { view.copyRow(key); }),
        MenuItem::item("Paste row", [&view, key] { view.pasteRow(key); })
            .when([&view] { return view.canPasteRow(); }),
        MenuItem::toggle("Mute row", [&view, key] { return view.rowMuted(key); }, [&view, key] { view.toggleRowMute(key); }),
        MenuItem::divider(),
        MenuItem::item("Clear row", [&view, key] { view.clearRow(key); }),
        MenuItem::item("Remove row", [&view, key] { view.removeRow(key); }),
    };
}

// --- Adding rows ----------------------------------------------------------------------------

std::vector<MenuItem> stepAddRowMenu(StepSequencerView &view)
{
    const std::vector<int> kicks = range(35, 40);
    const std::vector<int> hats{42, 44, 46, 49, 51, 52, 53, 55, 57, 59};
    const std::vector<int> toms{41, 43, 45, 47, 48, 50};
    // Everything else in the GM percussion range
    std::vector<int> percussion;
    for (int key = 35; key <= 81; ++key) {
        const bool listed = std::find(kicks.begin(), kicks.end(), key) != kicks.end()
                            || std::find(hats.begin(), hats.end(), key) != hats.end()
                            || std::find(toms.begin(), toms.end(), key) != toms.end();
        if (!listed)
            percussion.push_back(key);
    }
    return {
        MenuItem::sub("Kicks and snares", keyItems(view, kicks)),
        MenuItem::sub("Hats and cymbals", keyItems(view, hats)),
        MenuItem::sub("Toms", keyItems(view, toms)),
        MenuItem::sub("Percussion", keyItems(view, percussion)),
    };
}

// --- Step sizes -----------------------------------------------------------------------------

std::vector<MenuItem> stepSizeMenu(StepSequencerView &view)
{
    std::vector<MenuItem> items;
    const std::vector<StepChoice> &choices = stepChoices();
    for (size_t i = 0; i < choices.size(); ++i) {
        const int index = static_cast<int>(i);
        items.push_back(MenuItem::toggle(choices[i].name, [&view, index] { return view.stepChoice() == index; },
                                         [&view, index] { view.setStepChoice(index); }));
    }
    return items;
}

// --- The Euclidean dialog -------------------------------------------------------------------

void openEuclidDialog(ui::Context &context, StepSequencerView &view, int key)
{
    auto state = std::make_shared<EuclidState>();
    state->count = std::max(1, view.grid().count);
    state->pulses = std::min(4, state->count);
    state->rotation = 0;
    const int count = state->count;

    Dialog &dialog = Dialog::open(context, "Euclidean rhythm",
                                  "Spread pulses evenly over the " + std::to_string(count) + " steps of " + drumName(key));
    Container &body = dialog.body();
    auto labelled = [&body](const char *label) -> Container & {
        auto &r = body.add<Container>(Direction::Row);
        r.setAlign(Align::Center);
        r.add<Label>(label, TextStyle::Caption, TextTone::Secondary).setMinSize({76.0f, 0.0f});
        return r;
    };

    labelled("Pulses").add<Stepper>(static_cast<float>(state->pulses), 0.0f, static_cast<float>(count), 1.0f,
                                    [state](float value) { state->pulses = static_cast<int>(value); });
    labelled("Rotation").add<Stepper>(static_cast<float>(state->rotation), 0.0f, static_cast<float>(count - 1), 1.0f,
                                      [state](float value) { state->rotation = static_cast<int>(value); });
    body.add<EuclidPreview>(state);

    dialog.addButton("Cancel");
    dialog.addButton("Apply", [&view, key, state] { view.euclidRow(key, state->pulses, state->rotation); }, true);
}

}
