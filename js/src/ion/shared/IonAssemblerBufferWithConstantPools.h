/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=79:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Communicator client code, released
 * March 31, 1998.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Marty Rosenberg <mrosenberg@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef jsion_ion_assembler_buffer_with_constant_pools_h__
#define jsion_ion_assembler_buffer_with_constant_pools_h__
#include "ion/shared/IonAssemblerBuffer.h"
#include "assembler/wtf/SegmentedVector.h"
#include "ion/IonSpewer.h"
namespace js {
namespace ion {
typedef SegmentedVector<BufferOffset, 512> LoadOffsets;

struct Pool {
    const int maxOffset;
    const int immSize;
    const int instSize;
    const int bias;

  private:
    const int alignment;

  public:
    const bool isBackref;
    const bool canDedup;
    // "other" is the backwards half of this pool, it is held in another pool structure
    Pool *other;
    uint8 *poolData;
    uint32 numEntries;
    uint32 buffSize;
    LoadOffsets loadOffsets;

    // When filling pools where the the size of an immediate is larger
    // than the size of an instruction, we find we're in a case where the distance between the
    // next instruction and the next pool slot is increasing!
    // Moreover, If we want to do fancy things like deduplicate pool entries at
    // dump time, we may not know the location in a pool (and thus the limiting load)
    // until very late.
    // Lastly, it may be beneficial to interleave the pools.  I have absolutely no idea
    // how that will work, but my suspicions are that it will be difficult.

    BufferOffset limitingUser;
    int limitingUsee;

    Pool(int maxOffset_, int immSize_, int instSize_, int bias_, int alignment_,
         bool isBackref_ = false, bool canDedup_ = false, Pool *other_ = NULL)
        : maxOffset(maxOffset_), immSize(immSize_), instSize(instSize),
          bias(bias_), alignment(alignment_),
          isBackref(isBackref_), canDedup(canDedup_), other(other_),
          poolData(static_cast<uint8 *>(malloc(8*immSize))), numEntries(0),
          buffSize(8), limitingUser(), limitingUsee(INT_MIN)
    {
    }
    static const int garbage=0xa5a5a5a5;
    Pool() : maxOffset(garbage), immSize(garbage), instSize(garbage), bias(garbage),
             alignment(garbage), isBackref(garbage), canDedup(garbage)
    {
    }
    // Sometimes, when we are adding large values to a pool, the limiting use may change.
    // Handle this case.  nextInst is the address of the
    void updateLimiter(BufferOffset nextInst) {
        int oldRange, newRange;
        if (isBackref) {
            // common expressions that are not subtracted: the location of the pool, ...
            oldRange = limitingUser.getOffset() - ((numEntries - limitingUsee) * immSize);
            newRange = nextInst.getOffset();
        } else {
            oldRange = (limitingUsee * immSize) - limitingUser.getOffset();
            newRange = (numEntries * immSize) - nextInst.getOffset();
        }
        if (!limitingUser.assigned() || newRange > oldRange) {
            // We have a new largest range!
            limitingUser = nextInst;
            limitingUsee = numEntries;
        }
    }
    // checkFull is called before any modifications have been made.
    // It is "if we were to add this instruction and pool entry,
    // would we be in an invalid state?".  If it is true, then it is in fact
    // time for a "pool dump".

    // poolOffset is the distance from the end of the current section to the end of the pool.
    //            For the last section of the pool, this will be the size of the footer
    //            For the first section of the pool, it will be the size of every other
    //            section and the footer
    // codeOffset is the instruction-distance from the pool to the beginning of the buffer.
    //            Since codeOffset only includes instructions, the number is the same for
    //            the beginning and end of the pool.
    // instOffset is the offset from the beginning of the buffer to the instruction that
    //            is about to be placed.
    bool checkFullBackref(int poolOffset, int codeOffset) {
        if (!limitingUser.assigned())
            return false;
        if (signed(limitingUser.getOffset() + bias - codeOffset + poolOffset + (numEntries - limitingUsee) * immSize) >= maxOffset)
            return true;
        return false;
    }

    // checkFull answers the question "If a pool were placed at poolOffset, would
    // any reference into the pool be out of range?". It is meant to be used as instructions
    // and elements are inserted, to determine if a saved perforation point needs to be used.

    bool checkFull(int poolOffset) {
        // Inserting an instruction into the stream can
        // push any of the pools out of range.
        // Similarly, inserting into a pool can push the pool entry out of range
        JS_ASSERT(!isBackref);
        // Not full if there aren't any uses.
        if (!limitingUser.assigned()) {
            return false;
        }
        // We're considered "full" when:
        // bias + abs(poolOffset + limitingeUsee * numEntries - limitingUser) + sizeof(other_pools) >= maxOffset
        if (poolOffset + limitingUsee * immSize - (limitingUser.getOffset() + bias) >= maxOffset) {
            return true;
        }
        return false;
    }

