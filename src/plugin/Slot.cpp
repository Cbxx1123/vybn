#include "Slot.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "Manager.h"
#include "env/Scheduler.h"

namespace plugin {

double valueOf(const ParamValues &values, uint32_t id, double fallback)
{
    auto it = std::lower_bound(values.begin(), values.end(), id, [](const std::pair<uint32_t, double> &v, uint32_t i) { return v.first < i; });
    return it != values.end() && it->first == id ? it->second : fallback;
}

void setValue(ParamValues &values, uint32_t id, double value)
{
    auto it = std::lower_bound(values.begin(), values.end(), id, [](const std::pair<uint32_t, double> &v, uint32_t i) { return v.first < i; });
    if (it != values.end() && it->first == id)
        it->second = value;
    else
        values.insert(it, {id, value});
}

// What the slot's jobs share with it: they hold it, so a slot can go while they finish
struct Slot::Core {
    PluginManager &manager;
    std::string id;

    std::mutex mutex;
    std::deque<std::function<void(Core &)>> ops;
    bool running = false;
    std::shared_ptr<Instance> instance; // what ops work on

    // Results for the main thread
    bool loadDone = false;
    uint64_t loadGeneration = 0;
    std::shared_ptr<Instance> loaded;
    std::string loadError;
    std::vector<ParamInfo> loadedParams;
    std::shared_ptr<const std::string> loadedBaseline;
    bool paramsDone = false;
    std::vector<ParamInfo> refreshed;
    std::vector<std::pair<uint32_t, std::pair<double, std::string>>> texts;
    bool editorDone = false;
    std::string editorError;

    Core(PluginManager &m, std::string i) : manager(m), id(std::move(i)) {}

    void drain()
    {
        for (;;) {
            std::function<void(Core &)> op;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (ops.empty()) {
                    running = false;
                    return;
                }
                op = std::move(ops.front());
                ops.pop_front();
            }
            op(*this);
        }
    }

    std::shared_ptr<Instance> current()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return instance;
    }
};

Slot::Slot(PluginManager &manager, std::string id, std::string name)
    : manager_(manager), id_(std::move(id)), name_(std::move(name)), core_(std::make_shared<Core>(manager, id_))
{
    Descriptor d;
    if (manager_.find(id_, d))
        name_ = d.name;
    if (name_.empty())
        name_ = id_;
}

Slot::~Slot()
{
    // The chain retired this slot two blocks ago, so the audio thread isn't in it; the host closes
    // in the background, and jobs still queued finish on their own
    swapIn(nullptr);
}

void Slot::enqueue(std::function<void(Core &)> op)
{
    std::shared_ptr<Core> core = core_;
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(core->mutex);
        core->ops.push_back(std::move(op));
        if (!core->running)
            start = core->running = true;
    }
    if (start)
        env::runJob(manager_.environment(), env::Lane::Interactive, [core] { core->drain(); }, manager_.jobs());
}

void Slot::swapIn(std::shared_ptr<Instance> next)
{
    Instance *old = current_.exchange(next.get(), std::memory_order_seq_cst);
    while (old && using_.load(std::memory_order_seq_cst) == old)
        std::this_thread::yield();
    std::shared_ptr<Instance> previous = std::move(instance_);
    instance_ = std::move(next);
    {
        std::lock_guard<std::mutex> lock(core_->mutex);
        core_->instance = instance_;
    }
    if (previous)
        env::runJob(manager_.environment(), env::Lane::Background, [previous] { previous->close(); }, manager_.jobs());
}

