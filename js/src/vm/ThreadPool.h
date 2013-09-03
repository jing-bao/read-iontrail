/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ThreadPool_h__
#define ThreadPool_h__

#include <stddef.h>
#include "mozilla/StandardInteger.h"
#include "js/Vector.h"
#include "jsalloc.h"

#ifdef JS_THREADSAFE
#  include "prtypes.h"
#  include "prlock.h"
#  include "prcvar.h"
#endif

struct JSContext;
struct JSRuntime;
struct JSCompartment;
class JSScript;

namespace js {

class ThreadPoolWorker;

typedef void (*TaskFun)(void *userdata, uint32_t workerId, uintptr_t stackLimit);

class TaskExecutor
{
  public:
    virtual void executeFromWorker(uint32_t workerId, uintptr_t stackLimit) = 0;
};

// ThreadPool used for parallel JavaScript execution as well as
// parallel compilation.  Unless you are building a new kind of
// parallel service, it is very likely that you do not wish to
// interact with the threadpool directly.  In particular, if you wish
// to execute JavaScript in parallel, you probably want to look at
// |js::ForkJoin| in |forkjoin.cpp|.
// Threadpool被用于并行javascript执行和并行编译。
// 除非你在创建一种新的并行服务，你不该和threadpool直接交互。
// 如果在执行并行javascript，你可能要看一下forkjoin
// 
// The ThreadPool always maintains a fixed pool of worker threads.
// You can query the number of worker threads via the method
// |numWorkers()|.  Note that this number may be zero (generally if
// threads are disabled, or when manually specified for benchmarking
// purposes).
// Threadpool维持固定的线程池。你可以通过numWorkers查询数量。
// 注意，这个数量可能为0.
// 
// You can either submit jobs in one of two ways.  The first is
// |submitOne()|, which submits a job to be executed by one worker
// thread (this will fail if there are no worker threads).  The job
// will be enqueued and executed by some worker (the current scheduler
// uses round-robin load balancing; something more sophisticated,
// e.g. a central queue or work stealing, might be better).
// 你可以通过两种方式提交工作。
// submitOne会把工作提交给一个线程来执行，如果没有线程可用则失败。
// 工作会被入队，然后被某个线程执行。
// 目前调度器是round-robin，更精细的调度器例如central queue或者work stealing可能更好
// 
// The second way to submit a job is using |submitAll()|---in this
// case, the job will be executed by all worker threads.  This does
// not fail if there are no worker threads, it simply does nothing.
// Of course, each thread may have any number of previously submitted
// things that they are already working on, and so they will finish
// those before they get to this job.  Therefore it is possible to
// have some worker threads pick up (and even finish) their piece of
// the job before others have even started.
// 第二种方式是submitAll，任务会被所有线程执行。如果没有线程可用，不会失败，只是不做任何事。
// 当然，每个线程可能有一些之前提交的任务正在执行，在开始这个任务之前，他们会结束那些任务。
// 所以可能一些线程已经开始甚至完成他们的工作，而另一些线程还未开始。
// 
class ThreadPool
{
  private:
    friend class ThreadPoolWorker;

    // Initialized at startup only:
    JSRuntime *const runtime_;
    
    // 所有工作线程ThreadPoolWorker的列表
    js::Vector<ThreadPoolWorker*, 8, SystemAllocPolicy> workers_;

    // Number of workers we will start, when we actually start them
    size_t numWorkers_;

    // Next worker for |submitOne()|. Atomically modified.
    uint32_t nextId_;
    
    // 创建numWorkers_个ThreadPoolWorker，增加到workers_管理，并运行每个线程的任务。如果有失败，要清理之前的线程
    bool lazyStartWorkers(JSContext *cx);
    void terminateWorkers();   
    // 在lazyStartWorkers中，如果创建线程失败，则调用这个函数，终止之前的线程，并报告失败
    void terminateWorkersAndReportOOM(JSContext *cx);

  public:
    ThreadPool(JSRuntime *rt);
    ~ThreadPool();
    
    // 计算numWorkers_
    bool init();

    // Return number of worker threads in the pool.
    size_t numWorkers() { return numWorkers_; }

    // See comment on class:
    // 计算出下一个线程号，将任务加入线程的worklist
    bool submitOne(JSContext *cx, TaskExecutor *executor);
    // 将任务加入所有线程的worklist
    bool submitAll(JSContext *cx, TaskExecutor *executor);

    // Wait until all worker threads have finished their current set
    // of jobs and then return.  You must not submit new jobs after
    // invoking |terminate()|.
    // 等待所有线程的所有worklist完成，然后终止线程。
    bool terminate();
};

} // namespace js

#endif // ThreadPool_h__