    // By the time this function is called, we'd damn well better know that this is going to succeed.
    uint32 insertEntry(uint8 *data, BufferOffset off) {
        if (numEntries == buffSize) {
            buffSize <<= 1;
            poolData = static_cast<uint8*>(realloc(poolData, immSize * buffSize));
            if (poolData == NULL) {
                buffSize = 0;
                return -1;
            }
        }
        memcpy(&poolData[numEntries * immSize], data, immSize);
        loadOffsets.append(off.getOffset());
        return numEntries++;
    }

    bool reset() {
        numEntries = 0;
        buffSize = 8;
        poolData = static_cast<uint8*>(malloc(buffSize * immSize));
        if (poolData == NULL)
            return false;
        other = new Pool(other->maxOffset, other->immSize, other->instSize, other->bias,
                         other->alignment, other->isBackref, other->canDedup);
        if (other == NULL)
            return false;
        new (&loadOffsets) LoadOffsets;

        limitingUser = BufferOffset();
        limitingUsee = -1;
        return true;

    }
    // WARNING: This will not always align values. It will only
    // align to the requirement of the pool. If the pool is empty,
    // there is nothing to be aligned, so it will not perform any alignment
    uint8* align(uint8 *ptr) {
        return (uint8*)align((uint32)ptr);
    }
    uint32 align(uint32 ptr) {
        if (numEntries == 0)
            return ptr;
        return (ptr + alignment-1) & ~(alignment-1);
    }
    uint32 forceAlign(uint32 ptr) {
        return (ptr + alignment-1) & ~(alignment-1);
    }
    bool isAligned(uint32 ptr) {
        return ptr == align(ptr);
    }
    int getAlignment() {
        return alignment;
    }
    
    uint32 addPoolSize(uint32 start) {
        start = align(start);
        start += immSize * numEntries;
        return start;
    }
    uint8 *addPoolSize(uint8 *start) {
        start = align(start);
        start += immSize * numEntries;
        return start;
    }
    uint32 getPoolSize() {
        return immSize * numEntries;
    }
};


template <int SliceSize, int InstBaseSize>
struct BufferSliceTail : public BufferSlice<SliceSize> {
    Pool *data;
    uint8 isBranch[(SliceSize + (InstBaseSize * 8 - 1)) / (InstBaseSize * 8)];
    bool isNatural : 1;
    BufferSliceTail *getNext() {
        return (BufferSliceTail *)this->next;
    }
    BufferSliceTail() : data(NULL) {
        memset(isBranch, 0, sizeof(isBranch));
    }
    void markNextAsBranch() {
        int idx = this->nodeSize / InstBaseSize;
        isBranch[idx >> 3] |= 1 << (idx & 0x7);
    }
};

// NOTE: Adding in the ability to retroactively insert a pool has consequences!
// Most notably, Labels stop working.  Normally, we create a label, later bind it.
// when the label is bound, We back-patch all previous references to the label with
// the correct offset. However, since a pool may be retroactively inserted, we don't
// actually know what the final offset is going to be until much later. This will
// happen (in some instances) after the pools have been finalized. Attempting to compute
// the correct offsets for branches as the pools are finalized is quite infeasible.
// Instead, I write *just* the number of instructions that will be jumped over, then
// when we go to copy the instructions into the executable buffer, fix up all of the
// offsets to include the pools. Since we have about 32 megabytes worth of offset,
// I am not very worried about the pools moving it out of range.
// Now, How exactly do we generate these? The first step is to identify which
// instructions are actually branches that need to be fixed up.  A single bit
// per instruction should be enough to determine which ones are branches, but
// we have no guarantee that all instructions are the same size, so the start of
// each branch instruction will be marked with a bit (1 bit per byte).
// then we neet to call up to the assembler to determine what the offset of the branch
// is. The offset will be the number of instructions that are being skipped over
// along with any processor bias. We then need to calculate the offset, including pools
// and write that value into the buffer.  At this point, we can write it into the
// executable buffer, or the AssemblerBuffer, and copy the data over later.
// Previously, This was all handled by the assembler, since the location
// and size of pools were always known as soon as its location had been reached.

// A class for indexing into constant pools.
// Each time a pool entry is added, one of these is generated.
// This can be supplied to read and write that entry after the fact.
// And it can be used to get the address of the entry once the buffer
// has been finalized, and an executable copy allocated.
template <int SliceSize, int InstBaseSize, class Inst, class Asm, int poolKindBits>
struct AssemblerBufferWithConstantPool : public AssemblerBuffer<SliceSize, Inst> {
  private:
    int entryCount[1 << poolKindBits];
    static const int offsetBits = 32 - poolKindBits;
  public:
    class PoolEntry {
        template <int ss, int ibs, class i, class a, int pkb>
        friend struct AssemblerBufferWithConstantPool;
        uint32 offset_ : offsetBits;
        uint32 kind_ : poolKindBits;
        PoolEntry(int offset, int kind) : kind_(kind), offset_(offset) {
        }
      public:
        uint32 encode() {
            uint32 ret;
            memcpy(&ret, this, sizeof(uint32));
            return ret;
        }
        PoolEntry(uint32 bits) : offset_((1 << offsetBits) - 1 & bits),
                                 kind_(bits >> offsetBits) {
        }
        PoolEntry() : offset_(-1), kind_(-1) {
        }

