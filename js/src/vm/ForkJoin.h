/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ForkJoin_h__
#define ForkJoin_h__

#include "jscntxt.h"
#include "vm/ThreadPool.h"
#include "jsgc.h"
#include "ion/Ion.h"

///////////////////////////////////////////////////////////////////////////
// Read Me First
//
// The ForkJoin abstraction:
// -------------------------
//
// This is the building block for executing multi-threaded JavaScript with
// shared memory (as distinct from Web Workers).  The idea is that you have
// some (typically data-parallel) operation which you wish to execute in
// parallel across as many threads as you have available.
// 这是使用共享内存执行多线程javascript的基础构件。
// 思路是：你有一些（通常是数据并行）的操作，希望能使用尽可能多的线程并行运行。
// 
// The ForkJoin abstraction is intended to be used by self-hosted code
// to enable parallel execution.  At the top-level, it consists of a native
// function (exposed as the ForkJoin intrinsic) that is used like so:
// ForkJoin抽象意在被self-hosted代码用来启用并行执行。
// 在顶层，它包含一个本地函数（作为ForkJoin的固有性质暴露？），函数这样使用：
//
//     ForkJoin(func, feedback)
// 
// The intention of this statement is to start N copies of |func()|
// running in parallel.  Each copy will then do 1/Nth of the total
// work.  Here N is number of workers in the threadpool (see
// ThreadPool.h---by default, N is the number of cores on the
// computer).
// 该语句的目的是开启func的N个拷贝，并行运行。每份拷贝会做所有工作的1/N.
// N是threadpool中工作线程的数目。
// N默认为计算机的核数。
//
// Typically, each of the N slices will execute from a different
// worker thread, but that is not something you should rely upon---if
// we implement work-stealing, for example, then it could be that a
// single worker thread winds up handling multiple slices.
// 通常，N个slice会分别从不同工作线程执行，但你不应该依赖它。
// 例如，如果我们实现了工作窃取，就有可能一个工作线程最终处理了多个slice
//
// The second argument, |feedback|, is an optional callback that will
// receiver information about how execution proceeded.  This is
// intended for use in unit testing but also for providing feedback to
// users.  Note that gathering the data to provide to |feedback| is
// not free and so execution will run somewhat slower if |feedback| is
// provided.
// 第二个参数feedback是一个可选callback，会接收执行情况的信息。
// 它的目的是使用在单元测试中，但也被用来提供反馈给用户。
// 注意收集数据提供给feedback不是无代价的，所以如果提供了feedback，执行会变慢。
//
// func() should expect the following arguments:
//
//     func(id, n, warmup)
//
// Here, |id| is the slice id. |n| is the total number of slices.  The
// parameter |warmup| is true for a *warmup or recovery phase*.
// Warmup phases are discussed below in more detail, but the general
// idea is that if |warmup| is true, |func| should only do a fixed
// amount of work.  If |warmup| is false, |func| should try to do all
// remaining work is assigned.
// id是slice的id，n是slice的总数目。
// warmup在 warmup or revovery 阶段是true。
// 如果warmup为真，func应该做固定数目的工作。
// 如果warmup为假，func应该尝试做完所有指派的工作。
//
// Note that we implicitly assume that |func| is tracking how much
// work it has accomplished thus far; some techniques for doing this
// are discussed in |ParallelArray.js|.
// 注意，我们隐含假定func会记录目前完成了多少工作。
// 一些相关技术在ParallelArray.js
//
// Warmups and Sequential Fallbacks
// --------------------------------
//
// ForkJoin can only execute code in parallel when it has been
// ion-compiled in Parallel Execution Mode. ForkJoin handles this part
// for you. However, because ion relies on having decent type
// information available, it is necessary to run the code sequentially
// for a few iterations first to get the various type sets "primed"
// with reasonable information.  We try to make do with just a few
// runs, under the hypothesis that parallel execution code which reach
// type stability relatively quickly.
//ForkJoin只能在被ion并行模式下编译过的情况下并行执行代码
//但是，因为ion依赖于合适的类型信息，所以需要先串行执行代码若干次，以准备好各种类型集
//在并行执行代码很快达到相对类型稳定的假设下，我们试着运行较少次数

