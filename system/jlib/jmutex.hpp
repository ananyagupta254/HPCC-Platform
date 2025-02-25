/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */



#ifndef __JMUTEX__
#define __JMUTEX__

#include <assert.h>
#include <atomic>
#include <functional>
#include "jiface.hpp"
#include "jsem.hpp"

extern jlib_decl void ThreadYield();
extern jlib_decl void spinUntilReady(std::atomic_uint &value);


#ifdef _DEBUG
//#define SPINLOCK_USE_MUTEX // for testing
#define SPINLOCK_RR_CHECK     // checks for realtime threads
#define _ASSERT_LOCK_SUPPORT
#endif

#ifdef SPINLOCK_USE_MUTEX
#define NRESPINLOCK_USE_SPINLOCK
#endif


#ifdef _WIN32
#define NRESPINLOCK_USE_SPINLOCK
#endif

#ifdef _WIN32
class jlib_decl Mutex
{
protected:
    Mutex(const char *name)
    {
        mutex = CreateMutex(NULL, FALSE, name);
        assertex(mutex);
        lockcount = 0;
        owner = 0;
    }
public:
    Mutex()
    {
        mutex = CreateMutex(NULL, FALSE, NULL);
        lockcount = 0;
        owner = 0;
    }
    ~Mutex()
    {
        if (owner != 0)
            printf("Warning - Owned mutex destroyed"); // can't use DBGLOG here!
        CloseHandle(mutex);
    }
    void lock()
    {
        WaitForSingleObject(mutex, INFINITE);
        if (lockcount) {
            if(owner!=GetCurrentThreadId())     // I think only way this can happen is with unhandled thread exception
                lockcount = 0;                  // (don't assert as unhandled error may get lost)
        }
        lockcount++;
        owner=GetCurrentThreadId();
    }
    bool lockWait(unsigned timeout)
    {
        if (WaitForSingleObject(mutex, (long)timeout)!=WAIT_OBJECT_0)
            return false;
        if (lockcount) {
            if(owner!=GetCurrentThreadId())     // I think only way this can happen is with unhandled thread exception
                lockcount = 0;                  // (don't assert as unhandled error may get lost)
        }
        lockcount++;
        owner=GetCurrentThreadId();
        return true;
    }
    void unlock()
    {
        assertex(owner==GetCurrentThreadId());
        --lockcount;
        if (lockcount==0)
            owner = 0;
        ReleaseMutex(mutex);
    }

protected:
    MutexId mutex;
    ThreadId owner;
    int unlockAll()
    {
        assertex(owner==GetCurrentThreadId());
        assertex(lockcount);
        int ret = lockcount;
        int lc = ret;
        while (lc--)
            unlock();
        return ret;
    }
    void lockAll(int count)
    {
        while (count--)
            lock();
    }
    
private:
    int lockcount;
};

class jlib_decl NamedMutex: public Mutex
{
public:
    NamedMutex(const char *name)
        : Mutex(name)
    {
    }   
};



#else // posix

class jlib_decl Mutex
{
public:
    Mutex();
//  Mutex(const char *name);    //not supported
    ~Mutex();
    void lock();
    bool lockWait(unsigned timeout);
    void unlock();
protected:
    MutexId mutex;
    ThreadId owner;
    int unlockAll();
    void lockAll(int);
private:
    int lockcount;
    pthread_cond_t lock_free;
};


class jlib_decl NamedMutex
{
public:
    NamedMutex(const char *name);   
    ~NamedMutex();
    void lock();
    bool lockWait(unsigned timeout);
    void unlock();
private:
    Mutex threadmutex;
    char *mutexfname;
};



#endif

class jlib_decl synchronized
{
private:
    Mutex &mutex;
    void throwLockException(unsigned timeout);
public:
    synchronized(Mutex &m) : mutex(m) { mutex.lock(); };
    synchronized(Mutex &m,unsigned timeout) : mutex(m) { if(!mutex.lockWait(timeout)) throwLockException(timeout);  }
    inline ~synchronized() { mutex.unlock(); };
};

#ifdef _WIN32

