//
//  Copyright 2026 Jan Brittenson
//  All rights reserved.
//
//  This program is free software: you can redistribute it and/or modify it under
//  the terms of the GNU General Public License as published by the Free Software
//  Foundation, either version 3 of the License, or (at your option) any later
//  version. 
//
//  This program is distributed in the hope that it will be useful, but WITHOUT
//  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
//  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
//  details.
//
//  You should have received a copy of the GNU General Public License along with
//  this program, named LICENSE. If not, see <http://www.gnu.org/licenses/>
//

//
// Reentrant locking for Fat32::FileSys.
//
// We simply use a pthread mutex here since we know it can have
// threads block around I/O.  The slight spin overhead for threads
// doesn't matter for our testing purposes.
//

#pragma once
#include <pthread.h>
#ifdef FAT32_LOCK_DEBUG
#include "assert.h"
#endif

namespace Fat32 {

    template <typename T>
    inline T exch(T& a, const T& b) {
        const T tmp = a; a = b; return tmp;
    }

    template <typename T> class ScopedLock {
        const volatile T& _lock;
    public:
        ScopedLock(const volatile T& l) : _lock(l) { _lock.acquire(); }
        ~ScopedLock() { _lock.release(); }

        ScopedLock() = delete;
        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;
    };


    class Lock {
    protected:
        mutable ::pthread_mutex_t _m;	// Mutable so we can lock const objects
#ifdef FAT32_LOCK_DEBUG
        mutable ::pthread_t _tid;
        mutable int _count;
#endif

    public:
        Lock() {
#ifdef FAT32_LOCK_DEBUG
            _tid = 0;
            _count = 0;
#endif
            static bool ok = false;
            static ::pthread_mutexattr_t attrs;
            if (!exch(ok, true)) {
                ::pthread_mutexattr_init(&attrs);
                ::pthread_mutexattr_settype(&attrs, PTHREAD_MUTEX_RECURSIVE);
            }
            ::pthread_mutex_init(&_m, &attrs);
        }

        ~Lock() { ::pthread_mutex_destroy(&_m); }

        void assert_locked() volatile const {
#ifdef FAT32_LOCK_DEBUG
            assert(_count);
            assert(_tid == ::pthread_self());
#endif
        }

        bool try_acquire() volatile const {
            const bool ok = !::pthread_mutex_trylock((::pthread_mutex_t*)&_m);
#ifdef FAT32_LOCK_DEBUG
            if (ok)  {
                _tid = ::pthread_self();
                ++_count;
            }
#endif
            return ok;

        }

        void acquire() volatile const {
            ::pthread_mutex_lock((::pthread_mutex_t*)&_m);
#ifdef FAT32_LOCK_DEBUG
            assert((!_tid && !_count) || (_count && _tid == ::pthread_self()));
            _tid = ::pthread_self();
            ++_count;
#endif
        }
        void release() volatile const { 
#ifdef FAT32_LOCK_DEBUG
            assert(_tid == ::pthread_self());
            assert(_count);
            --_count;
            if (_count == 0)
                _tid = 0;
#endif
            ::pthread_mutex_unlock((::pthread_mutex_t*)&_m); 
        }

        // These make no sense
        Lock(const volatile Lock&) = delete;
        Lock& operator=(const volatile Lock&) = delete;
    };

    typedef ScopedLock<Lock> Exclusive;

} // ns Fat32
