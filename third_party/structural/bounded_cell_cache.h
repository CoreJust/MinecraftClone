#ifndef BOUNDED_CELL_CACHE_H
#define BOUNDED_CELL_CACHE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

class HierarchyCacheLimit final : public std::runtime_error
{
public:
    HierarchyCacheLimit() : std::runtime_error("hierarchy logical cache budget exceeded") {}
};

class CellBudget final
{
public:
    explicit CellBudget(const uint64_t limit) : limit_(limit) {}
    bool tryReserve(const uint64_t bytes)
    {
        auto current = used_.load(std::memory_order_relaxed);
        do {
            if (current > limit_ || bytes > limit_ - current) {
                return false;
            }
        } while (!used_.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed));
        return true;
    }
    void release(const uint64_t bytes) { used_.fetch_sub(bytes, std::memory_order_relaxed); }
    uint64_t used() const { return used_.load(std::memory_order_relaxed); }
    uint64_t limit() const { return limit_; }
private:
    const uint64_t limit_;
    std::atomic<uint64_t> used_{0};
};

template <typename T, uint32_t PERIOD>
class BoundedCellMemo final
{
    static constexpr uint32_t PAGE_SIDE = 32;
    static constexpr uint32_t PAGE_AXIS = PERIOD / PAGE_SIDE;
    static_assert(PERIOD >= PAGE_SIDE && PERIOD % PAGE_SIDE == 0);
    static constexpr uint64_t VALUE_OWNERSHIP_OVERHEAD_BYTES = 3U * sizeof(void*);
    struct Slot final { std::once_flag once; std::shared_ptr<const T> value = nullptr; };
    struct Page final { std::array<Slot, PAGE_SIDE * PAGE_SIDE> slots; };
    struct PageSlot final { std::once_flag once; std::unique_ptr<Page> value = nullptr; };
public:
    explicit BoundedCellMemo(CellBudget& budget) : budget_(budget)
    {
        if (!budget_.tryReserve(sizeof(*this))) {
            throw HierarchyCacheLimit();
        }
    }
    static constexpr uint64_t minimumPageCacheBytes() { return sizeof(BoundedCellMemo) + sizeof(Page); }
    template <typename Builder>
    std::shared_ptr<const T> get(const uint32_t x, const uint32_t y, Builder&& builder) const
    {
        if (x >= PERIOD || y >= PERIOD) {
            throw std::out_of_range("hierarchy cache key is not canonical");
        }
        auto& page_slot = pages_[(y / PAGE_SIDE) * PAGE_AXIS + x / PAGE_SIDE];
        std::call_once(page_slot.once, [&]() {
            if (!budget_.tryReserve(sizeof(Page))) {
                return;
            }
            try {
                page_slot.value = std::make_unique<Page>();
            } catch (...) {
                budget_.release(sizeof(Page));
                throw;
            }
            page_count_.fetch_add(1, std::memory_order_relaxed);
        });
        if (!page_slot.value) {
            return std::make_shared<const T>(builder());
        }
        auto& slot = page_slot.value->slots[(y % PAGE_SIDE) * PAGE_SIDE + x % PAGE_SIDE];
        std::shared_ptr<const T> uncached;
        std::call_once(slot.once, [&]() {
            // Build before admission so parents can retain uncached child results safely.
            auto candidate = std::make_shared<const T>(builder());
            if (!budget_.tryReserve(sizeof(T) + VALUE_OWNERSHIP_OVERHEAD_BYTES)) {
                // Keep cache saturation from turning a valid query into a world failure.
                uncached = std::move(candidate);
                return;
            }
            slot.value = std::move(candidate);
            entry_count_.fetch_add(1, std::memory_order_relaxed);
        });
        if (slot.value) {
            return slot.value;
        }
        if (uncached) {
            return uncached;
        }
        return std::make_shared<const T>(builder());
    }
    uint64_t entries() const { return entry_count_.load(std::memory_order_relaxed); }
    uint64_t pages() const { return page_count_.load(std::memory_order_relaxed); }
    uint64_t logicalBytes() const
    {
        return sizeof(*this) + pages() * sizeof(Page)
            + entries() * (sizeof(T) + VALUE_OWNERSHIP_OVERHEAD_BYTES);
    }
    static constexpr uint64_t logicalCapacityBytes()
    {
        return sizeof(BoundedCellMemo) + uint64_t{PAGE_AXIS} * PAGE_AXIS * sizeof(Page)
            + uint64_t{PERIOD} * PERIOD * (sizeof(T) + VALUE_OWNERSHIP_OVERHEAD_BYTES);
    }
private:
    CellBudget& budget_;
    mutable std::array<PageSlot, PAGE_AXIS * PAGE_AXIS> pages_;
    mutable std::atomic<uint64_t> page_count_{0};
    mutable std::atomic<uint64_t> entry_count_{0};
};
#endif