extern "C" {
WINBASEAPI
BOOL
WINAPI
TryEnterCriticalSection(
    IN OUT LPCRITICAL_SECTION lpCriticalSection
    );
};

class jlib_decl CriticalSection
{
    // lightweight mutex within a single process
private:
    CRITICAL_SECTION flags;
#ifdef _ASSERT_LOCK_SUPPORT
    ThreadId owner;
    unsigned depth;
#endif
    inline CriticalSection(CriticalSection &) = delete;
public:
    inline CriticalSection()
    {
        InitializeCriticalSection(&flags);
#ifdef _ASSERT_LOCK_SUPPORT
        owner = 0;
        depth = 0;
#endif
    };
    inline ~CriticalSection()
    {
#ifdef _ASSERT_LOCK_SUPPORT
        assertex(owner==0 && depth==0);
#endif
        DeleteCriticalSection(&flags);
    };
    inline void enter()
    {
        EnterCriticalSection(&flags);
#ifdef _ASSERT_LOCK_SUPPORT
        if (owner)
        {
            assertex(owner==GetCurrentThreadId());
            depth++;
        }
        else
            owner = GetCurrentThreadId();
#endif
    };
    inline void leave()
    {
#ifdef _ASSERT_LOCK_SUPPORT
        assertex(owner==GetCurrentThreadId());
        if (depth)
            depth--;
        else
            owner = 0;
#endif
        LeaveCriticalSection(&flags);
    };
    inline void assertLocked()
    {
#ifdef _ASSERT_LOCK_SUPPORT
        assertex(owner == GetCurrentThreadId());
#endif
    }
#ifdef ENABLE_CHECKEDCRITICALSECTIONS
    bool wouldBlock()  { if (TryEnterCriticalSection(&flags)) { leave(); return false; } return true; } // debug only
#endif
};
#else

/**
 * Mutex locking wrapper. Use enter/leave to lock/unlock.
 */
class CriticalSection 
{
private:
    MutexId mutex;
#ifdef _ASSERT_LOCK_SUPPORT
    ThreadId owner;
    unsigned depth;
#endif
    CriticalSection (const CriticalSection &);  
public:
    inline CriticalSection()
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
#ifdef _DEBUG
        verifyex(pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE)==0); // verify supports attr
#else
        pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
#endif
        pthread_mutex_init(&mutex, &attr);
        pthread_mutexattr_destroy(&attr);
#ifdef _ASSERT_LOCK_SUPPORT
        owner = 0;
        depth = 0;
#endif
    }

    inline ~CriticalSection()
    {
#ifdef _ASSERT_LOCK_SUPPORT
        assert(owner==0 && depth==0);
#endif
        pthread_mutex_destroy(&mutex);
    }

    inline void enter()
    {
        pthread_mutex_lock(&mutex);
#ifdef _ASSERT_LOCK_SUPPORT
        if (owner)
        {
            assertex(owner==GetCurrentThreadId());
            depth++;
        }
        else
            owner = GetCurrentThreadId();
#endif
    }

    inline void leave()
    {
#ifdef _ASSERT_LOCK_SUPPORT
        assertex(owner==GetCurrentThreadId());
        if (depth)
            depth--;
        else
            owner = 0;
#endif
        pthread_mutex_unlock(&mutex);
    }
    inline void assertLocked()
    {
#ifdef _ASSERT_LOCK_SUPPORT
        assertex(owner == GetCurrentThreadId());
#endif
    }
};
#endif

/**
 * Critical section delimiter, using scope to define lifetime of
 * the lock on a critical section (parameter).
 * Blocks on construction, unblocks on destruction.
 */
class CriticalBlock
{
    CriticalSection &crit;
public:
    inline CriticalBlock(CriticalSection &c) : crit(c)      { crit.enter(); }
    inline ~CriticalBlock()                             { crit.leave(); }
};

/**
 * Critical section delimiter, using scope to define lifetime of
 * the lock on a critical section (parameter).
 * Unblocks on construction, blocks on destruction.
 */