        uint32 poolKind() const {
            return kind_;
        }
        uint32 offset() const {
            return offset_;
        }
    };
  private:
    typedef BufferSliceTail<SliceSize, InstBaseSize> BufferSlice;
    typedef AssemblerBuffer<SliceSize, Inst> Parent;

    // The size of a guard instruction
    const int guardSize;
    // The size of the header that is put at the beginning of a full pool
    const int headerSize;
    // The size of a footer that is put in a pool after it is full.
    const int footerSize;
    // the number of sub-pools that we can allocate into.
    const int numPoolKinds;

    Pool *pools;

    // The buffer should be aligned to this address.
    const int instBufferAlign;

    // the number of times we've dumped the pool.
    int numDumps;
    struct PoolInfo {
        int offset; // the number of instructions before the start of the pool
        int size;   // the size of the pool, including padding
        int finalPos; // the end of the buffer, in bytes from the beginning of the buffer
        BufferSlice *slice;
    };
    PoolInfo *poolInfo;
    // we need to keep track of how large the pools are, so we can allocate
    // enough space for them later.  This should include any amount of padding
    // necessary to keep the pools aligned.
    int poolSize;
    // The Assembler should set this to true if it does not want us to dump a pool here
    int canNotPlacePool;
    // Are we filling up the forwards or backwards pools?
    bool inBackref;
    // Cache the last place we saw an opportunity to dump the pool
    BufferOffset perforation;
    BufferSlice *perforatedNode;

    static const int logBasePoolInfo = 3;
    BufferSlice ** getHead() {
        return (BufferSlice**)&this->head;
    }
    BufferSlice ** getTail() {
        return (BufferSlice**)&this->tail;
    }

    virtual BufferSlice *newSlice() {
        BufferSlice *tmp = static_cast<BufferSlice*>(malloc(sizeof(BufferSlice)));
        if (!tmp) {
            this->m_oom = true;
            return NULL;
        }
        new (tmp) BufferSlice;
        return tmp;
    }
  public:
    AssemblerBufferWithConstantPool(int guardSize_, int headerSize_, int footerSize_, int numPoolKinds_, Pool *pools_, int instBufferAlign_)
        : guardSize(guardSize_), headerSize(headerSize_),
          footerSize(footerSize_), numPoolKinds(numPoolKinds_),
          pools(pools_),
          instBufferAlign(instBufferAlign_), numDumps(0),
          poolInfo(static_cast<PoolInfo*>(calloc(sizeof(PoolInfo), 1 << logBasePoolInfo))),
          poolSize(0), canNotPlacePool(0), inBackref(false),
          perforatedNode(NULL)
    {
        for (int idx = 0; idx < numPoolKinds; idx++) {
            entryCount[idx] = 0;
        }
    }
    const PoolInfo & getInfo(int x) const {
        static const PoolInfo nil = {0,0,0};
        if (x < 0 || x >= numDumps)
            return nil;
        return poolInfo[x];
    }
    void executableCopy(uint8 *dest_) {
        if (this->oom())
            return;
        // TODO: only do this when the pool actually has a value in it
        flushPool();
        for (int idx = 0; idx < numPoolKinds; idx++) {
            JS_ASSERT(pools[idx].numEntries == 0 && pools[idx].other->numEntries == 0);
        }
        typedef uint8 Chunk[InstBaseSize];
        Chunk *start = (Chunk*)dest_;
        Chunk *dest = (Chunk*)(((uint32)dest_ + instBufferAlign - 1) & ~(instBufferAlign -1));
        int curIndex = 0;
        int curInstOffset = 0;
        JS_ASSERT(start == dest);
        for (BufferSlice * cur = *getHead(); cur != NULL; cur = cur->getNext()) {
            Chunk *src = (Chunk*)cur->instructions;
            for (unsigned int idx = 0; idx <cur->size()/InstBaseSize;
                 idx++, curInstOffset += InstBaseSize) {
                // Is the current instruction a branch?
                if (cur->isBranch[idx >> 3] & (1<<(idx&7))) {
                    // It's a branch.  fix up the branchiness!
                    patchBranch((Inst*)&src[idx], curIndex, BufferOffset(curInstOffset));
                }
                memcpy(&dest[idx], &src[idx], sizeof(Chunk));
            }
            dest+=cur->size()/InstBaseSize;
            if (cur->data != NULL) {
                // have the repatcher move on to the next pool
                curIndex ++;
                // loop over all of the pools, copying them into place.
                uint8 *poolDest = (uint8*)dest;
                Asm::writePoolHeader(poolDest, cur->data, cur->isNatural);
                poolDest += headerSize;
                for (int idx = 0; idx < numPoolKinds; idx++) {
                    Pool *curPool = &cur->data[idx];
                    // align the pool.
                    poolDest = curPool->align(poolDest);
                    memcpy(poolDest, curPool->poolData, curPool->immSize * curPool->numEntries);
                    poolDest += curPool->immSize * curPool->numEntries;
                }
                // now go over the whole list backwards, and copy in the reverse portions
                for (int idx = numPoolKinds-1; idx >= 0; idx--) {
                    Pool *curPool = cur->data[idx].other;
                    // align the pool.
                    poolDest = curPool->align(poolDest);
                    memcpy(poolDest, curPool->poolData, curPool->immSize * curPool->numEntries);
                    poolDest += curPool->immSize * curPool->numEntries;
                }
                // write a footer in place
                Asm::writePoolFooter(poolDest, cur->data, cur->isNatural);
                poolDest += footerSize;
                // at this point, poolDest had better still be aligned to a chunk boundary.
                dest = (Chunk*) poolDest;
            }
        }
    }

