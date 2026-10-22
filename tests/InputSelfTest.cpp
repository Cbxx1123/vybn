#include "InputSelfTest.h"
#include "Harness.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "daw/app/Actions.h"
#include "env/Environment.h"
#include "env/EventBus.h"
#include "env/Signal.h"
#include "ui/UI.h"

namespace tests {

namespace {

using engine::Key;
using engine::KeyEvent;
using engine::ModCtrl;
using engine::ModShift;
using engine::TextEvent;
using ui::Chord;

KeyEvent press(Key key, uint8_t mods = 0, bool repeat = false)
{
    KeyEvent e;
    e.key = key;
    e.mods = mods;
    e.repeat = repeat;
    return e;
}

void type(ui::InputSignals &hub, const std::string &text)
{
    for (char c : text) {
        TextEvent e;
        e.codepoint = static_cast<unsigned char>(c);
        hub.dispatch(e);
    }
}

double secondsSince(std::chrono::steady_clock::time_point since)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

// Benchmarks (VIBIN_TEST_BENCH): what dispatching costs through a Signal, the event bus and the
// environment's posted actions
void benchDispatch()
{
    using Clock = std::chrono::steady_clock;
    volatile int seen = 0;
    {
        // 32 hooks, none handling it: every key a window's components pass on goes this far
        env::Signal<int> signal;
        std::vector<env::Connection> hooks;
        for (int i = 0; i < 32; ++i)
            hooks.push_back(signal.connect([&seen](int v) { seen = seen + v; return false; }, i % 3));
        constexpr int kEmits = 200000;
        const auto begin = Clock::now();
        for (int i = 0; i < kEmits; ++i)
            signal.emit(1);
        std::printf("INFO dispatch signal: %.0f ns an emit through 32 hooks\n", secondsSince(begin) / kEmits * 1e9);
    }
    struct Tick {
        int value;
    };
    {
        env::EventBus bus;
        for (int i = 0; i < 8; ++i)
            bus.subscribe<Tick>([&seen](const Tick &t) { seen = seen + t.value; });
        constexpr int kEvents = 200000;
        auto begin = Clock::now();
        for (int i = 0; i < kEvents; ++i)
            bus.publish(Tick{1});
        const double publishNs = secondsSince(begin) / kEvents * 1e9;
        begin = Clock::now();
        for (int round = 0; round < kEvents / 100; ++round) {
            for (int i = 0; i < 100; ++i)
                bus.post(Tick{1});
            bus.dispatch();
        }
        const double postNs = secondsSince(begin) / kEvents * 1e9;
        std::printf("INFO dispatch bus: %.0f ns a publish to 8 subscribers, %.0f ns a post and its dispatch\n", publishNs, postNs);
    }
    {
        // The environment's loop with a daemon posting 16 actions a tick
        struct Poster : env::Daemon {
            explicit Poster(env::Environment &) : env::Daemon("Poster") {}
            int ticks = 0;
            int ran = 0;
            void tick(env::Environment &env) override
            {
                if (++ticks >= 20000) {
                    env.requestStop();
                    return;
                }
                for (int i = 0; i < 16; ++i)
                    env.post([this] { ++ran; });
            }
        };
        env::Environment environment;
        auto &poster = environment.add<Poster>();
        const auto begin = Clock::now();
        environment.run();
        std::printf("INFO dispatch environment: %.0f ns a loop iteration with 16 posted actions (%d ran)\n",
                    secondsSince(begin) / poster.ticks * 1e9, poster.ran);
    }
}

// A frame in which the component gets input, so its hooks are live
void frame(ui::InputSignals &hub, ui::Component &c)
{
    hub.beginFrame();
    ui::Input input;
    input.signals = &hub;
    input.mouse = {-1e9f, -1e9f};
    c.handleInput(input);
}

}

int runInputSelfTest()
{
    std::printf("Input self-test: signals, keymap and text editing, no window\n\n");
    Checker c;

    // Signals: priority, newest first, consumption, changes during an emit
    {
        env::Signal<int> signal;
        std::string order;
        auto low = signal.connect([&](int) { order += "L"; return false; }, 0);
        auto high = signal.connect([&](int) { order += "H"; return false; }, 10);
        auto newer = signal.connect([&](int) { order += "N"; return false; }, 0);
        signal.emit(1);
        c.check(order == "HNL", "Hooks run by priority, newest first", order);

        order.clear();
        auto stop = signal.connect([&](int) { order += "S"; return true; }, 5);
        const bool handled = signal.emit(1);
        c.check(handled && order == "HS", "A handled event goes no further", order);

        order.clear();
        stop.disconnect();
        env::Connection added;
        auto selfRemoving = std::make_shared<env::Connection>();
        *selfRemoving = signal.connect([&, selfRemoving](int) {
            order += "R";
            selfRemoving->disconnect();
            added = signal.connect([&](int) { order += "A"; return false; }, 20);
            return false;
        }, 15);
        signal.emit(1);
        c.check(order == "RHNL", "Hooks change safely while it runs", order);
        order.clear();
        signal.emit(1);
        c.check(order == "AHNL", "Changes apply from the next emit", order);
        {
            auto scoped = signal.connect([&](int) { order += "X"; return false; }, 50);
        }
        order.clear();
        signal.emit(1);
        c.check(order == "AHNL", "A destroyed connection unhooks", order);
    }

    // The event bus: changes during a publish, posted events in order
    {
        struct Note {
            char c;
        };
        env::EventBus bus;
        std::string order;
        bus.subscribe<Note>([&](const Note &n) { order += std::string("1") + n.c; });
        env::SubscriptionId self = 0, added = 0;
        self = bus.subscribe<Note>([&](const Note &n) {
            order += std::string("2") + n.c;
            bus.unsubscribe(self);
            added = bus.subscribe<Note>([&](const Note &m) { order += std::string("3") + m.c; });
        });
        bus.subscribe<Note>([&](const Note &n) { order += std::string("4") + n.c; });
        bus.publish(Note{'a'});
        c.check(order == "1a2a4a", "Bus: handlers change safely during a publish", order);
        order.clear();
        bus.publish(Note{'b'});
        c.check(order == "1b4b3b", "Bus: the changes apply from the next event", order);
        order.clear();
        bus.unsubscribe(added);
        int wakes = 0;
        bus.setWakeHook([&wakes] { ++wakes; });
        bus.post(Note{'c'});
        bus.post(Note{'d'});
        const bool early = order.empty();
        bus.dispatch();
        bus.setWakeHook(nullptr);
        c.check(early && order == "1c4c1d4d" && wakes == 2, "Bus: posted events wait for dispatch, in order, waking each time",
                order);
    }

    // Chords
    {
        auto parsed = Chord::parse("ctrl+shift+s");
        c.check(parsed && parsed->toString() == "Ctrl+Shift+S", "Chords parse and print",
                parsed ? parsed->toString() : "refused");
        auto minus = Chord::parse("Ctrl+-");
        auto keypad = Chord::parse("Alt+Keypad +");
        c.check(minus && minus->key == Key::Minus && keypad && keypad->key == Key::KeypadAdd &&
                    keypad->mods == engine::ModAlt,
                "Punctuation and keypad chords parse");
        c.check(!Chord::parse("Ctrl++") && !Chord::parse("Shift") && !Chord::parse("Hyper+A") && !Chord::parse(""),
                "Bad chords are refused");
        c.check(Chord{Key::Space, 0}.toString() == "Space" && Chord::parse("Esc")->key == Key::Escape,
                "Key names and other spellings");
    }

    // The keymap
    {
        ui::Keymap keymap;
        keymap.define({"test.one", "One", "Test", {{Key::D, ModCtrl}}});
        keymap.define({"test.two", "Two", "Test", {{Key::E, ModCtrl}}});
        const auto taken = keymap.assign("test.two", {Key::D, ModCtrl});
        c.check(taken.size() == 1 && taken[0] == "One" && keymap.chords("test.one").empty() &&
                    keymap.chords("test.two").size() == 2,
                "Assigning a chord takes it from its owner");
        keymap.assign("test.two", {Key::F5, 0}, 0);
        c.check(keymap.shortcutText("test.two") == "F5" && keymap.chords("test.two").size() == 2,
                "Replacing one of an action's chords", keymap.shortcutText("test.two"));
        keymap.setOverrides({{"test.later", {{Key::L, ModCtrl}}}});
        keymap.define({"test.later", "Later", "Test", {}});
        c.check(keymap.shortcutText("test.later") == "Ctrl+L", "Saved keys wait for their action");

        const std::string text = daw::writeKeybinds(keymap);
        ui::Keymap fresh;
        fresh.define({"test.one", "One", "Test", {{Key::D, ModCtrl}}});
        fresh.define({"test.two", "Two", "Test", {{Key::E, ModCtrl}}});
        fresh.define({"test.later", "Later", "Test", {}});
        std::string error;
        const bool read = daw::readKeybinds(text, fresh, error);
        c.check(read && fresh.chords("test.one").empty() && fresh.chords("test.two") == keymap.chords("test.two") &&
                    fresh.shortcutText("test.later") == "Ctrl+L",
                "The keybinds file round-trips", error);
        ui::Keymap other;
        error.clear();
        const bool partly = daw::readKeybinds("[keys]\n\"test.x\" = [\"Ctrl+Nope\", \"F2\"]\n", other, error);
        c.check(!partly && error.find("Ctrl+Nope") != std::string::npos, "Unreadable keys are reported", error);
        c.check(other.overrides().size() == 1 && other.overrides()[0].second.size() == 1,
                "The readable ones still apply");
        fresh.resetAll();
        c.check(fresh.overrides().empty() && fresh.isDefault("test.two"), "Reset all");

        ui::Keymap app;
        daw::defineActions(app);
        std::vector<std::string> clashes;
        for (const ui::ActionInfo &info : app.actions()) {
            for (const Chord &chord : app.chords(info.id)) {
                if (app.actionsFor(chord).size() > 1)
                    clashes.push_back(chord.toString());
            }
        }
        c.check(clashes.empty(), "The DAW's default keys don't clash",
                std::to_string(app.actions().size()) + " actions" + (clashes.empty() ? "" : ", " + clashes.front()));
    }

    // Keys to actions
    ui::InputSignals hub;
    hub.keymap().define({"test.play", "Play", "Test", {{Key::Space, 0}}});
    hub.keymap().define({"test.save", "Save", "Test", {{Key::S, ModCtrl}}});
    hub.keymap().define({"test.nudge", "Nudge", "Test", {{Key::Right, 0}}, true});
    int plays = 0, saves = 0, nudges = 0;
    auto playHook = hub.onAction("test.play", [&](const ui::ActionEvent &) { ++plays; return true; });
    auto saveHook = hub.onAction("test.save", [&](const ui::ActionEvent &) { ++saves; return true; });
    auto nudgeHook = hub.onAction("test.nudge", [&](const ui::ActionEvent &) { ++nudges; return true; });
    hub.dispatch(press(Key::Space));
    hub.dispatch(press(Key::Space, 0, true));
    hub.dispatch(press(Key::Right));
    hub.dispatch(press(Key::Right, 0, true));
    c.check(plays == 1 && nudges == 2, "Keys run actions; only some repeat",
            std::to_string(plays) + " plays, " + std::to_string(nudges) + " nudges");
    {
        ui::InputSignals second;
        second.setKeymap(&hub.keymap());
        second.setFallback(&hub);
        second.dispatch(press(Key::S, ModCtrl));
        c.check(saves == 1, "Other windows' actions fall back to the main one");
    }

    // A text field driven by events
    ui::TextField field;
    std::string clipboard;
    hub.writeClipboard = [&](const std::string &t) { clipboard = t; };
    hub.readClipboard = [&] { return clipboard; };
    frame(hub, field);
    field.focus();
    type(hub, "hello world");
    c.check(field.text() == "hello world" && field.focused(), "Typing goes into the focused field", field.text());
    plays = 0;
    const bool spaceTaken = hub.dispatch(press(Key::Space));
    c.check(spaceTaken && plays == 0, "Space while typing doesn't play");
    hub.dispatch(press(Key::Left, ModCtrl));
    hub.dispatch(press(Key::End, ModShift));
    c.check(field.selectionStart() == 6 && field.selectionEnd() == 11, "Ctrl+Left and Shift+End select a word");
    hub.dispatch(press(Key::C, ModCtrl));
    c.check(clipboard == "world", "Ctrl+C copies the selection", clipboard);
    type(hub, "there");
    c.check(field.text() == "hello there", "Typing replaces the selection", field.text());
    hub.dispatch(press(Key::Backspace, ModCtrl));
    c.check(field.text() == "hello ", "Ctrl+Backspace deletes a word", field.text());
    hub.dispatch(press(Key::A, ModCtrl));
    hub.dispatch(press(Key::X, ModCtrl));
    c.check(field.text().empty() && clipboard == "hello ", "Ctrl+A, Ctrl+X cut everything", "\"" + clipboard + "\"");
    clipboard = "two\nlines";
    hub.dispatch(press(Key::V, ModCtrl));
    c.check(field.text() == "two lines", "Ctrl+V pastes one line", field.text());
    saves = 0;
    hub.dispatch(press(Key::S, ModCtrl));
    c.check(saves == 1, "Other shortcuts still work while typing");
    hub.keymap().assign("edit.selectAll", {Key::L, ModCtrl});
    hub.dispatch(press(Key::L, ModCtrl));
    hub.dispatch(press(Key::Delete));
    c.check(field.text().empty(), "Rebound select all works in the field");
    hub.dispatch(press(Key::Escape));
    plays = 0;
    hub.dispatch(press(Key::Space));
    c.check(!field.focused() && plays == 1, "After Escape, Space plays again");

    field.focus();
    hub.beginFrame(); // a frame the field doesn't get: hidden
    type(hub, "x");
    plays = 0;
    hub.dispatch(press(Key::Space));
    c.check(field.text().empty() && plays == 1, "A hidden field's hooks stand aside");

    // The file dialog's logic: paths, types, names, sorting
    {
        namespace files = ui::files;
        c.check(files::normalize("c:/Users//calum/") == "C:\\Users\\calum", "Paths are normalized",
                files::normalize("c:/Users//calum/"));
        c.check(files::normalize("D:") == "D:\\" && files::normalize("C:\\") == "C:\\", "A drive keeps its root");
        const auto crumbs = files::breadcrumbs("C:\\Users\\calum\\Music");
        std::string labels;
        for (const auto &crumb : crumbs)
            labels += crumb.label + "|";
        c.check(crumbs.size() == 4 && labels == "C:|Users|calum|Music|" && crumbs[0].path == "C:\\" &&
                    crumbs[2].path == "C:\\Users\\calum",
                "Breadcrumbs split a path", labels);
        const auto share = files::breadcrumbs("\\\\server\\share\\Takes");
        c.check(share.size() == 2 && share[0].label == "\\\\server\\share" && share[1].path == "\\\\server\\share\\Takes",
                "A network share is one crumb", share.empty() ? "" : share[0].label);
        c.check(files::parent("C:\\Users\\calum") == "C:\\Users" && files::parent("C:\\Users") == "C:\\" &&
                    files::parent("C:\\").empty(),
                "The folder above, and none above a root");
        c.check(files::join("C:\\Music", "Take 1.wav") == "C:\\Music\\Take 1.wav" &&
                    files::join("C:\\", "x") == "C:\\x" && files::join("C:\\Music", "D:\\y.wav") == "D:\\y.wav",
                "Joining a name, or a whole path, onto a folder");
        c.check(files::samePath("c:\\users/CALUM\\\\", "C:\\Users\\calum"), "Paths compare like Windows does");

        const auto exts = files::extensions("*.sfz; *.WAV;*.flac");
        c.check(exts.size() == 3 && exts[1] == ".wav", "Patterns become extensions");
        c.check(files::extensions("*.*").empty() && files::extensions("*").empty(), "*.* is every file");
        c.check(files::matches("Lead.SFZ", exts) && files::matches("a.wav", exts) && !files::matches("a.mp3", exts) &&
                    !files::matches(".wav", exts) && files::matches("readme.txt", {}),
                "Files match their type, any case");
        c.check(files::withExtension("Song", ".vbp") == "Song.vbp" && files::withExtension("Song.VBP", ".vbp") == "Song.VBP" &&
                    files::withExtension("v1.2", ".vbp") == "v1.2.vbp" && files::withExtension("Mix.", ".wav") == "Mix.wav" &&
                    files::withExtension("Mix", "wav") == "Mix.wav",
                "The extension is added when missing", files::withExtension("v1.2", ".vbp"));
        c.check(files::cleanFileName("a/b:c?") == "a_b_c_", "Refused characters become _");

        c.check(files::naturalLess("Take 2", "Take 10") && !files::naturalLess("Take 10", "Take 2") &&
                    files::naturalLess("apple", "Banana") && files::naturalLess("a01", "a2"),
                "Names sort naturally, any case");
        std::vector<engine::FolderEntry> entries;
        auto entry = [&](std::string name, bool folder, uint64_t size, int64_t when) {
            engine::FolderEntry e;
            e.name = std::move(name);
            e.folder = folder;
            e.size = size;
            e.modified = when;
            entries.push_back(e);
        };
        entry("take 10.wav", false, 300, 30);
        entry("Drums", true, 0, 5);
        entry("Take 2.wav", false, 100, 10);
        entry("notes.txt", false, 50, 20);
        entry("Bass", true, 0, 50);
        auto names = [](const std::vector<engine::FolderEntry> &list) {
            std::string s;
            for (const auto &e : list)
                s += e.name + "|";
            return s;
        };
        auto sorted = entries;
        files::sort(sorted, files::SortBy::Name, false);
        c.check(names(sorted) == "Bass|Drums|notes.txt|Take 2.wav|take 10.wav|", "Folders first, then by name", names(sorted));
        files::sort(sorted, files::SortBy::Size, true);
        c.check(names(sorted) == "Bass|Drums|take 10.wav|Take 2.wav|notes.txt|", "By size, largest first", names(sorted));
        files::sort(sorted, files::SortBy::Date, true);
        c.check(names(sorted) == "Bass|Drums|take 10.wav|notes.txt|Take 2.wav|", "By date, newest first", names(sorted));
        auto shown = files::filter(entries, files::extensions("*.wav"), {});
        files::sort(shown, files::SortBy::Name, false);
        c.check(names(shown) == "Bass|Drums|Take 2.wav|take 10.wav|", "The type keeps folders and its files", names(shown));
        shown = files::filter(entries, {}, "TAKE");
        c.check(shown.size() == 2, "The search finds names, any case");
        c.check(files::sizeText(820) == "820 bytes" && files::sizeText(12 * 1024) == "12 KB" &&
                    files::sizeText(3565158) == "3.4 MB",
                "Sizes read naturally", files::sizeText(3565158));
    }

    {
        // A frame's fingerprint: equal for the same picture, different once anything it draws changes
        // (a surface's position, a text's characters, an image's version, a clip)
        ui::ImageData image;
        image.width = 8;
        image.height = 8;
        image.pixels.assign(8 * 8 * 4, 0);
        const ui::Theme theme = ui::Theme::light(); // outlives the lists, which lay their text out with its font
        auto picture = [&image, &theme](float dx, const std::string &label, uint64_t version, float clipWidth) {
            image.version = version;
            ui::DrawList list;
            list.setFont(&theme.font());
            ui::Surface s;
            s.rect = {10.0f + dx, 10.0f, 100.0f, 40.0f};
            list.add(s);
            list.pushClip({0.0f, 0.0f, clipWidth, 200.0f});
            ui::Text text;
            text.text = label;
            text.baseline = {20.0f, 30.0f};
            text.size = 13.0f;
            list.add(text);
            ui::ImageDraw draw;
            draw.rect = {0.0f, 0.0f, 8.0f, 8.0f};
            draw.image = &image;
            list.add(draw);
            list.popClip();
            return ui::fingerprint(list);
        };
        const uint64_t base = picture(0.0f, "Play", 1, 300.0f);
        c.check(picture(0.0f, "Play", 1, 300.0f) == base, "The same picture has the same fingerprint");
        c.check(picture(1.0f, "Play", 1, 300.0f) != base, "A surface moved a pixel changes it");
        c.check(picture(0.0f, "Stop", 1, 300.0f) != base, "A text changed changes it");
        c.check(picture(0.0f, "Play", 2, 300.0f) != base, "An image with a new version changes it");
        c.check(picture(0.0f, "Play", 1, 280.0f) != base, "A clip changed changes it");
        c.check(ui::fingerprint(ui::DrawList{}) != base, "An empty frame differs from a drawn one");
    }

    if (bench())
        benchDispatch();

    return c.finish("input");
}

}