class CriticalUnblock
{
    CriticalSection &crit;
public:
    inline CriticalUnblock(CriticalSection &c) : crit(c)        { crit.leave(); }
    inline ~CriticalUnblock()                                   { crit.enter(); }
};

class CLeavableCriticalBlock
{
    CriticalSection &crit;
    bool locked = false;
public:
    inline CLeavableCriticalBlock(CriticalSection &_crit) : crit(_crit)
    {
        enter();
    }
    inline CLeavableCriticalBlock(CriticalSection &_crit, bool lock) : crit(_crit)
    {
        if (lock)
            enter();
    }
    inline ~CLeavableCriticalBlock()
    {
        if (locked)
            crit.leave();
    }
    inline void enter()
    {
        if (locked)
            return;
        locked = true;
        crit.enter();
    }
    inline void leave()
    {
        if (locked)
        {
            locked = false;
            crit.leave();
        }
    }
};


#ifdef SPINLOCK_USE_MUTEX // for testing

class  SpinLock
{
    CriticalSection sect;
public:
    inline void enter()       
    { 
        sect.enter();
    }
    inline void leave()
    { 
        sect.leave();
    }
};

#else

class jlib_decl  SpinLock
{
    std::atomic_uint value{false};  // Use an atomic_uint rather than a bool because it is more efficient on power8
    unsigned nesting = 0;           // This is not atomic since it is only accessed by one thread at a time
    std::atomic<ThreadId> owner{0};
    inline SpinLock(SpinLock & value __attribute__((unused))) = delete; // to prevent inadvertent use as block
public:
    inline SpinLock()
    {   
    }
#ifdef _DEBUG
    ~SpinLock()             
    { 
        if (value)
            printf("Warning - Owned Spinlock destroyed"); // can't use DBGLOG here!
    }
#endif
    inline void enter()       
    { 
        ThreadId self = GetCurrentThreadId(); 
#if defined(SPINLOCK_RR_CHECK) && !defined(_WIN32)    // as requested by RKC
        int policy;
        sched_param param;
        if ((pthread_getschedparam(self, &policy, &param)==0)&&(policy==SCHED_RR)) {
            param.sched_priority = 0;
            pthread_setschedparam(self, SCHED_OTHER, &param);   // otherwise will likely re-enter
            assertex(!"SpinLock enter on SCHED_RR thread");
        }
#endif
        //owner can only match if it was set on this thread.  Therefore the load can be relaxed since single threaded
        //code is always sequentially consistent.
        if (self==owner.load(std::memory_order_relaxed))
        {
            dbgassertex(value);
            nesting++;
            return;
        }
        while (unlikely(value.exchange(true, std::memory_order_acquire)))
            spinUntilReady(value);
        owner.store(self, std::memory_order_relaxed);
    }
    inline void leave()
    {
        //It is safe to access nesting - since this thread is the only one that can access
        //it, so no need for a synchronized access
        if (nesting == 0)
        {
            owner.store(0, std::memory_order_relaxed);
            value.store(false, std::memory_order_release);
        }
        else
            nesting--;
    }
};

#endif

class SpinBlock
{
    SpinLock &lock;
public:
    inline SpinBlock(SpinLock & _lock) : lock(_lock)    { lock.enter(); }
    inline ~SpinBlock()                                 { lock.leave(); }
};

class SpinUnblock
{
    SpinLock &lock;
public:
    inline SpinUnblock(SpinLock & _lock) : lock(_lock)  { lock.leave(); }
    inline ~SpinUnblock()                               { lock.enter(); }
};

// Non re-entrant Spin locks where *absolutely* certain enters are not nested on same thread
// (debug version checks and asserts if are, release version will deadlock