// The general strategy of ForkJoin is as follows:
//
// - If the code has not yet been run, invoke `func` sequentially with
//   warmup set to true.  When warmup is true, `func` should try and
//   do less work than normal---just enough to prime type sets. (See
//   ParallelArray.js for a discussion of specifically how we do this
//   in the case of ParallelArray).
//如果代码没运行过，串行调用func（warmup设为true）。
//当warmup为true时，func应该尝试做比通常更少的工作，只需要足够准备类型集
//
// - Try to execute the code in parallel.  Parallel execution mode has
//   three possible results: success, fatal error, or bailout.  If a
//   bailout occurs, it means that the code attempted some action
//   which is not possible in parallel mode.  This might be a
//   modification to shared state, but it might also be that it
//   attempted to take some theoreticaly pure action that has not been
//   made threadsafe (yet?).
//尝试并行执行代码。
//并行执行模式有三种可能的结果：success，fatal error和bailout
//如果bailout发生，代表代码企图进行一些并行模式下不允许的动作。
//可能是一个共享状态修改，也可能是企图进行一些理论上可以但还未实现线程安全的动作。
//
// - If parallel execution is successful, ForkJoin returns true.
//如果successfull，返回true
//
// - If parallel execution results in a fatal error, ForkJoin returns false.
//如果fatal error，返回false
//
// - If parallel execution results in a *bailout*, this is when things
//   get interesting.  In that case, the semantics of parallel
//   execution guarantee us that no visible side effects have occurred
//   (unless they were performed with the intrinsic
//   |UnsafeSetElement()|, which can only be used in self-hosted
//   code).  We therefore reinvoke |func()| but with warmup set to
//   true.  The idea here is that often parallel bailouts result from
//   a failed type guard or other similar assumption, so rerunning the
//   warmup sequentially gives us a chance to recompile with more
//   data.  Because warmup is true, we do not expect this sequential
//   call to process all remaining data, just a chunk.  After this
//   recovery execution is complete, we again attempt parallel
//   execution.
//如果bailout，事情就有趣了
//并行执行的语义保证了没有副作用发生
//因此我们重新调用func但是将warmup设置为true
//这里的考虑是，通常bailout是由类型防护或类似的假设失败引起的，
//所以重新运行串行的warmup提供了一个用更多数据重新编译的机会
//因为warmup是true，我们不期望这次串行调用会处理所有剩余的数据，而只是一个chunk
//在recovery执行完毕后，我们继续尝试并行执行
//
// - If more than a fixed number of bailouts occur, we give up on
//   parallelization and just invoke |func()| N times in a row (once
//   for each worker) but with |warmup| set to false.
//如果超过某个次数的bailout发生，我们放弃并行，
//改为N次的连续调用func（每个线程一次），warmup设为false
//
// Operation callback:
//
// During parallel execution, |slice.check()| must be periodically
// invoked to check for the operation callback. This is automatically
// done by the ion-generated code. If the operation callback is
// necessary, |slice.check()| will arrange a rendezvous---that is, as
// each active worker invokes |check()|, it will come to a halt until
// everyone is blocked (Stop The World).  At this point, we perform
// the callback on the main thread, and then resume execution.  If a
// worker thread terminates before calling |check()|, that's fine too.
// We assume that you do not do unbounded work without invoking
// |check()|.
//并行执行过程中，slice.check()必须周期性地被调用，来检查操作的callback
//这是由ion生成的代码自动完成的
//如果操作的callback是需要的，slice.check()会安排一次rendezvous
//即，由于每个线程调用check(),它会停顿，知道所有线程都block（Stop The World）
//此时，我们完成在主线程上的callback，然后恢复执行
//如果一个工作线程在check之前终止，也是可以的
//我们假定你不会在不调用check()的情况下进行超界限的工作
//
// Transitive compilation:
//
// One of the challenges for parallel compilation is that we
// (currently) have to abort when we encounter an uncompiled script.
// Therefore, we try to compile everything that might be needed
// beforehand. The exact strategy is described in `ParallelDo::apply()`
// in ForkJoin.cpp, but at the highest level the idea is:
//并行编译的一个挑战是我们目前在遇到没编译的脚本时必须终止。
//因此，我们尝试提前编译所有可能用到的内容
//准确的策略在ForkJoin.cpp的ParallelDo::apply()中描述，但最顶层的思路是：
//
// 1. We maintain a flag on every script telling us if that script and
//    its transitive callees are believed to be compiled. If that flag
//    is set, we can skip the initial compilation.
//1.我们在每个脚本维护一个标志，告诉我们脚本和它的所有调用是否被相信可以编译。
//如果标志位被设置，我们可以跳过初始编译
// 2. Otherwise, we maintain a worklist that begins with the main
//    script. We compile it and then examine the generated parallel IonScript,
//    which will have a list of callees. We enqueue those. Some of these
//    compilations may take place off the main thread, in which case
//    we will run warmup iterations while we wait for them to complete.
//2.否则，我们维护一个任务列表，开始于主脚本。
//我们编译它然后检查生成的并行IonScript，它会有一个调用列表。将其入列。
//一些编译可能在主线程外发生，在此情况下我们会在等待完成的同时运行warmup循环
// 3. If the warmup iterations finish all the work, we're done.
// 4. If compilations fail, we fallback to sequential.
// 5. Otherwise, we will try running in parallel once we're all done.
//3.如果warmup循环结束了所有工作，我们完成了
//4.如果编译失败，我们退回串行
//5.否则，一旦完成，我们会尝试并行执行
//
// Bailout tracing and recording:
//
// When a bailout occurs, we record a bit of state so that we can
// recover with grace. Each |ForkJoinSlice| has a pointer to a
// |ParallelBailoutRecord| pre-allocated for this purpose. This
// structure is used to record the cause of the bailout, the JSScript
// which was executing, as well as the location in the source where
// the bailout occurred (in principle, we can record a full stack
// trace, but right now we only record the top-most frame). Note that
// the error location might not be in the same JSScript as the one
// which was executing due to inlining.
//当bailout发生，我们记录一些状态以便可以优雅的恢复。
//每个ForkJoinSlice有一个指向预先分配的ParallelBailoutRecord的指针
//该结构被用于记录bailout的原因，在执行的JSScript，以及bailout发生的源码位置
//原则上，我们可以记录整个堆栈跟踪，但目前只记录最上面的一个frame
//注意错误位置可能和在执行的JSScript是同一个，由于内联
//
// Bailout tracing and recording:
//
// When a bailout occurs, we have to record a bit of state so that we
// can recover with grace.  The caller of ForkJoin is responsible for
// passing in a.  This state falls into two categories: one is
// mandatory state that we track unconditionally, the other is
// optional state that we track only when we plan to inform the user
// about why a bailout occurred.
//ForkJoin的调用者有责任传入一个状态？
//状态可能是两种：一个是强制状态，我们无条件跟踪
//另一个是选择装惕啊，我们只在计划告知用户关于为什么发生bailout时跟踪
//
// The mandatory state consists of two things.
//
// - First, we track the top-most script on the stack.  This script
//   will be invalidated.  As part of ParallelDo, the top-most script
//   from each stack frame will be invalidated.
//
// - Second, for each script on the stack, we will set the flag
//   HasInvalidatedCallTarget, indicating that some callee of this
//   script was invalidated.  This flag is set as the stack is unwound
//   during the bailout.
//强制状态包括两个：
//- 首先，我们记录栈上最顶层的脚本。这个脚本会被设定为无效。
//  作为ParallelDo的一部分，每个栈帧最顶层的脚本会被设为无效
//- 第二，对栈上的每个脚本而言，
//  我们会设置HasInvalidatedCallTarget标志，代表这个脚本的某些调用是无效的
//  因为在bailout时栈是松散的
//
// The optional state consists of a backtrace of (script, bytecode)
// pairs.  The rooting on these is currently screwed up and needs to
// be fixed.
//选择状态包括一个（script，bytecode）对的回溯。
//这上面的rooting目前搞砸了，需要修复
//
// Garbage collection and allocation:
//
// Code which executes on these parallel threads must be very careful
// with respect to garbage collection and allocation.  The typical
// allocation paths are UNSAFE in parallel code because they access
// shared state (the compartment's arena lists and so forth) without
// any synchronization.  They can also trigger GC in an ad-hoc way.
// 通用的分配方式是不安全的。
//
// To deal with this, the forkjoin code creates a distinct |Allocator|
// object for each slice.  You can access the appropriate object via
// the |ForkJoinSlice| object that is provided to the callbacks.  Once
// the execution is complete, all the objects found in these distinct
// |Allocator| is merged back into the main compartment lists and
// things proceed normally.
// 给每个forkjoinslice分配一个allocator，结束后合并到主内存中
//
// In Ion-generated code, we will do allocation through the
// |Allocator| found in |ForkJoinSlice| (which is obtained via TLS).
// Also, no write barriers are emitted.  Conceptually, we should never
// need a write barrier because we only permit writes to objects that
// are newly allocated, and such objects are always black (to use
// incremental GC terminology).  However, to be safe, we also block
// upon entering a parallel section to ensure that any concurrent
// marking or incremental GC has completed.
// Allocator通过forkjoinslice存储，forkjoinslice通过TLS存储。
// 在生成的机器码中，通过allocator进行分配，不需要写屏障。
// 概念上，永不需要写屏障，因为我们只允许对新分配的对象进行写操作，而这些对象在GC中是black的（已扫描？？）。
// 但为了安全起见，我们在进入并行部分时阻塞，来确保任何并发的marking或者incremental GC已经完成.
//
// If the GC *is* triggered during parallel execution, it will
// redirect to the current ForkJoinSlice() and invoke requestGC() (or
// requestZoneGC).  This will cause an interrupt.  Once the interrupt
// occurs, we will stop the world and then re-trigger the GC to run
// it.
// 如果在并行执行时触发了GC，会重定向到当前的forkjoinslice然后调用requestGC或者requestZoneGC。
// 这会引起中断。一旦中断发生，我们stop the world然后重新触发GC。
// 
// Current Limitations:
//
// - The API does not support recursive or nested use.  That is, the
//   JavaScript function given to |ForkJoin| should not itself invoke
//   |ForkJoin()|. Instead, use the intrinsic |InParallelSection()| to
//   check for recursive use and execute a sequential fallback.
//   不支持递归，不能在传递给forkjoin的函数中再次调用forkjoin。使用InParallelSection来判断
// 
// - No load balancing is performed between worker threads.  That means that
//   the fork-join system is best suited for problems that can be slice into
//   uniform bits.
//   没有线程之间的负载均衡，所以最好把任务均匀分配
//
///////////////////////////////////////////////////////////////////////////

