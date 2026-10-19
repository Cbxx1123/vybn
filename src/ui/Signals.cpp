#include "Signals.h"

#include <utility>

namespace ui {

env::Connection InputSignals::onAction(std::string id, std::function<bool(const ActionEvent &)> handler, int priority)
{
    return action.connect(
        [id = std::move(id), handler = std::move(handler)](const ActionEvent &e) { return e.id == id && handler(e); },
        priority);
}

bool InputSignals::trigger(const std::string &id, bool repeat)
{
    if (action.emit(ActionEvent{id, repeat}))
        return true;
    return fallback_ && fallback_ != this && fallback_->trigger(id, repeat);
}

bool InputSignals::dispatch(const engine::KeyEvent &event)
{
    if (key.emit(event))
        return true;
    if (!event.down || engine::isModifier(event.key))
        return false;
    for (const std::string &id : keymap_->actionsFor(event.chord())) {
        const ActionInfo *info = keymap_->find(id);
        if (event.repeat && info && !info->repeats)
            continue;
        if (trigger(id, event.repeat))
            return true;
    }
    return false;
}

bool InputSignals::dispatch(const engine::TextEvent &event)
{
    return text.emit(event);
}

bool Hooks::attach(InputSignals *signals)
{
    if (!signals)
        return false;
    if (signals == signals_ && signals->id() == signalsId_) {
        seen_ = signals->frame();
        return false;
    }
    connections_.clear();
    signals_ = signals;
    signalsId_ = signals->id();
    seen_ = signals->frame();
    return true;
}

void Hooks::key(int priority, std::function<bool(const engine::KeyEvent &)> handler)
{
    if (!signals_)
        return;
    connections_.push_back(signals_->key.connect(
        [this, handler = std::move(handler)](const engine::KeyEvent &e) { return live() && handler(e); }, priority));
}

void Hooks::text(int priority, std::function<bool(const engine::TextEvent &)> handler)
{
    if (!signals_)
        return;
    connections_.push_back(signals_->text.connect(
        [this, handler = std::move(handler)](const engine::TextEvent &e) { return live() && handler(e); }, priority));
}

void Hooks::action(const std::string &id, int priority, std::function<bool(const ActionEvent &)> handler)
{
    if (!signals_)
        return;
    connections_.push_back(signals_->onAction(
        id, [this, handler = std::move(handler)](const ActionEvent &e) { return live() && handler(e); }, priority));
}

void Hooks::anyAction(int priority, std::function<bool(const ActionEvent &)> handler)
{
    if (!signals_)
        return;
    connections_.push_back(signals_->action.connect(
        [this, handler = std::move(handler)](const ActionEvent &e) { return live() && handler(e); }, priority));
}

void Hooks::clear()
{
    connections_.clear();
    signals_ = nullptr;
    signalsId_ = 0;
}

}