#ifdef NRESPINLOCK_USE_SPINLOCK
class jlib_decl NonReentrantSpinLock: public SpinLock
{
};
#else
#ifdef _DEBUG
class jlib_decl NonReentrantSpinLock
{
    std::atomic_uint value;
    std::atomic<ThreadId> owner;
    inline NonReentrantSpinLock(NonReentrantSpinLock & value __attribute__((unused))) = delete; // to prevent inadvertent use as block
public:
    inline NonReentrantSpinLock() : value(false), owner(0)
    {
    }
    inline void enter()       
    { 
        ThreadId self = GetCurrentThreadId(); 
        assertex(self!=owner.load(std::memory_order_relaxed)); // check for reentrancy
        while (unlikely(value.exchange(true, std::memory_order_acquire)))
            spinUntilReady(value);
        owner.store(self, std::memory_order_relaxed);
    }
    inline void leave()
    { 
        assertex(GetCurrentThreadId()==owner.load(std::memory_order_relaxed)); // check for spurious leave
        owner.store(0, std::memory_order_relaxed);
        value.store(false, std::memory_order_release);
    }
};

#else

class jlib_decl  NonReentrantSpinLock
{
    std::atomic_uint value;
    inline NonReentrantSpinLock(NonReentrantSpinLock & value __attribute__((unused))) = delete; // to prevent inadvertent use as block
public:
    inline NonReentrantSpinLock() : value(false)
    {   
    }
    inline void enter()       
    { 
        while (unlikely(value.exchange(true, std::memory_order_acquire)))
            spinUntilReady(value);
    }
    inline void leave()
    { 
        value.store(false, std::memory_order_release);
    }
};
#endif

#endif

class NonReentrantSpinBlock
{
    NonReentrantSpinLock &lock;
public:
    inline NonReentrantSpinBlock(NonReentrantSpinLock & _lock) : lock(_lock)    { lock.enter(); }
    inline ~NonReentrantSpinBlock()                                             { lock.leave(); }
};

class NonReentrantSpinUnblock
{
    NonReentrantSpinLock &lock;
public:
    inline NonReentrantSpinUnblock(NonReentrantSpinLock & _lock) : lock(_lock)  { lock.leave(); }
    inline ~NonReentrantSpinUnblock()                                           { lock.enter(); }
};





class jlib_decl Monitor: public Mutex
{
    // Like a java object - you can synchronize on it for a block, wait for a notify on it, or notify on it
    Semaphore *sem;
    int waiting;
    void *last;
public:
    Monitor() : Mutex() { sem = new Semaphore(); waiting = 0; last = NULL; }
//  Monitor(const char *name) : Mutex(name) { sem = new Semaphore(name); waiting = 0; last = NULL; } // not supported
    ~Monitor() {delete sem;};

    void wait();        // only called when locked
    void notify();      // only called when locked
    void notifyAll();   // only called when locked -- notifys for all waiting threads
};

//--------------------------------------------------------------------------------------------------------------------

//Currently disabled since performance profile of own implementation is preferable, and queryWriteLocked() cannot be implemented
//#define USE_PTHREAD_RWLOCK

#ifndef USE_PTHREAD_RWLOCK

class jlib_decl ReadWriteLock
{
    bool lockRead(bool timed, unsigned timeout) { 
                                cs.enter(); 
                                if (writeLocks == 0) 
                                {
                                    readLocks++;
                                    cs.leave();
                                }
                                else
                                {
                                    readWaiting++;
                                    cs.leave();
                                    if (timed)
                                    {
                                        if (!readSem.wait(timeout)) {
                                            cs.enter(); 
                                            if (!readSem.wait(0)) {
                                                readWaiting--;
                                                cs.leave();
                                                return false;
                                            }
                                            cs.leave();
                                        }
                                    }
                                    else
                                        readSem.wait();
                                    //NB: waiting and locks adjusted before the signal occurs.
                                }
                                return true;
                            }
    bool lockWrite(bool timed, unsigned timeout) { 
                                cs.enter(); 
                                if ((readLocks == 0) && (writeLocks == 0))
                                {
                                    writeLocks++;
                                    cs.leave();
                                }
                                else
                                {
                                    writeWaiting++;
                                    cs.leave();
                                    if (timed)
                                    {
                                        if (!writeSem.wait(timeout)) {
                                            cs.enter(); 
                                            if (!writeSem.wait(0)) {
                                                writeWaiting--;
                                                cs.leave();
                                                return false;
                                            }
                                            cs.leave();
                                        }
                                    }
                                    else
                                        writeSem.wait();
                                    //NB: waiting and locks adjusted before the signal occurs.
                                }
#ifdef _DEBUG
                                exclWriteOwner = GetCurrentThreadId();
#endif
                                return true;
                            }
public:
    ReadWriteLock()
    {
        readLocks = 0; writeLocks = 0; readWaiting = 0; writeWaiting = 0;
#ifdef _DEBUG
        exclWriteOwner = 0;
#endif
    }
    ~ReadWriteLock()        { assertex(readLocks == 0 && writeLocks == 0); }

