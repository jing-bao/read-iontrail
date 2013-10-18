/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jscntxt.h"
#include "jslock.h"

#include "vm/Monitor.h"
#include "vm/ThreadPool.h"

#ifdef JS_THREADSAFE
#  include "prthread.h"
#endif

using namespace js;

/////////////////////////////////////////////////////////////////////////////
// ThreadPoolWorker
//
// Each |ThreadPoolWorker| just hangs around waiting for items to be added
// to its |worklist_|.  Whenever something is added, it gets executed.
// Once the worker's state is set to |TERMINATING|, the worker will
// exit as soon as its queue is empty.

const size_t WORKER_THREAD_STACK_SIZE = 1*1024*1024;////每个线程的堆栈大小

// 代表每个线程

// 状态机
//                                 run()
//                                ------
//                                |循环|
// 构造函数            start()    v    |   terminate()                    run()
// ---------->CREATED ----------> ACTIVE ---------------> TERMINATING -----------> TERMINATED
//              |                 |                                                  ^  ^
//              |                 |  PR_CreateThread失败                             |  |
//              |   terminate()   ----------------------------------------------------  |
//              -------------------------------------------------------------------------
// 在run()中，一旦开始，会完成当前worklist的所有任务。然后检查是否TERMINATING。
// 所以ThreadPool要求terminate时，每个线程会先完成当前任务。

// 锁和条件变量都是对每个Worker而言的，所以要同步的是主线程和子线程
// 锁
// 子线程始终运行run()，在进入循环前锁住，run()退出即线程退出，此时解锁。在运行某个任务的间隙里，允许增加新任务，所以临时解锁。
// 主线程通过submit()添加任务，进入时锁住，添加完毕解锁
// 主线程通过terminate()要求线程退出，进入时锁住，改变状态完毕解锁
// 一旦子线程循环开始，由于锁的唯一性，submit()和terminate()只能在子线程run()的间隙中完成，且每次操作完整独立

// 条件变量
// 任务提交：
// 子线程在run()完成当前任务列表后wait，等待下一次任务提交或结束命令。
// 主线程submit()增加任务后notify，terminate()更改状态为TERMINATING后notify
// 线程退出：
// 主线程在terminate()更改状态为TERMINATING后，循环等待，每次有notify时，检查state是否已经为TERMINATED，直到确定子线程退出，才退出terminate()
// 子线程退出时notify

class js::ThreadPoolWorker : public Monitor
{
    const size_t workerId_;

    // Current point in the worker's lifecycle.
    //
    // Modified only while holding the ThreadPoolWorker's lock.
    enum WorkerState {
        CREATED, ACTIVE, TERMINATING, TERMINATED
    } state_;

    // Worklist for this thread.
    //
    // Modified only while holding the ThreadPoolWorker's lock.
    // 任务列表
    js::Vector<TaskExecutor*, 4, SystemAllocPolicy> worklist_;

    // The thread's main function
    static void ThreadMain(void *arg);
    // 主要工作实现函数，依次运行worklist里的任务，具体执行通过TaskExecutor::executeFromWorker
    void run();

  public:
    ThreadPoolWorker(size_t workerId);
    ~ThreadPoolWorker();

    bool init();

    // Invoked from main thread; signals worker to start.
    // 创建线程，每个线程的入口函数是ThreadMain，ThreadMain则会调用run
    bool start();

    // Submit work to be executed. If this returns true, you are guaranteed
    // that the task will execute before the thread-pool terminates (barring
    // an infinite loop in some prior task).
    // 给worklist增加一个任务
    bool submit(TaskExecutor *task);

    // Invoked from main thread; signals worker to terminate and blocks until
    // termination completes.
    // 终结
    void terminate();
};

ThreadPoolWorker::ThreadPoolWorker(size_t workerId)
  : workerId_(workerId),
    state_(CREATED),
    worklist_()
{ }

ThreadPoolWorker::~ThreadPoolWorker()
{ }

bool
ThreadPoolWorker::init()
{
    return Monitor::init();//Monitor类涉及锁和条件变量的操作，包括数据成员PRLock和PRCondVar
}

bool
ThreadPoolWorker::start()
{
#ifndef JS_THREADSAFE
    return false;
#else
    JS_ASSERT(state_ == CREATED);

    // Set state to active now, *before* the thread starts:
    state_ = ACTIVE;

    if (!PR_CreateThread(PR_USER_THREAD,
                         ThreadMain, this,
                         PR_PRIORITY_NORMAL, PR_LOCAL_THREAD,
                         PR_UNJOINABLE_THREAD,
                         WORKER_THREAD_STACK_SIZE))
    {
        // If the thread failed to start, call it TERMINATED.
        state_ = TERMINATED;
        return false;
    }

    return true;
#endif
}

void
ThreadPoolWorker::ThreadMain(void *arg)
{
    ThreadPoolWorker *thread = (ThreadPoolWorker*) arg;
    thread->run();
}