    BufferOffset insertEntry(uint32 instSize, uint8 *inst, Pool *p, uint8 *data, PoolEntry *pe = NULL) {
        if (this->oom())
            return BufferOffset();
        int token;
        // insert the pool value
        if (inBackref)
            token = insertEntryBackwards(instSize, inst, p, data);
        else
            token = insertEntryForwards(instSize, inst, p, data);
        // now to get an instruction to write
        PoolEntry retPE;
        if (p != NULL) {
            int poolId = p - pools;
            IonSpew(IonSpew_Pools, "Inserting entry (token %d) into pool %d", token, poolId);
            Asm::insertTokenIntoTag(instSize, inst, token);
            JS_ASSERT(poolId < (1 << poolKindBits));
            JS_ASSERT(poolId >= 0);
            // Figure out the offset within like-kinded pool entries
            retPE = PoolEntry(entryCount[poolId], poolId);
            entryCount[poolId]++;
        }
        // Now inst is a valid thing to insert into the instruction stream
        if (pe != NULL)
            *pe = retPE;
        return this->putBlob(instSize, inst);
    }

    uint32 insertEntryBackwards(uint32 instSize, uint8 *inst, Pool *p, uint8 *data) {
        // unlike the forward case, inserting an instruction without inserting
        // anything into a pool after a pool has been placed, we don't affect
        // anything relevant, so we can skip this check entirely!

        if (p == NULL)
            return INT_MIN;
        // TODO: calculating offsets for the alignment requirements is *hard*
        // Instead, assume that we always add the maximum.
        int codeOffset = this->size() + instSize - perforation.getOffset();
        int poolOffset = footerSize;
        Pool *cur, *tmp;
        // NOTE: we want to process the pools from last to first.
        // Since the last pool is pools[0].other, and the first pool
        // is pools[numPoolKinds-1], we actually want to process this
        // forwards.
        for (cur = &pools[numPoolKinds-1]; cur >= pools; cur--) {
            // fetch the pool for the backwards half.
            tmp = cur->other;
            if (p == cur)
                tmp->updateLimiter(this->nextOffset());

            if (tmp->checkFullBackref(poolOffset, perforation.getOffset())) {
                // uh-oh, the backwards pool is full.  Time to finalize it, and
                // switch to a new forward pool.
                this->finishPool();
                return this->insertEntryForwards(instSize, inst, p, data);
            }
            // when moving back to front, calculating the alignment is hard, just be
            // conservative with it.
            poolOffset += tmp->immSize * tmp->numEntries + tmp->getAlignment();
            if (p == tmp) {
                poolOffset += tmp->immSize;
            }
            poolOffset += tmp->immSize * tmp->numEntries + tmp->getAlignment();
        }
        return p->numEntries + p->other->insertEntry(data, this->nextOffset());
    }