    void lockRead()         { lockRead(false, 0); }
    void lockWrite()        { lockWrite(false, 0); }
    bool lockRead(unsigned timeout) { return lockRead(true, timeout); }
    bool lockWrite(unsigned timeout) { return lockWrite(true, timeout); }
    unsigned queryReadLockCount() const { return readLocks; }
    void unlock()           { 
                                cs.enter(); 
                                if (readLocks) readLocks--;
                                else
                                {
                                    writeLocks--;
#ifdef _DEBUG
                                    exclWriteOwner = 0;
#endif
                                }
                                assertex(writeLocks == 0);
                                if (readLocks == 0)
                                {
                                    if (readWaiting)
                                    {
                                        unsigned numWaiting = readWaiting;
                                        readWaiting = 0;
                                        readLocks += numWaiting;
                                        readSem.signal(numWaiting);
                                    }
                                    else if (writeWaiting)
                                    {
                                        writeWaiting--;
                                        writeLocks++;
                                        writeSem.signal();
                                    }
                                }
                                cs.leave();
                            }
    bool queryWriteLocked() { return (writeLocks != 0); }
    void unlockRead()       { unlock(); }
    void unlockWrite()      { unlock(); }

    //MORE: May want to use the pthread implementations under linux.
protected:
    CriticalSection     cs;
    Semaphore           readSem;
    Semaphore           writeSem;
    unsigned            readLocks;
    unsigned            writeLocks;
    unsigned            readWaiting;
    unsigned            writeWaiting;
#ifdef _DEBUG
    ThreadId            exclWriteOwner;
#endif
};

#else

class jlib_decl ReadWriteLock
{
public:
    ReadWriteLock()         { pthread_rwlock_init(&rwlock, nullptr); }
    ~ReadWriteLock()        { pthread_rwlock_destroy(&rwlock); }

    void lockRead()         { pthread_rwlock_rdlock(&rwlock); }
    void lockWrite()        { pthread_rwlock_wrlock(&rwlock); }
    bool lockRead(unsigned timeout);
    bool lockWrite(unsigned timeout);
    void unlock()           { pthread_rwlock_unlock(&rwlock); }
    void unlockRead()       { pthread_rwlock_unlock(&rwlock); }
    void unlockWrite()      { pthread_rwlock_unlock(&rwlock); }
    //  bool queryWriteLocked(); // I don't think this can be implemented on top of the pthread interface

protected:
    pthread_rwlock_t    rwlock;
};

#endif

class ReadLockBlock
{
    ReadWriteLock *lock;
public:
    ReadLockBlock(ReadWriteLock &l) : lock(&l)      { lock->lockRead(); }
    ~ReadLockBlock()                                { if (lock) lock->unlockRead(); }
    void clear()
    {
        if (lock)
        {
            lock->unlockRead();
            lock = NULL;
        }
    }
};

class WriteLockBlock
{
    ReadWriteLock *lock;
public:
    WriteLockBlock(ReadWriteLock &l) : lock(&l)     { lock->lockWrite(); }
    ~WriteLockBlock()                               { if (lock) lock->unlockWrite(); }
    void clear()
    {
        if (lock)
        {
            lock->unlockWrite();
            lock = NULL;
        }
    }
};


//--------------------------------------------------------------------------------------------------------------------

