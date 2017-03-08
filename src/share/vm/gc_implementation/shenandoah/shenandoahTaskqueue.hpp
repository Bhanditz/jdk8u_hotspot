/*
 * Copyright (c) 2016, Red Hat, Inc. and/or its affiliates.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_GC_SHENANDOAH_SHENANDOAH_TASKQUEUE_HPP
#define SHARE_VM_GC_SHENANDOAH_SHENANDOAH_TASKQUEUE_HPP

#include "memory/padded.hpp"
#include "utilities/taskqueue.hpp"
#include "runtime/mutex.hpp"

class Thread;

template<class E, MEMFLAGS F, unsigned int N = TASKQUEUE_SIZE>
class BufferedOverflowTaskQueue: public OverflowTaskQueue<E, F, N>
{
public:
  typedef OverflowTaskQueue<E, F, N> taskqueue_t;

  BufferedOverflowTaskQueue() : _buf_empty(true) {};

  TASKQUEUE_STATS_ONLY(using taskqueue_t::stats;)

  // Push task t onto:
  //   - first, try buffer;
  //   - then, try the queue;
  //   - then, overflow stack.
  // Return true.
  inline bool push(E t);

  // Attempt to pop from the buffer; return true if anything was popped.
  inline bool pop_buffer(E &t);

  inline void clear_buffer()  { _buf_empty = true; }
  inline bool buffer_empty()  const { return _buf_empty; }
  inline bool is_empty()        const {
    return taskqueue_t::is_empty() && buffer_empty();
  }

private:
  bool _buf_empty;
  E _elem;
};

// ObjArrayChunkedTask
//
// Encodes both regular oops, and the array oops plus chunking data for parallel array processing.
// The design goal is to make the regular oop ops very fast, because that would be the prevailing
// case. On the other hand, it should not block parallel array processing from efficiently dividing
// the array work.
//
// The idea is to steal the bits from the 64-bit oop to encode array data, if needed. For the
// proper divide-and-conquer strategies, we want to encode the "blocking" data. It turns out, the
// most efficient way to do this is to encode the array block as (chunk * 2^pow), where it is assumed
// that the block has the size of 2^pow. This requires for pow to have only 5 bits (2^32) to encode
// all possible arrays.
//
//    |---------oop---------|-pow-|--chunk---|
//    0                    49     54        64
//
// By definition, chunk == 0 means "no chunk", i.e. chunking starts from 1.
//
// This encoding gives a few interesting benefits:
//
// a) Encoding/decoding regular oops is very simple, because the upper bits are zero in that task:
//
//    |---------oop---------|00000|0000000000| // no chunk data
//
//    This helps the most ubiquitous path. The initialization amounts to putting the oop into the word
//    with zero padding. Testing for "chunkedness" is testing for zero with chunk mask.
//
// b) Splitting tasks for divide-and-conquer is possible. Suppose we have chunk <C, P> that covers
// interval [ (C-1)*2^P; C*2^P ). We can then split it into two chunks:
//      <2*C - 1, P-1>, that covers interval [ (2*C - 2)*2^(P-1); (2*C - 1)*2^(P-1) )
//      <2*C, P-1>,     that covers interval [ (2*C - 1)*2^(P-1);       2*C*2^(P-1) )
//
//    Observe that the union of these two intervals is:
//      [ (2*C - 2)*2^(P-1); 2*C*2^(P-1) )
//
//    ...which is the original interval:
//      [ (C-1)*2^P; C*2^P )
//
// c) The divide-and-conquer strategy could even start with chunk <1, round-log2-len(arr)>, and split
//    down in the parallel threads, which alleviates the upfront (serial) splitting costs.
//
// Encoding limitations caused by current bitscales mean:
//    10 bits for chunk: max 1024 blocks per array
//     5 bits for power: max 2^32 array
//    49 bits for   oop: max 512 TB of addressable space
//
// Stealing bits from oop trims down the addressable space. Stealing too few bits for chunk ID limits
// potential parallelism. Stealing too few bits for pow limits the maximum array size that can be handled.
// In future, these might be rebalanced to favor one degree of freedom against another. For example,
// if/when Arrays 2.0 bring 2^64-sized arrays, we might need to steal another bit for power. We could regain
// some bits back if chunks are counted in ObjArrayMarkingStride units.
//
// There is also a fallback version that uses plain fields, when we don't have enough space to steal the
// bits from the native pointer. It is useful to debug the _LP64 version.
//
#ifdef _LP64
class ObjArrayChunkedTask
{
public:
  enum {
    chunk_bits   = 10,
    pow_bits     = 5,
    oop_bits     = sizeof(uintptr_t)*8 - chunk_bits - pow_bits,
  };
  enum {
    chunk_size   = nth_bit(chunk_bits),
    pow_size     = nth_bit(pow_bits),
    oop_size     = nth_bit(oop_bits),
  };
  enum {
    oop_shift    = 0,
    pow_shift    = oop_shift + oop_bits,
    chunk_shift  = pow_shift + pow_bits,
  };
  enum {
    oop_mask     = right_n_bits(oop_bits),
    pow_mask     = right_n_bits(pow_bits),
    chunk_mask   = right_n_bits(chunk_bits),
    chunk_mask_unshift = ~right_n_bits(oop_bits + pow_bits),
  };

public:
  ObjArrayChunkedTask(oop o = NULL) {
    _obj = ((uintptr_t)(void*) o) << oop_shift;
  }
  ObjArrayChunkedTask(oop o, int chunk, int mult) {
    assert(0 <= chunk && chunk < chunk_size, err_msg("chunk is sane: %d", chunk));
    assert(0 <= mult && mult < pow_size, err_msg("pow is sane: %d", mult));
    uintptr_t t_b = ((uintptr_t) chunk) << chunk_shift;
    uintptr_t t_m = ((uintptr_t) mult) << pow_shift;
    uintptr_t obj = (uintptr_t)(void*)o;
    assert(obj < oop_size, err_msg("obj ref is sane: " PTR_FORMAT, obj));
    intptr_t t_o = obj << oop_shift;
    _obj = t_o | t_m | t_b;
  }
  ObjArrayChunkedTask(const ObjArrayChunkedTask& t): _obj(t._obj) { }

  ObjArrayChunkedTask& operator =(const ObjArrayChunkedTask& t) {
    _obj = t._obj;
    return *this;
  }
  volatile ObjArrayChunkedTask&
  operator =(const volatile ObjArrayChunkedTask& t) volatile {
    (void)const_cast<uintptr_t&>(_obj = t._obj);
    return *this;
  }

  inline oop obj()   const { return (oop) reinterpret_cast<void*>((_obj >> oop_shift) & oop_mask); }
  inline int chunk() const { return (int) (_obj >> chunk_shift) & chunk_mask; }
  inline int pow()   const { return (int) ((_obj >> pow_shift) & pow_mask); }
  inline bool is_not_chunked() const { return (_obj & chunk_mask_unshift) == 0; }

  DEBUG_ONLY(bool is_valid() const); // Tasks to be pushed/popped must be valid.

private:
  uintptr_t _obj;
};
#else
class ObjArrayChunkedTask
{
public:
  enum {
    chunk_bits  = 10,
    pow_bits    = 5,
  };
  enum {
    chunk_size  = nth_bit(chunk_bits),
    pow_size    = nth_bit(pow_bits),
  };
public:
  ObjArrayChunkedTask(oop o = NULL, int chunk = 0, int pow = 0): _obj(o) {
    assert(0 <= chunk && chunk < chunk_size, err_msg("chunk is sane: %d", chunk));
    assert(0 <= pow && pow < pow_size, err_msg("pow is sane: %d", pow));
    _chunk = chunk;
    _pow = pow;
  }
  ObjArrayChunkedTask(const ObjArrayChunkedTask& t): _obj(t._obj), _chunk(t._chunk), _pow(t._pow) { }

  ObjArrayChunkedTask& operator =(const ObjArrayChunkedTask& t) {
    _obj = t._obj;
    _chunk = t._chunk;
    _pow = t._pow;
    return *this;
  }
  volatile ObjArrayChunkedTask&
  operator =(const volatile ObjArrayChunkedTask& t) volatile {
    (void)const_cast<oop&>(_obj = t._obj);
    _chunk = t._chunk;
    _pow = t._pow;
    return *this;
  }

  inline oop obj()   const { return _obj; }
  inline int chunk() const { return _chunk; }
  inline int pow()  const { return _pow; }

  inline bool is_not_chunked() const { return _chunk == 0; }

  DEBUG_ONLY(bool is_valid() const); // Tasks to be pushed/popped must be valid.

private:
  oop _obj;
  int _chunk;
  int _pow;
};
#endif

typedef ObjArrayChunkedTask SCMTask;
typedef BufferedOverflowTaskQueue<SCMTask, mtGC> ShenandoahBufferedOverflowTaskQueue;
typedef Padded<ShenandoahBufferedOverflowTaskQueue> SCMObjToScanQueue;

template <class T, MEMFLAGS F>
class ParallelClaimableQueueSet: public GenericTaskQueueSet<T, F> {
private:
  volatile jint     _claimed_index;
  debug_only(uint   _reserved;  )

public:
  using GenericTaskQueueSet<T, F>::size;

public:
  ParallelClaimableQueueSet(int n) : GenericTaskQueueSet<T, F>(n) {
    debug_only(_reserved = 0; )
  }

  void clear_claimed() { _claimed_index = 0; }
  T*   claim_next();

  // reserve queues that not for parallel claiming
  void reserve(uint n) {
    assert(n <= size(), "Sanity");
    _claimed_index = (jint)n;
    debug_only(_reserved = n;)
  }

  debug_only(uint get_reserved() const { return (uint)_reserved; })
};


template <class T, MEMFLAGS F>
T* ParallelClaimableQueueSet<T, F>::claim_next() {
  jint size = (jint)GenericTaskQueueSet<T, F>::size();

  if (_claimed_index >= size) {
    return NULL;
  }

  jint index = Atomic::add(1, &_claimed_index);

  if (index <= size) {
    return GenericTaskQueueSet<T, F>::queue((uint)index - 1);
  } else {
    return NULL;
  }
}

class SCMObjToScanQueueSet: public ParallelClaimableQueueSet<SCMObjToScanQueue, mtGC> {

public:
  SCMObjToScanQueueSet(int n) : ParallelClaimableQueueSet<SCMObjToScanQueue, mtGC>(n) {
  }

  bool is_empty();

  void clear();
};


/*
 * This is an enhanced implementation of Google's work stealing
 * protocol, which is described in the paper:
 * Understanding and improving JVM GC work stealing at the data center scale
 * (http://dl.acm.org/citation.cfm?id=2926706)
 *
 * Instead of a dedicated spin-master, our implementation will let spin-master to relinquish
 * the role before it goes to sleep/wait, so allows newly arrived thread to compete for the role.
 * The intention of above enhancement, is to reduce spin-master's latency on detecting new tasks
 * for stealing and termination condition.
 */

class ShenandoahTaskTerminator: public ParallelTaskTerminator {
private:
  Monitor*    _blocker;
  Thread*     _spin_master;


public:
  ShenandoahTaskTerminator(uint n_threads, TaskQueueSetSuper* queue_set) :
    ParallelTaskTerminator(n_threads, queue_set), _spin_master(NULL) {
    _blocker = new Monitor(Mutex::leaf, "ShenandoahTaskTerminator", false);
  }

  bool offer_termination(TerminatorTerminator* terminator);

private:
  size_t tasks_in_queue_set() { return _queue_set->tasks(); }


  /*
   * Perform spin-master task.
   * return true if termination condition is detected
   * otherwise, return false
   */
  bool do_spin_master_work(TerminatorTerminator* terminator);
};

class ShenandoahCancelledTerminatorTerminator : public TerminatorTerminator {
  virtual bool should_exit_termination() {
    return false;
  }
  virtual bool should_force_termination() {
    return true;
  }
};

#endif // SHARE_VM_GC_SHENANDOAH_SHENANDOAH_TASKQUEUE_HPP
