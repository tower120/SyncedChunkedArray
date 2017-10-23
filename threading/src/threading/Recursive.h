#pragma once

/// Use as follow:
///
///     Recursive<SpinLock>
///     Recursive<RWSpinLock>

namespace threading{

    template<class spin_lock_t>
    class Recursive : public spin_lock_t{
        using Base = spin_lock_t;
        inline static thread_local std::size_t m_level{0};
    public:
        using Base::Base;

        bool is_locked() const{
            return m_level>0;
        }

        bool try_lock(){
            if (m_level==0) {
                const bool locked = Base::try_lock();
                if (locked){
                    m_level++;
                }
                return locked;
            }

            m_level++;
            return true;
        }

        void lock(){
            if (m_level==0){
                Base::lock();
            }

            m_level++;
        }


        void unlock(){
            m_level--;

            if (m_level==0)
                Base::unlock();
        }
    };

}