class Barrier
{
    CriticalSection crit;
    int limit, remaining, waiting;
    Semaphore sem;
public:
    Barrier(int _limit) { init(_limit); }
    Barrier() { init(0); }
    void init(int _limit)
    {
        waiting = 0;
        limit = _limit;
        remaining = limit;
    }
    void wait() // blocks until 'limit' barrier points are entered.
    {
        CriticalBlock block(crit);
        while (remaining==0) {
            if (waiting) {
                crit.leave();
                ThreadYield();
                crit.enter();
            }
            else
                remaining = limit;
        }
        remaining--;
        if (remaining==0)
            sem.signal(waiting);
        else if (remaining>0) {
            waiting++;
            crit.leave();
            sem.wait();
            crit.enter();
            waiting--;
        }
    }
    void abort()            
    {
        CriticalBlock block(crit);
        remaining = -1;
        sem.signal(waiting);
    }
    void cancel(int n, bool remove) // cancel n barrier points from this instance, if remove=true reduces barrier width
    {
        CriticalBlock block(crit);
        while (remaining==0) {
            if (waiting) {
                crit.leave();
                ThreadYield();
                crit.enter();
            }
            else
                remaining = limit;
        }
        assertex(remaining>=n);
        remaining-=n;
        if (remaining==0)
            sem.signal(waiting);
        if (remove)
            limit-=n;
    }
};



// checked versions of critical block and readwrite blocks - report deadlocks

#define USECHECKEDCRITICALSECTIONS
#ifdef USECHECKEDCRITICALSECTIONS

typedef Mutex CheckedCriticalSection;
void jlib_decl checkedCritEnter(CheckedCriticalSection &crit, unsigned timeout, const char *fname, unsigned lnum);
void jlib_decl checkedCritLeave(CheckedCriticalSection &crit);
class  jlib_decl CheckedCriticalBlock
{
    CheckedCriticalSection &crit;
public:
    CheckedCriticalBlock(CheckedCriticalSection &c, unsigned timeout, const char *fname,unsigned lnum);
    ~CheckedCriticalBlock()                                         
    { 
        crit.unlock(); 
    }
};

class  jlib_decl CheckedCriticalUnblock
{
    CheckedCriticalSection &crit;
    const char *fname;
    unsigned lnum;
    unsigned timeout;

public:
    CheckedCriticalUnblock(CheckedCriticalSection &c,unsigned _timeout,const char *_fname,unsigned _lnum) 
        : crit(c)   
    { 
        timeout = _timeout;
        fname = _fname;
        lnum = _lnum;
        crit.unlock(); 
    }
    ~CheckedCriticalUnblock();
};

#define CHECKEDCRITICALBLOCK(sect,timeout)   CheckedCriticalBlock glue(block,__LINE__)(sect,timeout,__FILE__,__LINE__)
#define CHECKEDCRITICALUNBLOCK(sect,timeout) CheckedCriticalUnblock glue(unblock,__LINE__)(sect,timeout,__FILE__,__LINE__)

#define CHECKEDCRITENTER(sect,timeout) checkedCritEnter(sect,timeout,__FILE__,__LINE__)
#define CHECKEDCRITLEAVE(sect) checkedCritLeave(sect)


class jlib_decl CheckedReadLockBlock
{
    ReadWriteLock &lock;
public:
    CheckedReadLockBlock(ReadWriteLock &l, unsigned timeout, const char *fname,unsigned lnum);
    ~CheckedReadLockBlock()                             { lock.unlockRead(); }
};

class jlib_decl CheckedWriteLockBlock
{
    ReadWriteLock &lock;
public:
    CheckedWriteLockBlock(ReadWriteLock &l, unsigned timeout, const char *fname, unsigned lnum);
    ~CheckedWriteLockBlock()                                { lock.unlockWrite(); }
};

