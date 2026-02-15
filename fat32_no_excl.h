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
// Dummy no-op locking for Fat32::FileSys, for systems that don't need
// MT safety.
//

#pragma once

namespace Fat32 {

    template <typename T> class ScopedLock {
    public:
        ScopedLock(const T& l) { }
        ~ScopedLock() { }

        ScopedLock() = delete;
        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;
    };


    class Lock {
    public:
        Lock() { }
        ~Lock() { }
        void assert_locked() const { }
        bool try_acquire() const { return true; }

        void acquire() const { }
        void release() const { }

        // These make no sense
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    };

    typedef ScopedLock<Lock> Exclusive;

} // ns Fat32