    // Simultaneously insert an instSized instruction into the stream,
    // and an entry into the pool.  There are many things that can happen.
    // 1) the insertion goes as planned
    // 2) inserting an instruction pushes a previous pool-reference out of range, forcing a dump
    // 2a) there isn't a reasonable save point in the instruction stream. We need to save room for
    //     a guard instruction to branch over the pool.
    int insertEntryForwards(uint32 instSize, uint8 *inst, Pool *p, uint8 *data) {
        // Advance the "current offset" by an inst, so everyone knows what their offset should be.
        uint32 nextOffset = this->size() + instSize;
        uint32 poolOffset = nextOffset;
        Pool *tmp;
        // If we need a guard instruction, reserve space for that.
        if (!perforatedNode)
            poolOffset += guardSize;
        // Also, take into account the size of the header that will be placed *after*
        // the guard instruction
        poolOffset += headerSize;

        // Perform the necessary range checks.
        for (tmp = pools; tmp < &pools[numPoolKinds]; tmp++) {
            // The pool may wish for a particular alignment, Let's give it one.
            JS_ASSERT((tmp->getAlignment() & (tmp->getAlignment() - 1)) == 0);
            // The pool only needs said alignment *if* there are any entries in the pool
            // WARNING: the pool needs said alignment if there are going to be entries in
            // the pool after this entry has been inserted
            if (p == tmp)
                poolOffset = tmp->forceAlign(poolOffset);
            else
                poolOffset = tmp->align(poolOffset);

            // If we're at the pool we want to insert into, find a new limiter
            // before we do the range check.
            if (p == tmp) {
                p->updateLimiter(BufferOffset(nextOffset));
            }
            if (tmp->checkFull(poolOffset)) {
                // uh-oh. DUMP DUMP DUMP
                this->dumpPool();
                return this->insertEntryBackwards(instSize, inst, p, data);
            }
            // include the size of this pool in the running total
            if (p == tmp) {
                nextOffset += tmp->immSize;
            }
            nextOffset += tmp->immSize * tmp->numEntries;
        }
        if (p == NULL) {
            return INT_MIN;
        }
        return p->insertEntry(data, this->nextOffset());
    }
    BufferOffset putInt(uint32 value) {
        return insertEntry(sizeof(uint32) / sizeof(uint8), (uint8*)&value, NULL, NULL);
    }
    // Mark the current section as an area where we can
    // later go to dump a pool
    void perforate() {
        // If we're filling the backrefrences, we don't want to start looking for a new dumpsite.
        if (inBackref)
            return;
        if (canNotPlacePool)
            return;
        // If there is nothing in the pool, then it is strictly disadvantageous
        // to attempt to place a pool here
        bool empty = true;
        for (int i = 0; i < numPoolKinds; i++) {
            if (pools[i].numEntries != 0) {
                empty = false;
                break;
            }
        }
        if (empty)
            return;
        perforatedNode = *getTail();
        perforation = this->nextOffset();
        Parent::perforate();
        IonSpew(IonSpew_Pools, "Adding a perforation at offset %d", perforation.getOffset());
    }

