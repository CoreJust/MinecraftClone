#ifndef BOUNDED_CELL_CACHE_H
#define BOUNDED_CELL_CACHE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
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
    struct Accounting final { std::atomic<uint64_t> used{0}; };
    explicit CellBudget(const uint64_t limit) : limit_(limit) {}
    bool tryReserve(const uint64_t bytes)
    {
        auto reserve = [&]() {
            auto current = accounting_->used.load(std::memory_order_relaxed);
            do {
                if (current > limit_ || bytes > limit_ - current) { return false; }
            } while (!accounting_->used.compare_exchange_weak(current, current + bytes,
                std::memory_order_relaxed));
            return true;
        };
        if (reserve()) { return true; }
        std::unique_lock lock(pool_mutex_);
        const auto attempts = retained_.size();
        for (size_t index = 0; index < attempts; ++index) {
            if (retained_.empty()) { break; }
            auto first = retained_.begin();
            if (first->use_count() != 1) {
                retained_.splice(retained_.end(), retained_, first);
                continue;
            }
            auto victim = std::move(*first);
            retained_.erase(first);
            lock.unlock();
            // Destruction can release an entire retained graph chain.
            victim.reset();
            if (reserve()) { return true; }
            lock.lock();
        }
        return reserve();
    }
    void retain(std::shared_ptr<const void> value)
    {
        std::lock_guard lock(pool_mutex_);
        retained_.push_back(std::move(value));
    }
    void release(const uint64_t bytes) { accounting_->used.fetch_sub(bytes, std::memory_order_relaxed); }
    uint64_t used() const { return accounting_->used.load(std::memory_order_relaxed); }
    uint64_t limit() const { return limit_; }
    std::shared_ptr<Accounting> accounting() const { return accounting_; }
    static constexpr uint64_t retainedEntryBytes() { return sizeof(std::shared_ptr<const void>) + 2U * sizeof(void*); }
private:
    const uint64_t limit_;
    // The budget owner's fixed bookkeeping, like CellBudget itself, is outside
    // cache logical bytes. Every retained value, FIFO node, page and memo is charged.
    std::shared_ptr<Accounting> accounting_ = std::make_shared<Accounting>();
    std::mutex pool_mutex_;
    std::list<std::shared_ptr<const void>> retained_;
};