namespace js {

struct ForkJoinSlice;

bool ForkJoin(JSContext *cx, CallArgs &args);

// Returns the number of slices that a fork-join op will have when
// executed.
uint32_t ForkJoinSlices(JSContext *cx);

#ifdef DEBUG
struct IonLIRTraceData {
    uint32_t bblock;
    uint32_t lir;
    uint32_t execModeInt;
    const char *lirOpName;
    const char *mirOpName;
    JSScript *script;
    jsbytecode *pc;
};
#endif

// Parallel operations in general can have one of three states.  They may
// succeed, fail, or "bail", where bail indicates that the code encountered an
// unexpected condition and should be re-run sequentially.
// Different subcategories of the "bail" state are encoded as variants of
// TP_RETRY_*.
enum ParallelResult { TP_SUCCESS, TP_RETRY_SEQUENTIALLY, TP_RETRY_AFTER_GC, TP_FATAL };

///////////////////////////////////////////////////////////////////////////
// Bailout tracking

enum ParallelBailoutCause {
    ParallelBailoutNone,

    // compiler returned Method_Skipped
    ParallelBailoutCompilationSkipped,

    // compiler returned Method_CantCompile
    ParallelBailoutCompilationFailure,

    // the periodic interrupt failed, which can mean that either
    // another thread canceled, the user interrupted us, etc
    ParallelBailoutInterrupt,

