#pragma once

/// Unordered
/// Thread-safe
/// Element access thread-safe
/// emplace/erase possible during iteration
/// Erase O(1) most of the time
/// Elements stored in continuous space
/// Speed in pair with deque/vector

#include "threading/src/threading/RWSpinLock.h"
#include "threading/src/threading/SpinLock.h"
#include "threading/src/threading/Recursive.h"
#include "threading/src/threading/RecursiveLevelCounter.h"
#include <cassert>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

template<class T, std::size_t chunk_size_t = std::max<std::size_t>(
        32,
        (std::size_t) (2048.0 / sizeof(T))    /* 4048 - best performance (higher has no effect) */
)>
class SyncedChunkedArray {
    struct settings {
        static constexpr const bool erase_immideatley = true;                   // false - for potentially higher speed
        static constexpr const bool trackable_iterator_check_aliveness = false;
    };

    using Self = SyncedChunkedArray<T, chunk_size_t>;

    struct SelfPtr {
        using Lock = threading::SpinLock<threading::SpinLockMode::Nonstop>;
        Lock lock;

        Self *ptr;

        SelfPtr(Self *ptr)
                : ptr(ptr) {}
    };

    struct Chunk;
public:
    struct Iterator {
        Chunk *chunk;
        std::size_t index;

        T &operator*() {
            return chunk->array()[index];
        }

        const T &operator*() const {
            return chunk->array()[index];
        }
    };

    class trackable_iterator;

private:
    struct Chunk : std::enable_shared_from_this<Chunk> {
        Chunk(const Chunk&) = delete;
        Chunk(Chunk&&) = delete;

        Chunk(std::shared_ptr<SelfPtr> self_ptr)
                : self_ptr(self_ptr) {}

        ~Chunk(){
            // destroy yet alive elements
            const std::size_t size = this->size;
            for (std::size_t i = 0; i < size; i++) {
                if (!aliveness[i].load()) continue;
                track_delete_element(this, i);
                array()[i].~T();
            }
        }

        // atomic_shared_ptr. Updated only under maintance lock
        std::shared_ptr<Chunk> next{nullptr};
        std::shared_ptr<Chunk> prev{nullptr};           // weak_ptr have no atomic operations, so use shared_ptr and manually set to nullptr (at maintance.merge)

        // Ownership lock
        using Lock = threading::RecursiveLevelCounter<
                threading::Recursive<threading::RWSpinLockWriterBiased<threading::SpinLockMode::Nonstop>>
                , unsigned short>;
        Lock lock;

        // May be acquried only under unique ownership
        // exclusive any change of chunk struct (emplace/maintain)
        using MaintanceLock = threading::SpinLock<threading::SpinLockMode::Yield>;
        MaintanceLock maintance_lock;

        // under maintance_lock
        bool in_free_list{false};

        // read/write under free_list.lock
        Chunk *next_free{nullptr};
        Chunk *prev_free{nullptr};


        std::shared_ptr<SelfPtr> self_ptr{nullptr};  // used by ~trackable_iterator only


        std::atomic<bool> is_first{false};     // for check only (updates in emplace)


        std::atomic<std::size_t> size{0};
        std::atomic<std::size_t> deleted_count{0};

        std::size_t alive_size() const {
            return size - deleted_count;
        }

        bool is_full() const {
            return size == chunk_size_t;
        }

        constexpr const static std::size_t merge_threshold = chunk_size_t * 0.25;      // for pathological cases only

        std::atomic<bool> aliveness[chunk_size_t];    // keep separate from values (faster skip)

        char/*std::byte*/ memory[chunk_size_t * sizeof(T)];

        T *array() {
            return reinterpret_cast<T *>(memory);
        }


        // actually, we can just use maintance_lock instead
        struct Trackable {
            std::atomic<bool> have{false};      // just for fast fail check

            // Trackable.first, trackable_iterator.next / .prev read/modify under this lock
            using Lock = threading::SpinLock<threading::SpinLockMode::Nonstop>;
            Lock lock;

            trackable_iterator *first{nullptr};
        };
        Trackable trackables[chunk_size_t];

        template<class Closure>
        void iterate(Closure &&closure) {
            const std::size_t size = this->size;
            for (std::size_t i = 0; i < size; i++) {
                // #https://stackoverflow.com/questions/46680842/c-stdmemory-order-relaxed-and-skip-stop-flag
                if (!aliveness[i].load(
                        settings::erase_immideatley ? std::memory_order_acquire : std::memory_order_relaxed))
                    continue;
                closure(Iterator{this, i});
            }
        }