void
ThreadPoolWorker::run()
{
    // This is hokey in the extreme.  To compute the stack limit,
    // subtract the size of the stack from the address of a local
    // variable and give a 10k buffer.  Is there a better way?
    // (Note: 2k proved to be fine on Mac, but too little on Linux)
    // 堆栈大小的计算没看明白
    uintptr_t stackLimitOffset = WORKER_THREAD_STACK_SIZE - 10*1024;
    uintptr_t stackLimit = (((uintptr_t)&stackLimitOffset) +
                             stackLimitOffset * JS_STACK_GROWTH_DIRECTION);

    AutoLockMonitor lock(*this);//锁住Lock

    for (;;) {
        while (!worklist_.empty()) {
            TaskExecutor *task = worklist_.popCopy();
            {
                // Unlock so that new things can be added to the
                // worklist while we are processing the current item:
                AutoUnlockMonitor unlock(*this);//解锁Lock
                task->executeFromWorker(workerId_, stackLimit);
                // unlock析构，锁住Lock
            }
        }

        if (state_ == TERMINATING)
            break;

        JS_ASSERT(state_ == ACTIVE);

        lock.wait();//等待condVar
    }

    JS_ASSERT(worklist_.empty() && state_ == TERMINATING);
    state_ = TERMINATED;
    lock.notify();//释放一个condVar，会在所有等待condVar的线程中选择一个允许运行。如果没有等待线程，空操作
    // lock析构，解锁Lock
}

bool
ThreadPoolWorker::submit(TaskExecutor *task)
{
    AutoLockMonitor lock(*this);//锁住Lock
    JS_ASSERT(state_ == ACTIVE);
    if (!worklist_.append(task))
        return false;
    lock.notify();//释放一个condVar
    return true;
    // lock析构，解锁Lock
}

void
ThreadPoolWorker::terminate()
{
    AutoLockMonitor lock(*this);//锁住Lock

    if (state_ == CREATED) {
        state_ = TERMINATED;
        return;
    } else if (state_ == ACTIVE) {
        state_ = TERMINATING;
        lock.notify();
        while (state_ != TERMINATED)
            lock.wait(); //等待condVar
    } else {
        JS_ASSERT(state_ == TERMINATED);
    }
    // lock析构，解锁Lock
}

/////////////////////////////////////////////////////////////////////////////
// ThreadPool
//
// The |ThreadPool| starts up workers, submits work to them, and shuts
// them down when requested.

// 在调用submit时，会先调用lazyStartWorkers()，如果线程池为空，则启动所有线程。在任何错误下，都会删除已有线程并报告失败。
ThreadPool::ThreadPool(JSRuntime *rt)
  : runtime_(rt),
    numWorkers_(0), // updated during init()
    nextId_(0)
{
}

bool
ThreadPool::init()
{
    // Compute the number of worker threads (which may legally
    // be zero, as described in ThreadPool.h).  This is not
    // done in the constructor because runtime_->useHelperThreads()
    // doesn't return the right thing then.

#ifdef JS_THREADSAFE
    if (runtime_->useHelperThreads())
        numWorkers_ = GetCPUCount() - 1;
    else
        numWorkers_ = 0;

    if (char *jsthreads = getenv("JS_THREADPOOL_SIZE"))
        numWorkers_ = strtol(jsthreads, NULL, 10);
#endif

    return true;
}

ThreadPool::~ThreadPool()
{
    terminateWorkers();
}

bool
ThreadPool::lazyStartWorkers(JSContext *cx)
{
    // Starts the workers if they have not already been started.  If
    // something goes wrong, reports an error and ensures that all
    // partially started threads are terminated.  Therefore, upon exit
    // from this function, the workers array is either full (upon
    // success) or empty (upon failure).

#ifndef JS_THREADSAFE
    return true;
#else
    if (!workers_.empty()) {
        JS_ASSERT(workers_.length() == numWorkers());
        return true;
    }

    // Allocate workers array and then start the worker threads.
    // Note that numWorkers_ is the number of *desired* workers,
    // but workers_.length() is the number of *successfully
    // initialized* workers.
    for (size_t workerId = 0; workerId < numWorkers(); workerId++) {
        ThreadPoolWorker *worker = js_new<ThreadPoolWorker>(workerId);
        if (!worker) {
            terminateWorkersAndReportOOM(cx);
            return false;
        }
        if (!worker->init() || !workers_.append(worker)) {
            js_delete(worker);
            terminateWorkersAndReportOOM(cx);
            return false;
        }
        if (!worker->start()) {
            // Note: do not delete worker here because it has been
            // added to the array and hence will be deleted by
            // |terminateWorkersAndReportOOM()|.
            terminateWorkersAndReportOOM(cx);
            return false;
        }
    }

    return true;
#endif
}

void
ThreadPool::terminateWorkersAndReportOOM(JSContext *cx)
{
    terminateWorkers();
    JS_ASSERT(workers_.empty());
    JS_ReportOutOfMemory(cx);
}

void
ThreadPool::terminateWorkers()
{
    while (workers_.length() > 0) {
        ThreadPoolWorker *worker = workers_.popCopy();
        worker->terminate();
        js_delete(worker);
    }
}

bool
ThreadPool::submitOne(JSContext *cx, TaskExecutor *executor)
{
    JS_ASSERT(numWorkers() > 0);

    runtime_->assertValidThread();//确保这个thread和runtime是可以access的？？？具体没看

    if (!lazyStartWorkers(cx))
        return false;

    // Find next worker in round-robin fashion.
    size_t id = JS_ATOMIC_INCREMENT(&nextId_) % numWorkers();
    return workers_[id]->submit(executor);
}

bool
ThreadPool::submitAll(JSContext *cx, TaskExecutor *executor)
{
    runtime_->assertValidThread();

    if (!lazyStartWorkers(cx))
        return false;

    for (size_t id = 0; id < numWorkers(); id++) {
        if (!workers_[id]->submit(executor))
            return false;
    }
    return true;
}

bool
ThreadPool::terminate()
{
    terminateWorkers();
    return true;
}