void Slot::startLoad()
{
    state_ = State::Loading;
    message_.clear();
    if (id_.empty()) {
        state_ = State::Failed;
        message_ = "no plugin chosen";
        return;
    }
    const std::shared_ptr<const std::string> state = appliedState_ ? appliedState_ : baseline_;
    const ParamValues values = applied_;
    const double rate = rate_;
    const bool offline = offline_;
    const std::string id = id_;
    const uint64_t generation = ++generation_;
    enqueue([state, values, rate, offline, id, generation](Core &c) {
        std::string error;
        // A project opened as the app starts asks before the plugin folders are scanned
        c.manager.waitForScan(30000);
        std::shared_ptr<Instance> made = c.manager.create(id, error);
        std::shared_ptr<const std::string> baseline = state;
        std::vector<ParamInfo> params;
        if (made) {
            made->setOffline(offline);
            std::string why;
            if (state && !state->empty()) {
                if (!made->loadState(*state, why))
                    made->takeLog(); // a state it can't read: it starts from its defaults
            } else if (made->info().state) {
                std::string saved;
                if (made->saveState(saved, why))
                    baseline = std::make_shared<const std::string>(std::move(saved));
            }
            for (const auto &v : values)
                made->setParam(v.first, v.second);
            if (rate > 0.0 && !made->activate(rate, error))
                made.reset();
            else if (made)
                made->params(params, why);
        }
        for (ParamInfo &p : params)
            p.value = valueOf(values, p.id, p.value);
        std::lock_guard<std::mutex> lock(c.mutex);
        if (c.loaded) // an older load nobody took: let it go
            env::runJob(c.manager.environment(), env::Lane::Background, [old = c.loaded] { old->close(); }, c.manager.jobs());
        c.loadDone = true;
        c.loadGeneration = generation;
        c.loaded = made;
        c.loadError = made ? std::string() : error;
        c.loadedParams = std::move(params);
        c.loadedBaseline = baseline;
    });
}

void Slot::setFormat(double sampleRate)
{
    if (sampleRate == rate_ || sampleRate <= 0.0)
        return;
    const bool first = rate_ == 0.0;
    rate_ = sampleRate;
    if (first) {
        startLoad();
        return;
    }
    enqueue([sampleRate](Core &c) {
        if (auto i = c.current()) {
            std::string error;
            i->activate(sampleRate, error);
        }
    });
}

void Slot::apply(const ParamValues &values, const std::shared_ptr<const std::string> &state)
{
    bool restore = false;
    if (state.get() != appliedState_.get()) {
        const std::shared_ptr<const std::string> &known = appliedState_ ? appliedState_ : baseline_;
        restore = state && !(known && *known == *state);
        appliedState_ = state;
        if (state)
            baseline_ = state;
    }
    if (values == applied_ && !restore)
        return;
    for (const auto &old : applied_)
        if (valueOf(values, old.first, -1e300) == -1e300)
            restore = true; // a value went: back to the state, then the rest
    if (state_ == State::Loading) {
        applied_ = values; // the load's end sends them
        return;
    }
    if (restore) {
        const std::shared_ptr<const std::string> base = baseline_;
        const ParamValues all = values;
        enqueue([base, all](Core &c) {
            auto i = c.current();
            if (!i)
                return;
            std::string error;
            if (base && !base->empty())
                i->loadState(*base, error);
            for (const auto &v : all)
                i->setParam(v.first, v.second);
            std::vector<ParamInfo> params;
            if (i->params(params, error)) {
                for (ParamInfo &p : params)
                    p.value = valueOf(all, p.id, p.value);
                std::lock_guard<std::mutex> lock(c.mutex);
                c.paramsDone = true;
                c.refreshed = std::move(params);
            }
        });
    } else if (instance_) {
        for (const auto &v : values)
            if (valueOf(applied_, v.first, -1e300) != v.second)
                instance_->setParam(v.first, v.second);
    }
    for (ParamInfo &p : params_)
        p.value = valueOf(values, p.id, p.value);
    applied_ = values;
}

void Slot::reload()
{
    swapIn(nullptr);
    startLoad();
}

void Slot::retarget(const std::string &id, const std::string &name)
{
    if (id == id_)
        return;
    swapIn(nullptr);
    id_ = id;
    name_ = name.empty() ? id : name;
    Descriptor d;
    if (!id.empty() && manager_.find(id, d))
        name_ = d.name;
    params_.clear();
    ++paramsVersion_;
    applied_.clear();
    appliedState_.reset();
    baseline_.reset();
    texts_.clear();
    asked_.clear();
    changes_.clear();
    if (rate_ > 0.0 || id.empty())
        startLoad();
    else
        state_ = State::Loading;
}