        template<class ...Args>
        std::size_t emplace(Args &&...args) {
            const std::size_t index = this->size;

            T &ptr = array()[index];

            new(&ptr) T(std::forward<Args>(args)...);
            aliveness[index].store(true, std::memory_order_release);

            size++;

            return index;
        }

        void erase(std::size_t index) {
            assert(index < chunk_size_t);

            aliveness[index].store(false, std::memory_order_release);
            deleted_count++;
        }
    };

    using FirstLock = threading::SpinLock<threading::SpinLockMode::Nonstop>;
    FirstLock first_lock;
    std::shared_ptr<Chunk> first{nullptr};


    class FreeList {
        using FreeListLock = threading::SpinLock<threading::SpinLockMode::Nonstop>;
        FreeListLock lock;
        std::atomic<bool> is_empty{true};        // true if free_list_first == nullptr
        Chunk *first{nullptr};
    public:
        FreeList() {}

        FreeList(FreeList &&other) {
            std::unique_lock l(other.lock);
            first = other.first;
            is_empty = other.is_empty.load();
        }

        Chunk *get_first_under_maintance_lock(std::unique_lock<typename Chunk::MaintanceLock> &l_maintance) {
            if (is_empty) return nullptr;

            // dead lock free
            while (true) {
                std::unique_lock l(lock);
                if (first->maintance_lock.try_lock()) {
                    l_maintance = std::unique_lock{first->maintance_lock, std::adopt_lock};
                    return first;
                }
            }
        }

        void erase(Chunk *chunk, std::unique_lock<typename Chunk::MaintanceLock> &chunk_maintance_lock) {
            assert(chunk_maintance_lock.owns_lock() && !chunk->maintance_lock.try_lock());

            if (!chunk->in_free_list) return;

            std::unique_lock l(lock);       // it's ok, we have fixed lock order
            if (chunk->prev_free)
                chunk->prev_free->next_free = chunk->next_free;

            if (chunk->next_free)
                chunk->next_free->prev_free = chunk->prev_free;

            if (chunk == first) {
                first = chunk->next_free;
                if (first) first->prev_free = nullptr;
            }
            if (!first) is_empty = true;
            chunk->in_free_list = false;
        }

        void add(Chunk *chunk, std::unique_lock<typename Chunk::MaintanceLock> &chunk_maintance_lock) {
            assert(chunk_maintance_lock.owns_lock() && !chunk->maintance_lock.try_lock());

            if (chunk->in_free_list) return;

            std::unique_lock l(lock);    // it's ok, we have fixed lock order

            chunk->next_free = first;
            if (first) first->prev_free = chunk;
            first = chunk;

            if (is_empty) is_empty = false;
            chunk->in_free_list = true;
        }
    } free_list;


    // used only by ~trackable_iterator to maintain (maintain may add to free_list)
    std::shared_ptr<SelfPtr> self_ptr{std::make_shared<SelfPtr>(this)};

    template<class Closure>
    static void iterate_trackable_iterators(trackable_iterator *iter, Closure &&closure) {
        while (iter) {
            std::unique_lock l(iter->m_lock);
            closure(iter);

            iter = iter->next;
        }
    }

    static void track_delete_element(Chunk *chunk, std::size_t index) {
        auto &trackable = chunk->trackables[index];
        if (!trackable.have) return;

        std::unique_lock<typename Chunk::Trackable::Lock> l(trackable.lock);

        iterate_trackable_iterators(trackable.first, [](trackable_iterator *iter) {
            iter->chunk = nullptr;
        });

        trackable.first = nullptr;
        trackable.have = false;
    }


    static void track_move_element(
            Chunk *chunk_from, std::size_t index_from, Chunk *chunk_to, std::size_t index_to
    ) {
        if (index_from == index_to && chunk_from == chunk_to) return;

        auto &trackable_from = chunk_from->trackables[index_from];
        auto &trackable_to = chunk_to->trackables[index_to];

        const bool have_from = trackable_from.have;
        const bool have_to = trackable_to.have;

        if (!have_from && !have_to) return;

        std::unique_lock<typename Chunk::Trackable::Lock> lock_from{trackable_from.lock, std::defer_lock};
        std::unique_lock<typename Chunk::Trackable::Lock> lock_to{trackable_to.lock, std::defer_lock};
        std::lock(lock_from, lock_to);

        iterate_trackable_iterators(trackable_to.first, [](trackable_iterator *iter) {
            iter->chunk = nullptr;
        });

        iterate_trackable_iterators(trackable_from.first, [&](trackable_iterator *iter) {
            iter->chunk = chunk_to;
            iter->index = index_to;
        });

        trackable_to.first = trackable_from.first;
        trackable_from.first = nullptr;

        trackable_from.have = false;
        if (!have_to) trackable_to.have = true;
    }


