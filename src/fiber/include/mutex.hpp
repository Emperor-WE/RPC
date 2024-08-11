#ifndef __MONSOON_MUTEX_H_
#define __MONSOON_MUTEX_H_

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <atomic>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include "noncopyable.hpp"
#include "utils.hpp"

namespace monsoon
{
    class Semaphore : Nonecopyable
    {
    public:
        Semaphore(uint32_t count = 0);
        ~Semaphore();

        void wait();
        void notify();

    private:
        sem_t semaphore_;
    };

    /**
     * @brief 局部锁 类模板
     */
    template <class T>
    struct ScopedLockImpl
    {
        ScopedLockImpl(T &mutex) : m_(mutex)
        {
            m_.lock();
            isLocked_ = true;
        }

        void lock()
        {
            if(!isLocked_)
            {
                std::cout << "lock" << std::endl;
                m_.lock();
                isLocked_ = true;
            }
        }

        void unlock()
        {
            if(isLocked_)
            {
                m_.unlock();
                isLocked_ = false;
            }
        }

        ~ScopedLockImpl()
        {
            unlock();
        }

    private:
        T &m_;  //mutex
        bool isLocked_; //是否已经上锁
    };

    template<class T>
    struct ReadScopedLockImpl
    {
    public:
        ReadScopedLockImpl(T &mutex) : mutex_(mutex)
        {
            mutex_.rdlock();
            isLocked_ = true;
        }

        ~ReadScopedLockImpl()
        {
            unlock();
        }

        void lock()
        {
            if(!isLocked_)
            {
                mutex_.rdlock();
                isLocked_ = true;
            }
        }

        void unlock()
        {
            if(isLocked_)
            {
                mutex_.unlock();
                isLocked_ = false;
            }
        }

    private:
        T &mutex_;
        bool isLocked_;
    };

    template <class T>
    struct WriteScopedLockImpl {
    public:
        WriteScopedLockImpl(T &mutex) : mutex_(mutex)
        {
            mutex_.wrlock();
            isLocked_ = true;
        }

        ~WriteScopedLockImpl() 
        { 
            unlock();
        }

        void lock()
        {
            if (!isLocked_)
            {
                mutex_.wrlock();
                isLocked_ = true;
            }
        }

        void unlock()
        {
            if (isLocked_)
            {
                mutex_.unlock();
                isLocked_ = false;
            }
        }

    private:
        T &mutex_;
        bool isLocked_;
    };

    class Mutex : Nonecopyable
    {
    public:
        typedef ScopedLockImpl<Mutex> Lock;

        Mutex()
        {
            CondPanic(0 == pthread_mutex_init(&m_, nullptr), "lcok init error");
        }

        ~Mutex()
        {
            CondPanic(0 == pthread_mutex_destroy(&m_), "destroy lock error");
        }

        void lock()
        {
            CondPanic(0 == pthread_mutex_lock(&m_), "lock error");
        }

        void unlock()
        {
            CondPanic(0 == pthread_mutex_unlock(&m_), "unlock error");
        }

    private:
        pthread_mutex_t m_;
    };

    class RWMutex : Nonecopyable
    {
    public:
        /* 局部读写锁 */
        typedef ReadScopedLockImpl<RWMutex> ReadLock;

        /* 局部写锁 */
        typedef WriteScopedLockImpl<RWMutex> WriteLock;

        RWMutex()
        {
            pthread_rwlock_init(&m_, nullptr);
        }

        ~RWMutex()
        {
            pthread_rwlock_destroy(&m_);
        }

        void rdlock()
        {
            pthread_rwlock_rdlock(&m_);
        }

        void wrlock()
        {
            pthread_rwlock_wrlock(&m_);
        }

        void unlock()
        {
            pthread_rwlock_unlock(&m_);
        }
    
    private:
        pthread_rwlock_t m_;
    };
}




#endif