    // an IC update failed
    ParallelBailoutFailedIC,

    // Heap busy flag was set during interrupt
    ParallelBailoutHeapBusy,

    ParallelBailoutMainScriptNotPresent,
    ParallelBailoutCalledToUncompiledScript,
    ParallelBailoutIllegalWrite,
    ParallelBailoutAccessToIntrinsic,
    ParallelBailoutOverRecursed,
    ParallelBailoutOutOfMemory,
    ParallelBailoutUnsupported,
    ParallelBailoutUnsupportedStringComparison,
    ParallelBailoutUnsupportedSparseArray,
};

struct ParallelBailoutTrace {
    JSScript *script;
    jsbytecode *bytecode;
};

// See "Bailouts" section in comment above.
struct ParallelBailoutRecord {
    JSScript *topScript;
    ParallelBailoutCause cause;

    // Eventually we will support deeper traces,
    // but for now we gather at most a single frame.
    static const uint32_t MaxDepth = 1;
    uint32_t depth;
    ParallelBailoutTrace trace[MaxDepth];

    void init(JSContext *cx);
    void reset(JSContext *cx);
    void setCause(ParallelBailoutCause cause,
                  JSScript *outermostScript,   // inliner (if applicable)
                  JSScript *currentScript,     // inlinee (if applicable)
                  jsbytecode *currentPc);
    void addTrace(JSScript *script,
                  jsbytecode *pc);
};

struct ForkJoinShared;

// 表示forkjoin时每个线程对应的任务
// 重要数据成员：ForkJoinShared类型
struct ForkJoinSlice
{
  public:
    // PerThreadData corresponding to the current worker thread.
    PerThreadData *perThreadData;