    static void track_move_element(Chunk *chunk, std::size_t index_from, std::size_t index_to) {
        track_move_element(chunk, index_from, chunk, index_to);
    }

    static void compact(Chunk *chunk, std::unique_lock<typename Chunk::MaintanceLock> &maintance_lock) {
        assert(maintance_lock.owns_lock());

        std::size_t deleted_left = chunk->deleted_count;
        std::size_t m_chunk_size = chunk->size;
        for (std::size_t i = 0; i < m_chunk_size; i++) {
            auto &alivness = chunk->aliveness[i];
            if (alivness) continue;

            // clear head first
            while (chunk->aliveness[m_chunk_size - 1] == false) {
                track_delete_element(chunk, m_chunk_size - 1);

                chunk->array()[m_chunk_size - 1].~T();
                chunk->aliveness[m_chunk_size - 1] = false;
                deleted_left--;
                m_chunk_size--;

                if (m_chunk_size == 0) break;
            }
            if (i >= m_chunk_size) break;

            track_move_element(chunk, m_chunk_size - 1, i);

            T &element = chunk->array()[i];
            T &element_last = chunk->array()[m_chunk_size - 1];
            element = std::move(element_last);
            alivness = true;

            element_last.~T();
            chunk->aliveness[m_chunk_size - 1] = false;
            m_chunk_size--;


            deleted_left--;
            if (deleted_left == 0) break;
        }

        chunk->deleted_count = 0;
        chunk->size = m_chunk_size;
    }

    static void merge(Chunk *chunk_to, Chunk *chunk_from,
          std::unique_lock<typename Chunk::MaintanceLock> &maintance_lock_to,
          std::unique_lock<typename Chunk::MaintanceLock> &maintance_lock_from
    ) {
        assert(maintance_lock_to.owns_lock());
        assert(maintance_lock_from.owns_lock());

        if (chunk_to->deleted_count > 0) {
            compact(chunk_to, maintance_lock_to);
        }

        const std::size_t m_chunk_size = chunk_from->size;
        for (std::size_t i = 0; i < m_chunk_size; i++) {
            if (!chunk_from->aliveness[i]) continue;

            const std::size_t index_to = chunk_to->size;

            track_move_element(chunk_from, i, chunk_to, index_to);

            chunk_to->array()[index_to] = std::move(chunk_from->array()[i]);
            chunk_to->aliveness[index_to] = true;
            chunk_to->size++;

            chunk_from->array()[i].~T();
        }

        chunk_from->size = 0;
        chunk_from->deleted_count = 0;
    }