    // After a pool is finished, no more elements may be added to it. During this phase, we
    // will know the exact offsets to the pool entries, and those values should be written into
    // the given instructions.
    PoolInfo getPoolData() const {
        int prevOffset = getInfo(numDumps-1).offset;
        int prevEnd = getInfo(numDumps-1).finalPos;
        // calculate the offset of the start of this pool;
        int perfOffset = perforation.assigned() ?
            perforation.getOffset() :
            this->nextOffset().getOffset() + this->guardSize;
        int initOffset = prevEnd + (perfOffset - prevOffset);
        int finOffset = initOffset;
        bool poolIsEmpty = true;
        for (int poolIdx = 0; poolIdx < numPoolKinds; poolIdx++) {
            if (pools[poolIdx].numEntries != 0) {
                poolIsEmpty = false;
                break;
            }
            if (pools[poolIdx].other != NULL && pools[poolIdx].other->numEntries != 0) {
                poolIsEmpty = false;
                break;
            }
        }
        if (!poolIsEmpty) {
            finOffset += headerSize;
            for (int poolIdx = 0; poolIdx < numPoolKinds; poolIdx++) {
                finOffset=pools[poolIdx].align(finOffset);
                finOffset+=pools[poolIdx].numEntries * pools[poolIdx].immSize;
            }
            // And compute the necessary adjustments for the second half of the pool.
            for (int poolIdx = numPoolKinds-1; poolIdx >= 0; poolIdx--) {
                finOffset=pools[poolIdx].other->align(finOffset);
                finOffset+=pools[poolIdx].other->numEntries * pools[poolIdx].other->immSize;
            }
            finOffset += footerSize;
        }

        PoolInfo ret;
        ret.offset = perfOffset;
        ret.size = finOffset - initOffset;
        ret.finalPos = finOffset;
        ret.slice = perforatedNode;
        return ret;
    }
    void finishPool() {
        // This function should only be called while the backwards half of the pool
        // is being filled in. The backwards half of the pool is always in a state
        // where it is sane. Everything that needs to be done here is for "sanity's sake".
        // The per-buffer pools need to be reset, and we need to record the size of the pool.
        IonSpew(IonSpew_Pools, "Finishing pool %d", numDumps);
        JS_ASSERT(inBackref);
        PoolInfo newPoolInfo = getPoolData();
        if (newPoolInfo.size == 0) {
            // The code below also creates a new pool, but that is not necessary, since
            // the pools have not been modified at all.
            new (&perforation) BufferOffset();
            perforatedNode = NULL;
            inBackref = false;
            IonSpew(IonSpew_Pools, "Aborting because the pool is empty");
            // Bail out early, since we don't want to even pretend these pools exist.
            return;
        }
        JS_ASSERT(perforatedNode != NULL);
        if (numDumps >= (1<<logBasePoolInfo) && (numDumps & (numDumps-1)) == 0) {
            // need to resize.
            poolInfo = static_cast<PoolInfo*>(realloc(poolInfo, sizeof(PoolInfo) * numDumps * 2));
            if (poolInfo == NULL) {
                this->fail_oom();
                return;
            }
        }
        poolInfo[numDumps] = newPoolInfo;
        poolSize += poolInfo[numDumps].size;
        numDumps++;

        // In order to figure out how to fix up the loads for the second half of the pool
        // we need to find where the bits of the pool that have been implemented end.
        int poolOffset = perforation.getOffset();
        int magicAlign = getInfo(numDumps-2).finalPos - getInfo(numDumps-2).offset;
        poolOffset += magicAlign;
        poolOffset += headerSize;
        for (int poolIdx = 0; poolIdx < numPoolKinds; poolIdx++) {
            poolOffset=pools[poolIdx].align(poolOffset);
            poolOffset+=pools[poolIdx].numEntries * pools[poolIdx].immSize;
        }
        // All of the pool loads referred to by this code are going to
        // need fixing up here.
        for (int poolIdx = numPoolKinds-1; poolIdx >= 0; poolIdx--) {
            Pool *p =  pools[poolIdx].other;
            JS_ASSERT(p != NULL);
            int idx = 0;
            // Hacks on top of Hacks!
            // the patching code takes in the address of the instruction to be patched,
            // and the "address" of the element in the pool that we want to load.
            // However, since the code isn't actually in an array, we need to lie about
            // the address that the pool is in. Furthermore, since the offsets are
            // technically from the beginning of the FORWARD reference section, we have
            // to lie to ourselves about where this pool starts in order to make sure
            // the distance into the pool is interpreted correctly.
            // There is a more elegant way to fix this that will need to be implemented
            // eventually. We will want to provide the fixup function with a method to
            // convert from a 'token' into a pool offset.
            poolOffset = p->align(poolOffset);
            int fakePoolOffset = poolOffset - pools[poolIdx].numEntries * pools[poolIdx].immSize;
            for (LoadOffsets::Iterator iter = p->loadOffsets.begin();
                 iter != p->loadOffsets.end(); ++iter, ++idx)
            {

                IonSpew(IonSpew_Pools, "Linking entry %d in pool %d", idx, poolIdx);
                JS_ASSERT(iter->getOffset() >= perforation.getOffset());
                // Everything here is known, we can safely do the necessary substitutions
                Inst * inst = this->getInst(*iter);
                // Manually compute the offset, including a possible bias.
                // Also take into account the whole size of the pool that is being placed.
                int codeOffset = fakePoolOffset - iter->getOffset() - newPoolInfo.size;
                // That is, patchConstantPoolLoad wants to be handed the address of the
                // pool entry that is being loaded.  We need to do a non-trivial amount
                // of math here, since the pool that we've made does not actually reside there
                // in memory.
                IonSpew(IonSpew_Pools, "Fixing offset to %d", codeOffset - magicAlign);
                Asm::patchConstantPoolLoad(inst, (uint8*)inst + codeOffset - magicAlign);
            }
            poolOffset += p->numEntries * p->immSize;
        }
        // bind the current pool to the perforation point.
        Pool **tmp = &perforatedNode->data;
        *tmp = static_cast<Pool*>(malloc(sizeof(Pool) * numPoolKinds));
        if (tmp == NULL) {
            this->fail_oom();
            return;
        }
        memcpy(*tmp, pools, sizeof(Pool) * numPoolKinds);

        // reset everything to the state that it was in when we started
        for (int poolIdx = 0; poolIdx < numPoolKinds; poolIdx++) {
            if (!pools[poolIdx].reset()) {
                this->fail_oom();
                return;
            }
        }
        new (&perforation) BufferOffset();
        perforatedNode = NULL;
        inBackref = false;
    }