void jlib_decl checkedReadLockEnter(ReadWriteLock &l, unsigned timeout, const char *fname, unsigned lnum);
void jlib_decl checkedWriteLockEnter(ReadWriteLock &l, unsigned timeout, const char *fname, unsigned lnum);
#define CHECKEDREADLOCKBLOCK(l,timeout)   CheckedReadLockBlock glue(block,__LINE__)(l,timeout,__FILE__,__LINE__)
#define CHECKEDWRITELOCKBLOCK(l,timeout)  CheckedWriteLockBlock glue(block,__LINE__)(l,timeout,__FILE__,__LINE__)
#define CHECKEDREADLOCKENTER(l,timeout) checkedReadLockEnter(l,timeout,__FILE__,__LINE__)
#define CHECKEDWRITELOCKENTER(l,timeout) checkedWriteLockEnter(l,timeout,__FILE__,__LINE__)
#else
#define CheckedCriticalSection  CriticalSection
#define CheckedCriticalBlock    CriticalBlock
#define CheckedCriticalUnblock  CriticalUnblock
#define CHECKEDCRITENTER(sect,timeout) (sect).enter()
#define CHECKEDCRITLEAVE(sect) (sect).leave()
#define CHECKEDCRITICALBLOCK(sect,timeout)   CheckedCriticalBlock glue(block,__LINE__)(sect)
#define CHECKEDCRITICALUNBLOCK(sect,timeout) CheckedCriticalUnblock glue(unblock,__LINE__)(sect)
#define CHECKEDREADLOCKBLOCK(l,timeout)   ReadLockBlock glue(block,__LINE__)(l)
#define CHECKEDWRITELOCKBLOCK(l,timeout)  WriteLockBlock glue(block,__LINE__)(l)
#define CHECKEDREADLOCKENTER(l,timeout) (l).lockRead()
#define CHECKEDWRITELOCKENTER(l,timeout) (l).lockWrite()
#endif

class CSingletonLock        // a lock that will generally only be locked once (for locking singleton objects - see below for examples
{
    volatile bool needlock;
    CriticalSection  sect;
public:
    inline CSingletonLock()
    {
        needlock = true;
    }
    inline bool lock()
    {
        if (needlock) {
            sect.enter();
            //prevent compiler from moving any code before the critical section (unlikely)
            compiler_memory_barrier();
            return true;
        }
        //Prevent the value of the protected object from being evaluated before the condition
        compiler_memory_barrier();
        return false;
    }
    inline void unlock()
    {
        //Ensure that no code that precedes unlock() gets moved to after needlock being cleared.
        compiler_memory_barrier();
        needlock = false;
        sect.leave();
    }
};

/* Usage example

    static void *sobj = NULL;
    static CSingletonLock slock;
    void *get()
    {
        if (slock.lock()) {
            if (!sobj)          // required
                sobj = createSObj();
            slock.unlock();
        }
        return sobj;
    }
*/

/*
 * A template function for implementing a singleton object.  Using the same example as above would require:

    static std::atomic<void *> sobj;
    static CriticalSection slock;
    void *get()
    {
        return querySingleton(sobj, slock, []{ return createSObj; });
    }

 */

template <typename X, typename FUNC>
inline X * querySingleton(std::atomic<X *> & singleton, CriticalSection & cs, FUNC factory)
{
    X * value = singleton.load(std::memory_order_acquire);
    if (value)
        return value; // avoid crit

    CriticalBlock block(cs);
    value = singleton.load(std::memory_order_acquire);  // reload in case another thread got here first
    if (!value)
    {
        value = factory();
        singleton.store(value, std::memory_order_release);
    }
    return value;
}

/*
 * A template class for implementing a singleton object.  Using the same example as above would require:

    static Singleton<void> sobj;
    void *get()
    {
        return sobj.query([]{ return createSObj; });
    }

 */
template <typename X>
class Singleton
{
public:
    template <typename FUNC> X * query(FUNC factory) { return querySingleton(singleton, cs, factory); }

    //destroy() is designed to be called from a static destructor, not thread safe with calls to query() ...
    void destroy(std::function<void (X*)> destructor)
    {
        X * value = singleton.exchange(nullptr, std::memory_order_acq_rel);
        if (value)
            destructor(value);
    }
    void destroy()
    {
        X * value = singleton.exchange(nullptr, std::memory_order_acq_rel);
        delete value;
    }
private:
    std::atomic<X *> singleton = {nullptr};
    CriticalSection cs;
};

#endif