void Slot::update()
{
    bool loadDone = false;
    uint64_t loadGeneration = 0;
    std::shared_ptr<Instance> loaded;
    std::string loadError;
    std::vector<ParamInfo> loadedParams;
    std::shared_ptr<const std::string> loadedBaseline;
    bool paramsDone = false;
    std::vector<ParamInfo> refreshed;
    std::vector<std::pair<uint32_t, std::pair<double, std::string>>> texts;
    {
        std::lock_guard<std::mutex> lock(core_->mutex);
        std::swap(loadDone, core_->loadDone);
        loadGeneration = core_->loadGeneration;
        loaded = std::move(core_->loaded);
        loadError = std::move(core_->loadError);
        loadedParams = std::move(core_->loadedParams);
        loadedBaseline = std::move(core_->loadedBaseline);
        std::swap(paramsDone, core_->paramsDone);
        refreshed = std::move(core_->refreshed);
        texts.swap(core_->texts);
    }
    if (loadDone && loadGeneration != generation_) {
        // A load for a plugin this slot no longer holds
        if (loaded)
            env::runJob(manager_.environment(), env::Lane::Background, [loaded] { loaded->close(); }, manager_.jobs());
        loadDone = false;
        paramsDone = false;
    }
    if (loadDone) {
        if (loaded) {
            for (const auto &v : applied_) // values applied while it loaded
                loaded->setParam(v.first, v.second);
            for (ParamInfo &p : loadedParams)
                p.value = valueOf(applied_, p.id, p.value);
            if (!baseline_)
                baseline_ = loadedBaseline;
            name_ = loaded->info().name.empty() ? name_ : loaded->info().name;
            swapIn(loaded);
            params_ = std::move(loadedParams);
            ++paramsVersion_;
            texts_.clear();
            asked_.clear();
            state_ = State::Ready;
        } else {
            state_ = State::Failed;
            message_ = loadError;
        }
    }
    if (paramsDone) {
        const bool sameList = refreshed.size() == params_.size() &&
                              std::equal(refreshed.begin(), refreshed.end(), params_.begin(),
                                         [](const ParamInfo &a, const ParamInfo &b) { return a.id == b.id && a.name == b.name; });
        params_ = std::move(refreshed);
        if (!sameList)
            ++paramsVersion_;
    }
    for (auto &t : texts)
        texts_[t.first] = std::move(t.second);

    if (instance_) {
        if (state_ == State::Ready && instance_->status() == Status::Crashed) {
            state_ = State::Crashed;
            message_ = instance_->statusText();
        } else if (state_ == State::Ready && instance_->status() == Status::Hung) {
            state_ = State::Hung;
            message_ = instance_->statusText();
        }
        // The plugin's own changes: shown at once, and handed to the project
        for (const ParamChange &change : instance_->takeChanges()) {
            if (change.kind == ChangeKind::GestureBegin) {
                if (std::find(gestures_.begin(), gestures_.end(), change.id) == gestures_.end())
                    gestures_.push_back(change.id);
                continue;
            }
            if (change.kind == ChangeKind::GestureEnd) {
                gestures_.erase(std::remove(gestures_.begin(), gestures_.end(), change.id), gestures_.end());
                continue;
            }
            for (ParamInfo &p : params_) {
                if (p.id != change.id)
                    continue;
                p.value = std::clamp(change.value, p.min, p.max);
                if (!p.readOnly()) {
                    setValue(changes_, p.id, p.value);
                    setValue(applied_, p.id, p.value); // the project will say the same: nothing to send back
                }
                break;
            }
        }
        if (instance_->takeParamsChanged()) {
            enqueue([](Core &c) {
                auto i = c.current();
                std::vector<ParamInfo> params;
                std::string error;
                if (i && i->params(params, error)) {
                    std::lock_guard<std::mutex> lock(c.mutex);
                    c.paramsDone = true;
                    c.refreshed = std::move(params);
                }
            });
        }
        instance_->takeLog();
        if (!instance_->guiOpen())
            gestures_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(core_->mutex);
        if (core_->editorDone) {
            core_->editorDone = false;
            editorError_ = core_->editorError;
            editorOpening_ = false;
        }
    }
}

void Slot::openEmbeddedEditor()
{
    if (!instance_ || state_ != State::Ready || editorOpening_ || instance_->guiEmbedded())
        return;
    editorOpening_ = true;
    editorError_.clear();
    enqueue([](Core &c) {
        auto i = c.current();
        std::string error;
        uint32_t w = 0, h = 0;
        if (i && i->guiOpen() && !i->guiEmbedded())
            i->closeGui(); // its own window gives way to the embedded one
        if (i)
            i->openEmbeddedGui(w, h, error);
        std::lock_guard<std::mutex> lock(c.mutex);
        c.editorDone = true;
        c.editorError = error;
    });
}

void Slot::closeEditor()
{
    if (!instance_ || !instance_->guiOpen())
        return;
    enqueue([](Core &c) {
        if (auto i = c.current())
            i->closeGui();
    });
}