    void dumpPool() {
        JS_ASSERT(!inBackref);
        IonSpew(IonSpew_Pools, "Attempting to dump the pool");
        PoolInfo newPoolInfo = getPoolData();
        if (newPoolInfo.size == 0) {
            // If there is no data in the pool being dumped, don't dump anything.
            inBackref = true;
            IonSpew(IonSpew_Pools, "Abort, no pool data");
            return;
        }

        IonSpew(IonSpew_Pools, "Dumping %d bytes", newPoolInfo.size);
        if (!perforation.assigned()) {
            IonSpew(IonSpew_Pools, "No Perforation point selected, generating a new one");
            // There isn't a perforation here, we need to dump the pool with a guard.
            BufferOffset branch = this->nextOffset();
            this->markNextAsBranch();
            this->putBlob(guardSize, NULL);
            BufferOffset afterPool = this->nextOffset();
            Asm::writePoolGuard(branch, this->getInst(branch), afterPool);
            markGuard();
            perforatedNode->isNatural = false;
        } else {
            perforatedNode->isNatural = true;
        }

        // We have a perforation.  Time to cut the instruction stream, patch in the pool
        // and possibly re-arrange the pool to accomodate its new location.
        int poolOffset = perforation.getOffset();
        int magicAlign =  getInfo(numDumps-1).finalPos - getInfo(numDumps-1).offset;
        poolOffset += magicAlign;
        poolOffset += headerSize;
        for (int poolIdx = 0; poolIdx < numPoolKinds; poolIdx++) {
            bool beforePool = true;
            Pool *p = &pools[poolIdx];
            // align the pool offset to the alignment of this pool
            poolOffset = p->align(poolOffset);
            // Any entries that happened to be after the place we put our pool will need to be
            // switched from the forward-referenced pool to the backward-refrenced pool.
            int idx = 0;
            for (LoadOffsets::Iterator iter = p->loadOffsets.begin();
                 iter != p->loadOffsets.end(); ++iter, ++idx)
            {
                if (iter->getOffset() >= perforation.getOffset()) {
                    IonSpew(IonSpew_Pools, "Pushing entry %d in pool %d into the backwards section.", idx, poolIdx);
                    // insert this into the rear part of the pool.
                    int offset = idx * p->immSize;
                    p->other->insertEntry(&p->poolData[offset], BufferOffset(*iter));
                    // Update the current pool to report fewer entries.  They are now in the
                    // backwards section.
                    p->numEntries--;
                    beforePool = false;
                } else {
                    JS_ASSERT(beforePool);
                    IonSpew(IonSpew_Pools, "Entry %d in pool %d is before the pool.", idx, poolIdx);
                    // Everything here is known, we can safely do the necessary substitutions
                    Inst * inst = this->getInst(*iter);
                    // We need to manually compute the offset, including a possible bias.
                    int codeOffset = poolOffset - iter->getOffset();
                    // That is, patchConstantPoolLoad wants to be handed the address of the
                    // pool entry that is being loaded.  We need to do a non-trivial amount
                    // of math here, since the pool that we've made does not actually reside there
                    // in memory.
                    IonSpew(IonSpew_Pools, "Fixing offset to %d", codeOffset - magicAlign);
                    Asm::patchConstantPoolLoad(inst, (uint8*)inst + codeOffset - magicAlign);
                }
            }
            // Some number of entries have been positively identified as being
            // in this section of the pool. Before processing the next pool,
            // update the offset from the beginning of the buffer
            poolOffset += p->numEntries * p->immSize;
        }
        inBackref = true;
    }

    void flushPool() {
        if (this->oom())
            return;
        if (!inBackref)
            dumpPool();
        finishPool();
    }
    void patchBranch(Inst *i, int curpool, BufferOffset branch) {
        const Inst *ci = i;
        ptrdiff_t offset = Asm::getBranchOffset(ci);
        // If the offset is 0, then there is nothing to do.
        if (offset == 0)
            return;
        int destOffset = branch.getOffset() + offset;
        if (offset > 0) {
            while (poolInfo[curpool].offset <= destOffset && curpool < numDumps) {
                offset += poolInfo[curpool].size;
                curpool++;
            }
        } else {
            // Ignore the pool that comes next, since this is a backwards branch
            curpool--;
            while (curpool >= 0 && poolInfo[curpool].offset > destOffset) {
                offset -= poolInfo[curpool].size;
                curpool--;
            }
            // Can't assert anything here, since the first pool may be after the target.
        }
        Asm::retargetNearBranch(i, offset);
    }