    // Which slice should you process? Ranges from 0 to |numSlices|.
    const uint32_t sliceId;

    // How many slices are there in total?
    const uint32_t numSlices;

    // Allocator to use when allocating on this thread.  See
    // |ion::ParFunctions::ParNewGCThing()|.  This should move into
    // |perThreadData|.
    Allocator *const allocator;

    // Bailout record used to record the reason this thread stopped executing
    ParallelBailoutRecord *const bailoutRecord;

#ifdef DEBUG
    // Records the last instr. to execute on this thread.
    IonLIRTraceData traceData;
#endif

    ForkJoinSlice(PerThreadData *perThreadData, uint32_t sliceId, uint32_t numSlices,
                  Allocator *arenaLists, ForkJoinShared *shared,
                  ParallelBailoutRecord *bailoutRecord);
    ~ForkJoinSlice();

    // True if this is the main thread, false if it is one of the parallel workers.
    bool isMainThread();

    // When the code would normally trigger a GC, we don't trigger it
    // immediately but instead record that request here.  This will
    // cause |ExecuteForkJoinOp()| to invoke |TriggerGC()| or
    // |TriggerZoneGC()| as appropriate once the par. sec. is
    // complete. This is done because those routines do various
    // preparations that are not thread-safe, and because the full set
    // of arenas is not available until the end of the par. sec.
    void requestGC(JS::gcreason::Reason reason);
    void requestZoneGC(JS::Zone *compartment, JS::gcreason::Reason reason);

    // During the parallel phase, this method should be invoked
    // periodically, for example on every backedge, similar to the
    // interrupt check.  If it returns false, then the parallel phase
    // has been aborted and so you should bailout.  The function may
    // also rendesvous to perform GC or do other similar things.
    //
    // This function is guaranteed to have no effect if both
    // runtime()->interrupt is zero.  Ion-generated code takes
    // advantage of this by inlining the checks on those flags before
    // actually calling this function.  If this function ends up
    // getting called a lot from outside ion code, we can refactor
    // it into an inlined version with this check that calls a slower
    // version.
    bool check();

    // Be wary, the runtime is shared between all threads!
    JSRuntime *runtime();

    // Acquire and release the JSContext from the runtime.
    JSContext *acquireContext();
    void releaseContext();

    // Check the current state of parallel execution.
    static inline ForkJoinSlice *Current();
    bool InWorldStoppedForGCSection();

    // Initializes the thread-local state.
    static bool InitializeTLS();

  private:
    friend class AutoRendezvous;
    friend class AutoSetForkJoinSlice;
    friend class AutoMarkWorldStoppedForGC;

    bool checkOutOfLine();

#if defined(JS_THREADSAFE) && defined(JS_ION)
    // Initialized by InitializeTLS()
    static unsigned ThreadPrivateIndex;
    static bool TLSInitialized;
#endif

    ForkJoinShared *const shared;

private:
    // Stack base and tip of this slice's thread, for Stop-The-World GC.
    gc::StackExtent *extent;

public:
    // Establishes tip for stack scan; call before yielding to GC.
    void recordStackExtent();