    // chunk may become destructed if not holded by shared_ptr above
    template<bool shared>
    static void maintain_and_unlock(Chunk *chunk, Self *p_self = nullptr) {
        // !shared chunk must come under unique_lock
        // shared under shared_lock

        const bool need_merge  = !chunk->is_first && chunk->alive_size() <= Chunk::merge_threshold;
        const bool need_compact = chunk->deleted_count > 0;
        const bool need_maintain = need_merge || need_compact;

        std::shared_ptr < Chunk > chunk_self{nullptr};   // postpone destruction


        // chunk must be under unique_lock + maintance lock to be deleted
        auto remove_chunk = [&](Chunk *chunk) {
            chunk_self = chunk->shared_from_this();      assert(chunk_self);
            std::shared_ptr < Chunk > prev = std::atomic_load(&chunk->prev);
            std::shared_ptr < Chunk > next = std::atomic_load(&chunk->next);

            //assert(prev);   // we do not delete/merge first chunk, so prev must exists
            if (prev){
                std::shared_ptr<Chunk> self = chunk_self;
                std::atomic_compare_exchange_strong(&prev->next, &self, next);
            }

            if (next){
                std::shared_ptr<Chunk> self = chunk_self;
                std::atomic_compare_exchange_strong(&next->prev, &self, prev);
            }

            // unlink
            const std::shared_ptr<Chunk> chunk_null{nullptr};
            std::atomic_store(&prev, chunk_null);
        };

        auto can_merge = [](Chunk *chunk, Chunk *other) -> bool {
            return !chunk->is_first && !other->is_first
                   && (chunk->alive_size() + other->alive_size()) <= Chunk::merge_threshold;
        };

        auto if_self = [](Self *p_self, Chunk *chunk, auto &&closure) {
            std::unique_lock<typename SelfPtr::Lock> self_ptr_lock;
            Self *self = p_self;
            if (!self) {
                self_ptr_lock = std::unique_lock{chunk->self_ptr->lock};
                self = chunk->self_ptr->ptr;
            }
            if (self) {
                closure(self);
            }
        };

        auto try_add_to_free_list = [if_self](Self *p_self, Chunk *chunk,
                                              std::unique_lock<typename Chunk::MaintanceLock> &l_m) {
            if (!chunk->in_free_list && !chunk->is_full()
                && !chunk->is_first     /* may delete this check */
                    ) {
                if_self(p_self, chunk, [&](Self *self) {
                    self->free_list.add(chunk, l_m);
                });
            }
        };

        auto try_remove_from_free_list = [if_self](Self *p_self, Chunk *chunk,
                                                   std::unique_lock<typename Chunk::MaintanceLock> &l_m) {
            if (chunk->in_free_list) {
                if_self(p_self, chunk, [&](Self *self) {
                    self->free_list.erase(chunk, l_m);
                });
            }
        };

        auto try_merge_with = [&](Chunk *other) -> bool {
            if (!can_merge(chunk, other)) return false;

            std::unique_lock l(other->lock, std::try_to_lock);
            if (!l) return false;

            std::unique_lock l_m_chunk{chunk->maintance_lock, std::defer_lock};
            std::unique_lock l_m_other{other->maintance_lock, std::defer_lock};
            std::lock(l_m_chunk, l_m_other);

            if (!can_merge(chunk, other)) return false;

            {
                Chunk *from;
                Chunk *to;
                std::unique_lock<typename Chunk::MaintanceLock> *from_maintance_lock;
                std::unique_lock<typename Chunk::MaintanceLock> *to_maintance_lock;

                // merge to chunk with bigger size
                if (chunk->alive_size() > other->alive_size()) {
                    to = chunk;         to_maintance_lock = &l_m_chunk;
                    from = other;       from_maintance_lock = &l_m_other;
                } else {
                    to = other;         to_maintance_lock = &l_m_other;
                    from = chunk;       from_maintance_lock = &l_m_chunk;
                }

                merge(to, from, *to_maintance_lock, *from_maintance_lock);

                // remove "from chunk" free_list
                try_remove_from_free_list(p_self, from, *from_maintance_lock);

                // add "to chunk" free_list
                try_add_to_free_list(p_self, to, *to_maintance_lock);

                remove_chunk(from);
            }

            return true;
        };

        auto try_delete = [&](Chunk* chunk) -> bool{
            if (chunk->alive_size() > 0 || chunk->is_first) return false;

            // try just delete
            std::unique_lock l_m{chunk->maintance_lock};
            if (chunk->alive_size() > 0 || chunk->is_first) return false;

            try_remove_from_free_list(p_self, chunk, l_m);
            remove_chunk(chunk);

            return true;
        };

        auto try_maintain = [&]() {
            if (chunk->lock.level() != 1) return;    // maintain only at toppest level

            if (try_delete(chunk)) return;

            if (need_merge) {
                // merge with previous
                std::shared_ptr < Chunk > prev = std::atomic_load(&chunk->prev);
                const bool merged = prev && try_merge_with(prev.get());
                if (!merged) {
                    std::shared_ptr < Chunk > next = std::atomic_load(&chunk->next);
                    const bool merged = next && try_merge_with(next.get());
                }
            }

            // still need compact?
            if (chunk->deleted_count > 0) {
                std::unique_lock l_m{chunk->maintance_lock};
                compact(chunk, l_m);
                try_add_to_free_list(p_self, chunk, l_m);
            }
        };

        // maintance
        if (!shared) {
            // we under unique_lock now
            if (need_maintain) try_maintain();
            chunk->lock.unlock();
        } else {
            chunk->lock.unlock_shared();

            if (need_maintain) {
                if (chunk->lock.try_lock()) {
                    try_maintain();
                    chunk->lock.unlock();
                }
            }
        }
    }

public:
    SyncedChunkedArray() {}