template <typename T, uint32_t PERIOD>
class BoundedCellMemo final
{
    static constexpr uint32_t PAGE_SIDE = 32;
    static constexpr uint32_t PAGE_AXIS = PERIOD / PAGE_SIDE;
    static_assert(PERIOD >= PAGE_SIDE && PERIOD % PAGE_SIDE == 0);
    static constexpr uint64_t OWNERSHIP_BYTES = 3U * sizeof(void*);
    struct Counter final
    {
        std::atomic<uint64_t> entries{0};
        std::shared_ptr<CellBudget::Accounting> accounting;
        bool charged = false;
        ~Counter()
        {
            if (charged) {
                accounting->used.fetch_sub(sizeof(Counter) + OWNERSHIP_BYTES, std::memory_order_relaxed);
            }
        }
    };
    struct Charge final
    {
        std::shared_ptr<CellBudget::Accounting> accounting;
        std::shared_ptr<Counter> counter;
        bool admitted = false;
        Charge(std::shared_ptr<CellBudget::Accounting> state, std::shared_ptr<Counter> count)
            : accounting(std::move(state)), counter(std::move(count)) {}
        Charge(const Charge&) = delete;
        Charge& operator=(const Charge&) = delete;
        ~Charge()
        {
            if (admitted) {
                counter->entries.fetch_sub(1, std::memory_order_relaxed);
                accounting->used.fetch_sub(valueBytes(), std::memory_order_relaxed);
            }
        }
    };
    // Reverse member destruction keeps the charge until T and its child leases die.
    struct Value final
    {
        Charge charge;
        T value;
        Value(T built, std::shared_ptr<CellBudget::Accounting> state, std::shared_ptr<Counter> count)
            : charge(std::move(state), std::move(count)), value(std::move(built)) {}
    };
    struct Page final { std::array<std::weak_ptr<const T>, PAGE_SIDE * PAGE_SIDE> slots; };
    static constexpr uint64_t baseBytes() { return sizeof(BoundedCellMemo) + sizeof(Counter) + OWNERSHIP_BYTES; }
    static constexpr uint64_t valueBytes() { return sizeof(Value) + OWNERSHIP_BYTES + CellBudget::retainedEntryBytes(); }
public:
    explicit BoundedCellMemo(CellBudget& budget) : budget_(budget)
    {
        if (!budget_.tryReserve(baseBytes())) { throw HierarchyCacheLimit(); }
        counter_->accounting = budget_.accounting();
        counter_->charged = true;
    }
    ~BoundedCellMemo() { budget_.release(sizeof(*this) + pages() * sizeof(Page)); }
    static constexpr uint64_t minimumCacheBytes() { return baseBytes(); }
    static constexpr uint64_t minimumPageCacheBytes() { return baseBytes() + sizeof(Page); }
    template <typename Builder>
    std::shared_ptr<const T> get(const uint32_t x, const uint32_t y, Builder&& builder) const
    {
        if (x >= PERIOD || y >= PERIOD) {
            throw std::out_of_range("hierarchy cache key is not canonical");
        }
        const auto page_index = (y / PAGE_SIDE) * PAGE_AXIS + x / PAGE_SIDE;
        const auto slot_index = (y % PAGE_SIDE) * PAGE_SIDE + x % PAGE_SIDE;
        Page* page;
        {
            std::lock_guard lock(mutex_);
            page = pages_[page_index].get();
            if (page) {
                if (auto hit = page->slots[slot_index].lock()) { return hit; }
            }
        }
        if (!page && budget_.tryReserve(sizeof(Page))) {
            std::unique_ptr<Page> created;
            try { created = std::make_unique<Page>(); }
            catch (...) { budget_.release(sizeof(Page)); throw; }
            std::lock_guard lock(mutex_);
            if (!pages_[page_index]) {
                pages_[page_index] = std::move(created);
                page_count_.fetch_add(1, std::memory_order_relaxed);
            } else { budget_.release(sizeof(Page)); }
            page = pages_[page_index].get();
        }
        // Builders recursively query other memos; never run them under a cache lock.
        auto owned = std::make_shared<Value>(builder(), budget_.accounting(), counter_);
        std::shared_ptr<const T> result(owned, &owned->value);
        if (!page || !budget_.tryReserve(valueBytes())) { return result; }
        owned->charge.admitted = true;
        counter_->entries.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard lock(mutex_);
            if (auto hit = page->slots[slot_index].lock()) { return hit; }
            // Pool publication precedes the weak slot, so an admitted hit has an owner.
            budget_.retain(result);
            page->slots[slot_index] = result;
        }
        return result;
    }
    uint64_t entries() const { return counter_->entries.load(std::memory_order_relaxed); }
    uint64_t pages() const { return page_count_.load(std::memory_order_relaxed); }
    uint64_t logicalBytes() const { return baseBytes() + pages() * sizeof(Page) + entries() * valueBytes(); }
    static constexpr uint64_t logicalCapacityBytes()
    {
        return baseBytes() + uint64_t{PAGE_AXIS} * PAGE_AXIS * sizeof(Page)
            + uint64_t{PERIOD} * PERIOD * valueBytes();
    }
private:
    CellBudget& budget_;
    mutable std::mutex mutex_;
    mutable std::array<std::unique_ptr<Page>, PAGE_AXIS * PAGE_AXIS> pages_;
    std::shared_ptr<Counter> counter_ = std::make_shared<Counter>();
    mutable std::atomic<uint64_t> page_count_{0};
};
#endif