    // Establishes base for stack scan; call before entering parallel code.
    void recordStackBase(uintptr_t *baseAddr);
};

// Locks a JSContext for its scope. Be very careful, because locking a
// JSContext does *not* allow you to safely mutate the data in the
// JSContext unless you can guarantee that any of the other threads
// that want to access that data will also acquire the lock, which is
// generally not the case. For example, the lock is used in the IC
// code to allow us to atomically patch up the dispatch table, but we
// must be aware that other threads may be reading from the table even
// as we write to it (though they cannot be writing, since they must
// hold the lock to write).
class LockedJSContext
{
#if defined(JS_THREADSAFE) && defined(JS_ION)
    ForkJoinSlice *slice_;
#endif
    JSContext *cx_;

  public:
    LockedJSContext(ForkJoinSlice *slice)
#if defined(JS_THREADSAFE) && defined(JS_ION)
      : slice_(slice),
        cx_(slice->acquireContext())
#else
      : cx_(NULL)
#endif
    { }

    ~LockedJSContext() {
#if defined(JS_THREADSAFE) && defined(JS_ION)
        slice_->releaseContext();
#endif
    }

    operator JSContext *() { return cx_; }
    JSContext *operator->() { return cx_; }
};

static inline bool
ParallelJSActive()
{
#ifdef JS_THREADSAFE
    ForkJoinSlice *current = ForkJoinSlice::Current();
    return current != NULL && !current->InWorldStoppedForGCSection();
#else
    return false;
#endif
}

bool ParallelTestsShouldPass(JSContext *cx);

///////////////////////////////////////////////////////////////////////////
// Debug Spew

namespace parallel {

enum ExecutionStatus {
    // Parallel or seq execution terminated in a fatal way, operation failed
    ExecutionFatal,

    // Parallel exec failed and so we fell back to sequential
    ExecutionSequential,

    // We completed the work in seq mode before parallel compilation completed
    ExecutionWarmup,

    // Parallel exec was successful after some number of bailouts
    ExecutionParallel
};

enum SpewChannel {
    SpewOps,
    SpewCompile,
    SpewBailouts,
    NumSpewChannels
};

#if defined(DEBUG) && defined(JS_THREADSAFE) && defined(JS_ION)

bool SpewEnabled(SpewChannel channel);
void Spew(SpewChannel channel, const char *fmt, ...);
void SpewBeginOp(JSContext *cx, const char *name);
void SpewBailout(uint32_t count, HandleScript script, jsbytecode *pc,
                 ParallelBailoutCause cause);
ExecutionStatus SpewEndOp(ExecutionStatus status);
void SpewBeginCompile(HandleScript script);
ion::MethodStatus SpewEndCompile(ion::MethodStatus status);
void SpewMIR(ion::MDefinition *mir, const char *fmt, ...);
void SpewBailoutIR(uint32_t bblockId, uint32_t lirId,
                   const char *lir, const char *mir, JSScript *script, jsbytecode *pc);

#else

static inline bool SpewEnabled(SpewChannel channel) { return false; }
static inline void Spew(SpewChannel channel, const char *fmt, ...) { }
static inline void SpewBeginOp(JSContext *cx, const char *name) { }
static inline void SpewBailout(uint32_t count, HandleScript script,
                               jsbytecode *pc, ParallelBailoutCause cause) {}
static inline ExecutionStatus SpewEndOp(ExecutionStatus status) { return status; }
static inline void SpewBeginCompile(HandleScript script) { }
#ifdef JS_ION
static inline ion::MethodStatus SpewEndCompile(ion::MethodStatus status) { return status; }
static inline void SpewMIR(ion::MDefinition *mir, const char *fmt, ...) { }
#endif
static inline void SpewBailoutIR(uint32_t bblockId, uint32_t lirId,
                                 const char *lir, const char *mir,
                                 JSScript *script, jsbytecode *pc) { }

#endif // DEBUG && JS_THREADSAFE && JS_ION

} // namespace parallel
} // namespace js

/* static */ inline js::ForkJoinSlice *
js::ForkJoinSlice::Current()
{
#if defined(JS_THREADSAFE) && defined(JS_ION)
    return (ForkJoinSlice*) PR_GetThreadPrivate(ThreadPrivateIndex);
#else
    return NULL;
#endif
}

#endif // ForkJoin_h__