    // TODO: add copy

    // maybe better to put SyncedChunkedArray in shared_ptr ?
    // or make SyncedChunkedArray non movable?
    SyncedChunkedArray(SyncedChunkedArray &&other) {
        std::unique_lock l_other_self_ptr(other.self_ptr->lock);

        {
            std::unique_lock l_other(other.first_lock);
            first = std::move(other.first);
        }

        free_list = std::move(other.free_list);

        self_ptr->ptr = other.self_ptr->ptr;
        *self_ptr = this;
    }

    // may block, till all trackable_iterators will be released
    ~SyncedChunkedArray() {
        std::unique_lock l_other(self_ptr->lock);
        self_ptr->ptr = nullptr;

        {
            std::unique_lock l(first_lock);
            std::shared_ptr<Chunk> chunk = first;
            while (chunk) {
                std::shared_ptr<Chunk> next;
                {
                    // fixed lock order
                    std::unique_lock l_chunk(chunk->lock);
                    std::unique_lock l_maintain(chunk->maintance_lock);

                    next = std::atomic_load(&chunk->next);
                    chunk->next = nullptr;
                    chunk->prev = nullptr;
                }

                chunk = std::move(next);
            }
        }
    }

    template<class ...Args>
    auto emplace(Args &&...args) {
        Chunk *chunk;       // can't be merged/deleted while under lock

        std::unique_lock<typename Chunk::MaintanceLock> l_maintance;

        chunk = free_list.get_first_under_maintance_lock(l_maintance);

        if (!chunk) {
            std::unique_lock l(first_lock);

            if (!first) {
                first = std::make_shared<Chunk>(self_ptr);
                first->is_first = true;
            }

            l_maintance = std::unique_lock{first->maintance_lock};
            if (first->is_full()) {
                auto chunk = std::make_shared<Chunk>(self_ptr);

                chunk->next = first;
                std::atomic_store(&first->prev, chunk);
                chunk->is_first = true;

                auto prev_first = std::move(first); // keep first alive till unlock
                    first = std::move(chunk);
                prev_first->is_first = false;

                l_maintance = std::unique_lock{first->maintance_lock};
            }

            chunk = first.get();
        }


        std::size_t index = chunk->emplace(std::forward<Args>(args)...);


        if (chunk->in_free_list && chunk->is_full()) {
            free_list.erase(chunk, l_maintance);
        }

        return [l = std::move(l_maintance), chunk, index]() -> trackable_iterator {
            return {chunk, index};
        };
    }

    void erase(const Iterator &iter) {
        iter.chunk->erase(iter.index);

        if (settings::erase_immideatley){
            if (iter.chunk->lock.try_lock()){
                maintain_and_unlock<false>(iter.chunk, this);
            }
        }
    }

    void erase(const trackable_iterator &iter) {
        auto ptr = iter.lock();
        if (!ptr) return;

        erase(Iterator{iter.chunk, iter.index});
    }

    // unordered iteration
    template<bool shared = false, class Closure>
    void iterate(Closure &&closure) {
        std::vector<std::shared_ptr<Chunk>> skipped;    // TODO: put to thread_local / or use small_vector


        auto try_lock_chunk = [](Chunk *chunk) {
            return shared ? chunk->lock.try_lock_shared() : chunk->lock.try_lock();
        };
        auto unlock_chunk = [](Chunk *chunk) {
            return shared ? chunk->lock.unlock_shared() : chunk->lock.unlock();
        };


        auto iterate_and_unlock = [&](Chunk *chunk) {
            chunk->iterate(closure);
            maintain_and_unlock<shared>(chunk, this);
        };


        {
            std::shared_ptr < Chunk > chunk;
            {
                std::unique_lock l(first_lock);
                chunk = first;
            }

            while (chunk) {
                if (try_lock_chunk(chunk.get())) {
                    iterate_and_unlock(chunk.get());
                } else {
                    skipped.emplace_back(chunk);
                }

                chunk = std::atomic_load(&chunk->next);
            }
        }

        // loop on skipped
        std::size_t size = skipped.size();
        while (size > 0) {
            for (int i = 0; i < size; ++i) {
                std::shared_ptr < Chunk > &chunk = skipped[i];
                if (try_lock_chunk(chunk.get())) {
                    iterate_and_unlock(chunk.get());

                    // unordered remove from list
                    if (chunk != skipped.back()) chunk = std::move(skipped.back());
                    skipped.pop_back();
                    size--;
                }
            }
            std::this_thread::yield();
        }
    }