    // Mark the next instruction as a valid guard.  This means we can place a pool here.
    void markGuard() {
        // If we are in a no pool zone then there is no point in dogearing
        // this branch as a place to go back to
        if (canNotPlacePool)
            return;
        // There is no point in trying to grab a new slot if we've already
        // found one and are in the process of filling it in.
        if (inBackref)
            return;
        perforate();
    }
    void enterNoPool() {
        if (!canNotPlacePool && !perforation.assigned()) {
            // Embarassing mode: The Assembler requests the start of a no pool section
            // and there have been no valid places that a pool could be dumped thusfar.
            // If a pool were to fill up before this no-pool section ends, we need to go back
            // in the stream and enter a pool guard after the fact.  This is feasable, but
            // for now, it is easier to just allocate a junk instruction, default it to a nop, and
            // finally, if the pool *is* needed, patch the nop to  apool guard.
            // What the assembler requests:

            // #request no-pool zone
            // push pc
            // blx r12
            // #end no-pool zone

            // however, if we would need to insert a pool, and there is no perforation point...
            // so, actual generated code:

            // b next; <= perforation point
            // next:
            // #beginning of no pool zone
            // push pc
            // blx r12

            BufferOffset branch = this->nextOffset();
            this->markNextAsBranch();
            this->putBlob(guardSize, NULL);
            BufferOffset afterPool = this->nextOffset();
            Asm::writePoolGuard(branch, this->getInst(branch), afterPool);
            markGuard();
        }
        canNotPlacePool++;
    }
    void leaveNoPool() {
        canNotPlacePool--;
    }
    int size() const {
        return uncheckedSize();
    }
    Pool *getPool(int idx) {
        return &pools[idx];
    }
    void markNextAsBranch() {
        JS_ASSERT(*this->getTail() != NULL);
        // If the previous thing inserted was the last instruction of
        // the node, then whoops, we want to mark the first instruction of
        // the next node.
        this->ensureSpace(InstBaseSize);
        (*this->getTail())->markNextAsBranch();
    }
    int uncheckedSize() const {
        PoolInfo pi = getPoolData();
        int codeEnd = this->nextOffset().getOffset();
        return (codeEnd - pi.offset) + pi.finalPos;
    }
    ptrdiff_t curDumpsite;
    void resetCounter() {
        curDumpsite = 0;
    }
    ptrdiff_t poolSizeBefore(ptrdiff_t offset) const {
        int cur = 0;
        while(cur < numDumps && poolInfo[cur].offset <= offset)
            cur++;
        // poolInfo[curDumpsite] is now larger than the offset
        // either this is the first one, or the previous is the last one we care about
        if (cur == 0)
            return 0;
        return poolInfo[cur-1].finalPos - poolInfo[cur-1].offset;
    }

  private:
    void getPEPool(PoolEntry pe, Pool **retP, int32 * retOffset, int32 *poolNum) const {
        int poolKind = pe.poolKind();
        Pool *p = NULL;
        uint32 offset = pe.offset() * pools[poolKind].immSize;
        int idx;
        for (idx = 0; idx < numDumps; idx++) {
            p = &poolInfo[idx].slice->data[poolKind];
            if (p->getPoolSize() > offset)
                break;
            offset -= p->getPoolSize();
            p = p->other;
            if (p->getPoolSize() > offset)
                break;
            offset -= p->getPoolSize();
            p = NULL;
        }
        if (poolNum != NULL)
            *poolNum = idx;
        // If this offset is contained in any finished pool, forward or backwards, p now
        // points to that pool, if it is not in any pool (should be in the currently building pool)
        // then p is NULL.
        if (p == NULL) {
            p = &pools[poolKind];
            if (offset >= p->getPoolSize()) {
                p = p->other;
                offset -= p->getPoolSize();
            }
        }
        JS_ASSERT(p != NULL);
        JS_ASSERT(offset < p->getPoolSize());
        *retP = p;
        *retOffset = offset;
    }
    uint8 *getPoolEntry(PoolEntry pe) {
        Pool *p;
        int32 offset;
        getPEPool(pe, &p, &offset, NULL);
        return &p->poolData[offset];
    }
    size_t getPoolEntrySize(PoolEntry pe) {
        int idx = pe.poolKind();
        return pools[idx].immSize;
    }

  public:
    uint32 poolEntryOffset(PoolEntry pe) const {
        Pool *realPool;
        // offset is in bytes, not entries.
        int32 offset;
        int32 poolNum;
        getPEPool(pe, &realPool, &offset, &poolNum);
        PoolInfo *pi = &poolInfo[poolNum];
        Pool *poolGroup = pi->slice->data;
        uint32 start = pi->finalPos - pi->size + headerSize;
        /// The order of the pools is:
        // A B C C_Rev B_Rev A_Rev, so in the initial pass,
        // go through the pools forwards, and in the second pass
        // go through them in reverse order.
        for (int idx = 0; idx < numPoolKinds; idx++) {
            if (&poolGroup[idx] == realPool) {
                return start + offset;
            }
            start = poolGroup[idx].addPoolSize(start);
        }
        for (int idx = numPoolKinds-1; idx >= 0; idx--) {
            if (poolGroup[idx].other == realPool) {
                return start + offset;
            }
            start = poolGroup[idx].other->addPoolSize(start);
        }
        JS_NOT_REACHED("Entry is not in a pool");
        return -1;
    }
    void writePoolEntry(PoolEntry pe, uint8 *buff) {
        size_t size = getPoolEntrySize(pe);
        uint8 *entry = getPoolEntry(pe);
        memcpy(entry, buff, size);
    }
    void readPoolEntry(PoolEntry pe, uint8 *buff) {
        size_t size = getPoolEntrySize(pe);
        uint8 *entry = getPoolEntry(pe);
        memcpy(buff, entry, size);
    }

};
} // ion
} // js
#endif // jsion_ion_assembler_buffer_with_constant_pools_h__
