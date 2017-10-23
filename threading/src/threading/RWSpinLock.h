#pragma once

#include <atomic>

#include <shared_mutex> // for std::shared_lock

#include "details/SpinLockSpinner.h"


namespace threading{

    // one way upgrade unique_ptr -> shared_ptr
    // just for compatibility with standart
    // use plain unlock_and_lock_shared for higher performance
    template<class LockT>
    class upgrade_lock{
        LockT* lock = nullptr;
    public:
        upgrade_lock(std::unique_lock<LockT>&& lock)
            : lock(lock.release())
        {}

        upgrade_lock(const upgrade_lock&) = delete;
        upgrade_lock(upgrade_lock&& other) noexcept
            : lock(other.lock)
        {
            other.lock = nullptr;
        }

        operator std::shared_lock<LockT>(){
            lock->unlock_and_lock_shared();
            auto* t_lock = lock;
            lock = nullptr;
            return std::shared_lock<LockT>(*t_lock);
        }

        ~upgrade_lock(){
            if (lock){
                lock->unlock();
            }
        }
    };


    // use by default. Updates should be prioritised. Reader may never have a chance, if there is always writer queue.
    // Safe to use if lock -> shared_lock (updates) always.
    // Use only one cmpxchg for lock/unlock
    // and one "lock; xaddl" for shared_lock/shared_unlock
    template<SpinLockMode mode = SpinLockMode::Adaptive, class Counter = unsigned int>
    class RWSpinLockWriterBiased{
        std::atomic<Counter>  readers_count{0};
        std::atomic<bool> write_now{false};
    public:
        RWSpinLockWriterBiased(){}
        RWSpinLockWriterBiased(const RWSpinLockWriterBiased&) = delete;
        RWSpinLockWriterBiased(RWSpinLockWriterBiased&&) = delete;

        void lock() {
            using namespace details::SpinLockSpinner;

            spinWhile<mode>([&]() {
                return write_now.exchange(true, std::memory_order_acquire);
            });

            // wait for readers to exit
            spinWhile<mode>([&]() {
                return readers_count.load(std::memory_order_acquire) != 0;
            });
        }
        void unlock() {
            write_now.store(false, std::memory_order_release);
        }


        bool try_lock() {
            // fast fail path
            if(readers_count.load(std::memory_order_acquire) != 0){
                return false;
            }

            const bool was_locked = write_now.exchange(true, std::memory_order_acquire);
            if (was_locked) {
                return false;
            }

            if(readers_count.load(std::memory_order_acquire) == 0){
                return true;
            } else {
                //restore write_now state
                unlock();
                return false;
            }
        }


        bool try_lock_shared(){
            if (write_now.load(std::memory_order_acquire) == true) return false;

            readers_count.fetch_add(1, std::memory_order_acquire);

            // Very rare case
            if (!write_now.load(std::memory_order_acquire)){
                // all ok
                return true;
            } else {
                // locked while "transaction"? Fallback.
                unlock_shared();
                return false;
            }
        }


        bool try_upgrade_shared_to_unique(){
            // fast fail path
            if(readers_count.load(std::memory_order_acquire) != 1){
                return false;
            }

            const bool was_locked = write_now.exchange(true, std::memory_order_acquire);
            if (was_locked) {
                return false;
            }

            if(readers_count.load(std::memory_order_acquire) == 1){
                unlock_shared();
                return true;
            } else {
                //restore write_now state
                unlock();
                return false;
            }
        }


        void lock_shared() {
            while(true) {
                // wait for unlock
                details::SpinLockSpinner::spinWhile<mode>([&]() {
                    return write_now.load(std::memory_order_acquire);
                });


                // Safe to use acquire instead of acq_rel because of RMW operation https://stackoverflow.com/questions/21536846/is-memory-order-acquire-really-sufficient-for-locking-a-spinlock#comment32531346_21537024
                // Per 29.3/12 "Atomic read-modify-write operations shall always read the last value
                // (in the modification order) written before the write associated with the read-modify-write operation."
                // The memory ordering you select for such an R-M-W op only affects how it is ordered with reads/writes to other memory locations and non R-M-W ops to the same atomic.
                readers_count.fetch_add(1, std::memory_order_acquire);

                // Very rare case
                if (!write_now.load(std::memory_order_acquire)){
                    // all ok
                    return;
                } else {
                    // locked while "transaction"? Fallback. Go another round
                    unlock_shared();
                }
            }
        }

        void unlock_shared() {
            readers_count.fetch_sub(1, std::memory_order_release);
        }

        void unlock_and_lock_shared() {
            readers_count.fetch_add(1, std::memory_order_acquire);
            unlock();
        }
    };


    // Same as WriterBiased, but reader biased. Writer may never have a chance, if there is always reader queue.
    // lock -> shared_lock update will not help here.
    template<SpinLockMode mode = SpinLockMode::Adaptive, class Counter = unsigned int>
    class RWSpinLockReaderBiased {
        std::atomic<Counter> readers_count{0};
        std::atomic<bool> write_now{false};
    public:
        RWSpinLockReaderBiased(){}
        RWSpinLockReaderBiased(const RWSpinLockReaderBiased&) = delete;
        RWSpinLockReaderBiased(RWSpinLockReaderBiased&&) = delete;

        void lock() {
            using namespace details::SpinLockSpinner;

            while(true) {
                // wait for readers to exit first
                spinWhile<mode>([&]() {
                    return readers_count.load(std::memory_order_acquire) != 0;
                });

                spinWhile<mode>([&]() {
                    return write_now.exchange(true, std::memory_order_acquire);
                });

                // Rare case
                if (readers_count.load(std::memory_order_acquire) == 0) {
                    return;     // all ok
                } else {
                    unlock();   // Fallback, give a way to readers
                }
            }
        }
        void unlock() {
            write_now.store(false, std::memory_order_release);
        }

        void lock_shared() {
            readers_count.fetch_add(1, std::memory_order_acquire);

            // wait for unlock
            details::SpinLockSpinner::spinWhile<mode>([&]() {
                return write_now.load(std::memory_order_acquire);
            });
        }
        void unlock_shared() {
            readers_count.fetch_sub(1, std::memory_order_release);
        }

        void unlock_and_lock_shared() {
            readers_count.fetch_add(1, std::memory_order_acquire);
            unlock();
        }
    };


    using RWSpinLock = RWSpinLockWriterBiased<>;
}