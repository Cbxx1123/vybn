#include "DrawCache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ui {

namespace {

DrawCache::Totals totals;

} // namespace

uint64_t DrawCache::fullKey(const DrawList &list, const Theme &theme, const Rect &rect, uint64_t key)
{
    Fingerprint fp(key);
    fp.add(rect);
    float clip[4];
    list.clip(clip);
    for (float v : clip)
        fp.add(v);
    float bounds[4];
    list.bounds(bounds);
    for (float v : bounds)
        fp.add(v);
    fp.add(theme.fingerprint());
    fp.add(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(list.font())));
    return fp.value();
}

bool DrawCache::verifying()
{
    static const bool on = [] {
        const char *value = std::getenv("VYBN_VERIFY_CACHE");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return on;
}

void DrawCache::check(const DrawSection &fresh) const
{
    if (warned_)
        return;
    const size_t count = section_.prims.size();
    bool same = fresh.prims.size() == count;
    if (same && count > 0)
        same = std::memcmp(fresh.prims.data(), section_.prims.data(), count * sizeof(Prim)) == 0;
    if (same) {
        same = fresh.data.size() == section_.data.size();
        if (same && !section_.data.empty())
            same = std::memcmp(fresh.data.data(), section_.data.data(),
                               section_.data.size() * sizeof(float)) == 0;
    }
    if (same)
        same = fresh.hash == section_.hash;
    if (same)
        return;
    std::fprintf(stderr, "DrawCache: %s drew something else under the same key (%zu prims cached, %zu painted)\n",
                 name_, count, fresh.prims.size());
    std::fflush(stderr);
    warned_ = true;
}

void DrawCache::counted(bool hit, size_t prims) const
{
    if (hit) {
        ++hits_;
        ++totals.hits;
        totals.cachedPrims += static_cast<uint32_t>(prims);
    } else {
        ++misses_;
        ++totals.misses;
    }
}

DrawCache::Totals DrawCache::takeTotals()
{
    Totals out = totals;
    totals = Totals{};
    return out;
}

} // namespace ui