bool Slot::readEditorFrame(std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height, uint32_t &sequence)
{
    return instance_ && instance_->readGuiFrame(pixels, width, height, sequence);
}

void Slot::sendEditorInput(const GuiInput &input)
{
    if (instance_)
        instance_->sendGuiInput(input);
}

void Slot::placeEditor(int32_t x, int32_t y, bool shown, void *appWindow)
{
    if (instance_)
        instance_->placeGui(x, y, shown, appWindow);
}

void Slot::clipEditor(int32_t x, int32_t y, int32_t width, int32_t height)
{
    if (instance_)
        instance_->clipGui(x, y, width, height);
}

void Slot::allowEditorToFront()
{
    if (instance_)
        instance_->allowToFront();
}

void Slot::resizeEditor(uint32_t width, uint32_t height)
{
    if (instance_)
        instance_->resizeGui(width, height);
}

void Slot::toggleEditor()
{
    if (!instance_ || state_ != State::Ready)
        return;
    const bool open = !instance_->guiOpen();
    editorError_.clear();
    enqueue([open](Core &c) {
        auto i = c.current();
        if (!i)
            return;
        std::string error;
        if (open)
            i->openGui(error);
        else
            i->closeGui();
        std::lock_guard<std::mutex> lock(c.mutex);
        c.editorDone = true;
        c.editorError = error;
    });
}

std::string Slot::message() const
{
    switch (state_) {
    case State::Loading:
        return "Loading";
    case State::Ready:
        return {};
    case State::Failed:
        return "Couldn't load: " + message_;
    case State::Crashed:
    case State::Hung: {
        std::string m = message_;
        if (!m.empty())
            m[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(m[0])));
        return m;
    }
    }
    return {};
}

bool Slot::busy() const
{
    if (state_ == State::Loading)
        return true;
    std::lock_guard<std::mutex> lock(core_->mutex);
    return core_->running || !core_->ops.empty();
}

bool Slot::contained() const { return instance_ && instance_->contained(); }

const ParamInfo *Slot::param(uint32_t id) const
{
    for (const ParamInfo &p : params_)
        if (p.id == id)
            return &p;
    return nullptr;
}

std::string Slot::text(uint32_t id, double value)
{
    auto it = texts_.find(id);
    const bool fresh = it != texts_.end() && it->second.first == value;
    auto asked = asked_.find(id);
    if (!fresh && instance_ && state_ == State::Ready && (asked == asked_.end() || asked->second != value)) {
        asked_[id] = value;
        enqueue([id, value](Core &c) {
            auto i = c.current();
            std::string text;
            if (i && i->paramText(id, value, text)) {
                std::lock_guard<std::mutex> lock(c.mutex);
                c.texts.push_back({id, {value, text}});
            }
        });
    }
    if (it != texts_.end())
        return it->second.second;
    char number[32];
    std::snprintf(number, sizeof number, "%.2f", value);
    return number;
}

ParamValues Slot::takeChanges()
{
    ParamValues out;
    out.swap(changes_);
    return out;
}

std::shared_ptr<const std::string> Slot::captureState(uint32_t timeoutMs)
{
    if (!instance_ || state_ != State::Ready || !instance_->info().state)
        return nullptr;
    std::string state, error;
    if (!instance_->saveState(state, error, timeoutMs))
        return nullptr;
    auto captured = std::make_shared<const std::string>(std::move(state));
    // The project will hold this state with the values on top: nothing to restore
    appliedState_ = captured;
    baseline_ = captured;
    return captured;
}

uint32_t Slot::latency() const { return instance_ ? instance_->latency() : 0; }
uint32_t Slot::missed() const { return instance_ ? instance_->missed() : 0; }

void Slot::setOffline(bool offline)
{
    offline_ = offline;
    if (instance_)
        instance_->setOffline(offline);
}

bool Slot::process(const ProcessBlock &block)
{
    Instance *i = current_.load(std::memory_order_acquire);
    if (i) {
        using_.store(i, std::memory_order_seq_cst);
        if (current_.load(std::memory_order_seq_cst) != i)
            i = nullptr; // replaced just now: let it go
    }
    bool ok = false;
    if (i)
        ok = i->process(block);
    else
        for (uint32_t c = 0; c < block.outputChannels; ++c)
            std::fill(block.outputs[c], block.outputs[c] + block.frames, 0.0f);
    using_.store(nullptr, std::memory_order_release);
    return ok;
}

}