    template<bool shared = false, class Closure>
    void iterate_shared(Closure &&closure) {
        iterate<true>(std::forward<Closure>(closure));
    };

    std::size_t get_chunks_count() {
        std::shared_ptr < Chunk > chunk;
        {
            std::unique_lock l(first_lock);
            chunk = first;
        }

        std::size_t count = 0;
        while (chunk) {
            count++;
            chunk = std::atomic_load(&chunk->next);
        }
        return count;
    }


    class trackable_iterator {
        friend Self;

        Chunk *chunk{nullptr};      // if use shared_ptr<Chunk> ~SyncChunkedArray() can be lockless
        std::size_t index;

        typename Chunk::Trackable &trackable() { return chunk->trackables[index]; }

        // changeread under_trackable_lock
        trackable_iterator *prev{nullptr};
        trackable_iterator *next{nullptr};

        using Lock = threading::SpinLock<threading::SpinLockMode::Nonstop>;
        mutable Lock m_lock;

        // closure will not be executed if no chunk
        template<class Closure>
        void under_trackable_lock(Closure &&closure) {
            typename Chunk::Trackable *trackable;
            while (true) {
                std::unique_lock l(m_lock);
                if (!chunk) return;

                trackable = &this->trackable();
                if (trackable->lock.try_lock()) break;

                std::this_thread::yield();
            }

            closure();

            trackable->lock.unlock();
        }

        trackable_iterator(Chunk *chunk, std::size_t index)
                : chunk(chunk), index(index) {
            assert(chunk);

            typename Chunk::Trackable &trackable = this->trackable();
            std::unique_lock l(trackable.lock);

            trackable_iterator *was = trackable.first;
            trackable.first = this;
            this->next = was;

            if (was) was->prev = this;

            if (!trackable.have) trackable.have = true;
        }

    public:
        trackable_iterator(Iterator &iter)
                : trackable_iterator(iter.chunk, iter.index) {}

        trackable_iterator() {}


        trackable_iterator(trackable_iterator &&other) {
            other.under_trackable_lock([&]() {
                std::unique_lock<Lock> l(m_lock);

                next = other.next;
                prev = other.prev;

                chunk = other.chunk;
                index = other.index;

                other.chunk = nullptr;

                if (prev) prev->next = this;
                if (next) next->prev = this;

                if (!prev) {
                    assert(trackable().first == &other);
                    trackable().first = this;
                }
            });
        }

        // TODO: add copy ctr

        ~trackable_iterator() {
            under_trackable_lock([&]() {
                typename Chunk::Trackable &trackable = chunk->trackables[index];

                if (!prev && !next) {
                    // the very last one
                    assert(trackable.first == this);

                    trackable.first = nullptr;
                    trackable.have = false;
                } else if (!prev) {
                    // first
                    next->prev = nullptr;

                    assert(trackable.first == this);
                    trackable.first = next;
                } else if (!next) {
                    // last
                    prev->next = nullptr;
                } else {
                    // in between
                    prev->next = next;
                    next->prev = prev;
                }
            });
        }


        template<bool shared = false>
        class access {
            friend trackable_iterator;

            Chunk *chunk;
            T *ptr;

            access(Chunk *chunk, T *ptr)
                    : chunk(chunk), ptr(ptr) {}

        public:
            operator bool() const {
                return chunk != nullptr;
            }

            T *get() const {
                return ptr;
            }

            T &operator*() {
                return *get();
            }

            const T &operator*() const {
                return *get();
            }

            ~access() {
                if (!chunk) return;

                Self::maintain_and_unlock<shared>(chunk);       // chunk may be deleted after maintain
            }
        };

        template<bool shared = false>
        access<shared> lock() const {
            while (true) {
                std::unique_lock<Lock> l(m_lock);
                if (!chunk) return {nullptr, nullptr};

                if (chunk->lock.try_lock()) break;
                std::this_thread::yield();
            }

            if (settings::trackable_iterator_check_aliveness) {
                if (!chunk->aliveness[index].load(
                        settings::erase_immideatley ? std::memory_order_acquire : std::memory_order_relaxed))
                    return {nullptr, nullptr};
            }

            return {chunk, &chunk->array()[index]};
        }

        access<true> lock_shared() const {
            return lock<true>();
        }
    };

};
