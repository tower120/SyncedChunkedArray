#pragma once

#include <thread>

// std::lock like, accept closures which returns pointers to Lockables, or nullptr
// return tuple of std::unique_lock
/*
    std::mutex m1, m2;
    bool b;

    auto l = lock_functional(
        [&](){ return &m1; },
        [&](){ return (b ? nullptr : &m2); }
    );
 */

namespace threading{

    template< class GetLockPtr1, class GetLockPtr2>
    static auto lock_functional(GetLockPtr1&& get_lock_ptr1, GetLockPtr2&& get_lock_ptr2){
        using Mutex1 = std::remove_pointer_t<decltype(get_lock_ptr1())>;
        using Mutex2 = std::remove_pointer_t<decltype(get_lock_ptr2())>;

        using Lock1 = std::unique_lock<Mutex1>;
        using Lock2 = std::unique_lock<Mutex2>;

        using Ret = std::pair<Lock1, Lock2>;

        while(true){
            auto* lock_ptr1 = get_lock_ptr1();
            if (!lock_ptr1){
                return Ret( Lock1(), Lock2() );
            }

            Lock1 lock1(*lock_ptr1);
            if (!lock1){
                std::this_thread::yield();
                continue;
            }

            auto* lock_ptr2 = get_lock_ptr2();
            if (!lock_ptr2){
                return Ret( std::move(lock1), Lock2() );
            }

            Lock2 lock2(*lock_ptr2);
            if (!lock1){
                std::this_thread::yield();
                continue;
            }

            return Ret( std::move(lock1), std::move(lock2) );
        }
    }

}