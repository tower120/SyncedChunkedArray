#pragma once

#include <type_traits>
#include <cassert>

#include "Recursive.h"

/// RecursiveLevelCounter for recursive Lockable (recursive_mutex/spinlock/etc.)
/// Use as follow:
///
///     LevelCounter<Recursive<SpinLock>>
///     LevelCounter<Recursive<RWSpinLock>>

namespace threading {
    namespace details::LevelCounter{
        template<typename>
        struct is_recursive : std::false_type {};

        template<typename T>
        struct is_recursive<threading::Recursive<T>> : std::true_type {};
    }

    template<class recursive_spinlock_t, class level_t = unsigned int>
    class RecursiveLevelCounter : public recursive_spinlock_t{
        level_t m_level{0};                // protected by recursive_spinlock_t
        using Base = recursive_spinlock_t;
    public:
        using Base::Base;

        // call only under lock
        auto level() const {
            if constexpr (details::LevelCounter::is_recursive<recursive_spinlock_t>::value){
                assert(Base::is_locked());
            }

            return m_level;
        }

        bool try_lock(){
            const bool locked = Base::try_lock();
            if (locked) {
                // under unique_lock now
                m_level++;
            }

            return locked;
        }

        void lock(){
            Base::lock();
            m_level++;
        }

        void unlock(){
            m_level--;
            Base::unlock();
        }
    };

}