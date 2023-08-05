/*************************************************************************
 * Copyright (c) 2017-2022, NVIDIA CORPORATION. All rights reserved.
 * Modifications Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All rights reserved.
 * Modifications Copyright (c) Microsoft Corporation. Licensed under the MIT License.
 *
 * See LICENSE.txt for license information
 ************************************************************************/
#ifndef MSSCLKERNELIMPL_H
#define MSSCLKERNELIMPL_H

#define HIP_ENABLE_PRINTF

#include "devcomm.h"
#include "primitives.h"
#include "collectives.h"

#include "msccl/msccl_struct.h"
#include "msccl/msccl_kernel.h"

extern __shared__ struct mscclShmemData mscclShmem;

#define MSCCL_MAX_ITER 65536

// flags are a 3-tuple of (workindex, gridoffset_iter, step) and it follows a lexicographical order. a threadblock is ahead of another iff its flag is ahead
#define COMPUTE_FLAG(__WORKINDEX__,__GRIDOFFSET_ITER__,__STEP__) \
  MSCCL_MAX_ITER*MSCCL_MAX_NUM_STEPS*(uint64_t)__WORKINDEX__ + ((uint64_t)__GRIDOFFSET_ITER__ * MSCCL_MAX_NUM_STEPS + (uint64_t)__STEP__)

// a copy of the volatile load/store from prims_ll
template<typename U>
__device__ static U load(U *src) {
  union {
    U elt;
    uint8_t u1;
    uint16_t u2;
    uint32_t u4;
    uint64_t u8;
  };
#if defined(__HIP_PLATFORM_HCC__) || defined(__HCC__) || defined(__HIPCC__)
  if(sizeof(U) == 1)
    u1 = __builtin_nontemporal_load((uint8_t*)src);
  else if(sizeof(U) == 2)
    u2 = __builtin_nontemporal_load((uint16_t*)src);
  else if(sizeof(U) == 4)
    u4 = __builtin_nontemporal_load((uint32_t*)src);
  else
    u8 = __builtin_nontemporal_load((uint64_t*)src);
#else
  if(sizeof(U) == 1)
    asm("ld.volatile.global.b8 %0,[%1];" : "=r"(u4) : "l"(src));
  else if(sizeof(U) == 2)
    asm("ld.volatile.global.b16 %0,[%1];" : "=h"(u2) : "l"(src));
  else if(sizeof(U) == 4)
    asm("ld.volatile.global.b32 %0,[%1];" : "=r"(u4) : "l"(src));
  else
    asm("ld.volatile.global.b64 %0,[%1];" : "=l"(u8) : "l"(src));
#endif
  return elt;
}

template<typename U>
__device__ static void store(U *dst, U val) {
  union {
    U elt;
    uint8_t u1;
    uint16_t u2;
    uint32_t u4;
    uint64_t u8;
  };
  elt = val;
#if defined(__HIP_PLATFORM_HCC__) || defined(__HCC__) || defined(__HIPCC__)
  if(sizeof(U) == 1)
    __builtin_nontemporal_store(u1, (uint8_t*)dst);
  else if(sizeof(U) == 2)
    __builtin_nontemporal_store(u2, (uint16_t*)dst);
  else if(sizeof(U) == 4)
    __builtin_nontemporal_store(u4, (uint32_t*)dst);
  else
    __builtin_nontemporal_store(u8, (uint64_t*)dst);
#else
  if(sizeof(U) == 1)
    asm("st.volatile.global.b8 [%0],%1;" :: "l"(dst), "r"(u4));
  else if(sizeof(U) == 2)
    asm("st.volatile.global.b16 [%0],%1;" :: "l"(dst), "h"(u2));
  else if(sizeof(U) == 4)
    asm("st.volatile.global.b32 [%0],%1;" :: "l"(dst), "r"(u4));
  else
    asm("st.volatile.global.b64 [%0],%1;" :: "l"(dst), "l"(u8));
#endif
}

inline __device__ static void barrier(int nthreads, uint64_t* barrier_next, uint64_t* barriers) {
#if defined(__HIP_PLATFORM_HCC__) || defined(__HCC__) || defined(__HIPCC__)
  if (nthreads != WARP_SIZE)
    barrier_by_group();
#else
  asm volatile ("bar.sync %1, %0;" :: "r"(nthreads), "r"(15));
#endif
}

// Copy 8-byte aligned data. You must call with at least `(bytes+7)/8` threads.
inline __device__ static void copyToShmem8(int tid, void* dst, void const* src, int bytes) {
  int offset = 8 * tid;
  if (offset < bytes) {
    uint64_t *src2 = (uint64_t*)((char const*)src + offset);
    uint64_t *dst2 = (uint64_t*)((char*)dst + offset);
    *dst2 = *src2;
  }
}

__device__ __forceinline__ static void threadBlockCopy(
  uint64_t *dst, uint64_t const *src, uint64_t size, int tid, int nthreads) {
  for (int i = tid; i < size; i += nthreads) {
    dst[i] = src[i];
  }
}

#define MSCCL_REDUCE_UNROLL_LOOP_A(numloops) \
for (int r = 0; r < numloops; r++) { \
  srcOffset = srcBaseOffset + (ssize_t)mscclShmem.mscclTB.reductionSrcOffsets[t->reductionPointer+r] * sizePerMscclChunk; \
  reduceInput = load(srcPointer + srcOffset); \
  o = applyReduce(redFn, reduceInput, o); \
}

#define MSCCL_REDUCE_UNROLL_LOOP_B(numloops) \
for (int r = 0; r < numloops; r++) { \
  srcOffset = srcBaseOffset + (ssize_t)mscclShmem.mscclTB.reductionSrcOffsets[t->reductionPointer+r] * sizePerMscclChunk; \
  srcs[r] = srcPointer + srcOffset; \
}

template<typename T>
class PrimitivesWrapperInterface {
public:
  virtual __device__ void setDataPtrs(void const *inputBuf, void *outputBuf) = 0;
  virtual __device__ void sendWithBarrier(intptr_t inpIx, int eltN) = 0;
  virtual __device__ void recv(intptr_t outIx, int eltN) = 0;
  virtual __device__ void reduce(T** srcs, int nsrcs, T** dsts, int ndsts, int eltN) = 0;
  virtual __device__ void recvCopySend(intptr_t outIx, int eltN) = 0;
  virtual __device__ void recvReduceSend(intptr_t inpIx, int eltN) = 0;
  virtual __device__ void recvReduceCopySend(intptr_t inpIx, intptr_t outIx, int eltN) = 0;
  virtual __device__ void recvReduceCopy(intptr_t inpIx, intptr_t outIx, int eltN) = 0;
  virtual __device__ void localCopy(T* srcs, T* dsts, int eltN) = 0;
};

template<typename T, typename RedOp, typename Fan, typename Proto>
class PrimitivesWrapper : public PrimitivesWrapperInterface<T> {
  Primitives<T, RedOp, Fan, 1, Proto, 0> prims;
public:
  __device__ PrimitivesWrapper(
      const int tid, const int nthreads, int const *recvPeers, int const *sendPeers,
      void const *inputBuf, void *outputBuf, uint64_t redOpArg
    ): prims(tid, nthreads, recvPeers, sendPeers, inputBuf, outputBuf, redOpArg) {}

  __device__ void setDataPtrs(void const *inputBuf, void *outputBuf) override { prims.setDataPtrs(inputBuf, outputBuf); }
  __device__ void sendWithBarrier(intptr_t inpIx, int eltN) override { prims.sendWithBarrier(inpIx, eltN); }
  __device__ void recv(intptr_t outIx, int eltN) override { prims.recv(outIx, eltN); }
  __device__ void reduce(T** srcs, int nsrcs, T** dsts, int ndsts, int eltN) override { prims.reduce(srcs, nsrcs, dsts, ndsts, eltN); }
  __device__ void recvCopySend(intptr_t outIx, int eltN) override { prims.recvCopySend(outIx, eltN); }
  __device__ void recvReduceSend(intptr_t inpIx, int eltN) override { prims.recvReduceSend(inpIx, eltN); }
  __device__ void recvReduceCopySend(intptr_t inpIx, intptr_t outIx, int eltN) override { prims.recvReduceCopySend(inpIx, outIx, eltN); }
  __device__ void recvReduceCopy(intptr_t inpIx, intptr_t outIx, int eltN) override { prims.recvReduceCopy(inpIx, outIx, eltN); }
  __device__ void localCopy(T* srcs, T* dsts, int eltN) override { prims.localCopy(srcs, dsts, eltN); }
};

template<typename T, typename RedOp, bool IsSimple>
__device__ __forceinline__ void mscclRunInterpreterHelper(
  PrimitivesWrapperInterface<T>* prims, uint64_t* mscclBarrierNext, uint64_t* mscclBarriers,
  T* thisInput, T* thisOutput, T* thisScratch, ssize_t chunkSize, int minChunkSize) {
  const int tid = threadIdx.x;
  const int bid = blockIdx.x;
  const int nthreads = NCCL_MAX_NTHREADS;

  RedOp redFn(mscclShmem.work.redOpArg);

  const ssize_t sizePerMscclChunk = mscclShmem.work.count / mscclShmem.work.nChunksPerLoop;
  uint32_t maxAllowedCount = mscclShmem.work.maxAllowedCount;

  // msccl flags all start out with 0. this is used as a part of the flag to make sure different work items deal with different synchronization flags
  // this still needs more work. when we make a way around the queue, the flag might have been set to undesired values. will be fixed in subsequent versions.
  const int64_t workIndex = mscclShmem.work.workIndex;
  volatile struct mscclFlag* mscclFlags = mscclShmem.work.syncFlags;
  for (ssize_t gridOffset = 0, iter = 0; gridOffset < sizePerMscclChunk; gridOffset += chunkSize, iter++) {
    ssize_t realChunkSize;
    if (IsSimple) {
      realChunkSize = min(chunkSize, sizePerMscclChunk-gridOffset);
      realChunkSize = roundUp(realChunkSize, nthreads*sizeof(uint64_t)/sizeof(T));
    }
    else
      realChunkSize = min(chunkSize, divUp(sizePerMscclChunk-gridOffset, minChunkSize)*minChunkSize);
    realChunkSize = int(realChunkSize);
    int nelem = min(realChunkSize, sizePerMscclChunk-gridOffset);

    ssize_t srcOffset, dstOffset;
    T *srcPointer, *dstPointer;
    int step = 0;
    for (int i = 0; i < mscclShmem.mscclTB.nSteps; i++){
      struct mscclTransmission* t = &mscclShmem.mscclTB.transmissions[i];
      // first wait if there is a dependence
      int16_t numDependencies = t->numDependencies;
      if (numDependencies > 0){
        if (tid < numDependencies) {
          int16_t dependentPointer = t->dependencePointer;
          int8_t dependentBid = mscclShmem.mscclTB.dependentBid[dependentPointer+tid];
          int16_t dependentStep = mscclShmem.mscclTB.dependentStep[dependentPointer+tid];
          uint64_t goalFlag = COMPUTE_FLAG(workIndex, iter, dependentStep);
          while ((mscclFlags + dependentBid)->flag < goalFlag);
        }
        step += numDependencies-1;
        barrier(nthreads, mscclBarrierNext, mscclBarriers);
      }

      srcPointer = (t->srcBuffer == MSCCL_INPUT_BUFFER) ? thisInput : ((t->srcBuffer == MSCCL_OUTPUT_BUFFER) ? thisOutput : thisScratch);
      dstPointer = (t->dstBuffer == MSCCL_INPUT_BUFFER) ? thisInput : ((t->dstBuffer == MSCCL_OUTPUT_BUFFER) ? thisOutput : thisScratch);
      prims->setDataPtrs(srcPointer, dstPointer);
      int count = t->count;
      for (int c = 0; c < count; c += maxAllowedCount) {
        srcOffset = gridOffset + (ssize_t) (t->srcOffset+c) * sizePerMscclChunk;
        dstOffset = gridOffset + (ssize_t) (t->dstOffset+c) * sizePerMscclChunk;
        int thisCount = min(maxAllowedCount, count - c);
        int thisNelem = nelem * thisCount;
        if (t->type == MSCCL_SEND)
          prims->sendWithBarrier(srcOffset, thisNelem); // LL.send is the only situation where there is no barrier at the end.
        else if (t->type == MSCCL_RECV)
          prims->recv(dstOffset, thisNelem);
        else if (t->type == MSCCL_REDUCE) {
          int numReductions = t->numReductions;
          if (thisNelem < nthreads){
            if (tid < thisNelem){
              dstOffset = gridOffset + (ssize_t) (t->dstOffset+c) * sizePerMscclChunk;
              T* dstIndex = dstPointer + dstOffset + tid;
              T reduceInput;
              T o = load(dstIndex);
              ssize_t srcBaseOffset = gridOffset + (ssize_t)c * sizePerMscclChunk + tid;
              switch (numReductions) {
                case 1:
                  #pragma unroll
                  MSCCL_REDUCE_UNROLL_LOOP_A(1);
                  break;
                case 3:
                  #pragma unroll
                  MSCCL_REDUCE_UNROLL_LOOP_A(3);
                  break;
                case 7:
                  #pragma unroll
                  MSCCL_REDUCE_UNROLL_LOOP_A(7);
                  break;
                case 15:
                  #pragma unroll
                  MSCCL_REDUCE_UNROLL_LOOP_A(15);
                  break;
                default:
                  MSCCL_REDUCE_UNROLL_LOOP_A(numReductions);
                  break;
              }
              store(dstIndex, o);
            }
            barrier(nthreads, mscclBarrierNext, mscclBarriers);
          } else {
            T* srcs[MSCCL_MAX_REDUCE_FUSION+1]; // +1 is for SIMPLE protocol as dst is added in the list of srcs
            dstOffset = gridOffset + (ssize_t) (t->dstOffset+c) * sizePerMscclChunk;
            T* dst = dstPointer + dstOffset;
            ssize_t srcBaseOffset = gridOffset + (ssize_t)c * sizePerMscclChunk;
            switch (numReductions) {
              case 1:
                #pragma unroll
                MSCCL_REDUCE_UNROLL_LOOP_B(1);
                break;
              case 3:
                #pragma unroll
                MSCCL_REDUCE_UNROLL_LOOP_B(3);
                break;
              case 7:
                #pragma unroll
                MSCCL_REDUCE_UNROLL_LOOP_B(7);
                break;
              case 15:
                #pragma unroll
                MSCCL_REDUCE_UNROLL_LOOP_B(15);
                break;
              default:
                MSCCL_REDUCE_UNROLL_LOOP_B(numReductions);
                break;
            }
            prims->reduce(srcs, numReductions, &dst, 1, thisNelem);
          }
          if (c == 0) step += (numReductions-1); // only advance step once!
        } else if (t->type == MSCCL_RECV_COPY_SEND)
          prims->recvCopySend(dstOffset, thisNelem);
        else if (t->type == MSCCL_RECV_REDUCE_SEND)
          prims->recvReduceSend(srcOffset, thisNelem);
        else if (t->type == MSCCL_RECV_REDUCE_COPY_SEND)
          prims->recvReduceCopySend(srcOffset, dstOffset, thisNelem);
        else if (t->type == MSCCL_RECV_REDUCE_COPY)
          prims->recvReduceCopy(srcOffset, dstOffset, thisNelem);
        else if (t->type == MSCCL_LOCAL_COPY)
          prims->localCopy(srcPointer+srcOffset, dstPointer+dstOffset, thisNelem);
        else
          return;
      }
      if (t->hasDependence && tid == nthreads-1){
        mscclFlags[bid].flag = (uint64_t) COMPUTE_FLAG(workIndex, iter, step);
      }
      step++;
    }
  }
}

template<typename T, typename RedOp, typename Proto>
__device__ __forceinline__ void mscclRunInterpreter(
  struct ncclDevComm* comm, struct mscclAlgo* algo, struct mscclWork work) {
  const int tid = threadIdx.x;
  const int bid = blockIdx.x;
  const int nthreads = NCCL_MAX_NTHREADS;

  // initialize barriers
  if (tid == 0) {
    for (auto i = 0; i < NCCL_MAX_GROUPS; i++) {
      ncclShmem.groups[i].barrier = 0;
      for (auto j = 0; j < NCCL_MAX_GROUPS; j++) ncclShmem.groups[i].barrier_next[j] = 0;
    }
  }
  uint64_t* mscclBarrierNext = ncclShmem.groups[0].barrier_next;
  uint64_t* mscclBarriers = &ncclShmem.groups[0].barrier;

  // initialize mscclShmem.mscclTB
  threadBlockCopy(
    (uint64_t *)&mscclShmem.mscclTB, (uint64_t *)(algo->mscclTBs + bid),
    sizeof(struct mscclThreadBlock) / sizeof(uint64_t), tid, nthreads);
  __synclds(); // publish mscclShmem.mscclTB.channelId

  // initialize ncclShmem and mscclShmem.work
  int channelId = mscclShmem.mscclTB.channelId;
  {
    void *dst, *src;
    int bytes = 0;
    // Use first 3 warps to load comm, channel, and work into shmem
    switch (tid/WARP_SIZE) {
    case 0:
      dst = &ncclShmem.comm;
      src = comm;
      bytes = sizeof(ncclDevComm);
      static_assert(sizeof(ncclDevComm) <= sizeof(uint64_t) * WARP_SIZE, "ncclDevComm cannot be loaded by a single warp in one insn.");
      break;
    case 1:
      // Get address of channel without incurring indirect load from ncclDevComm::channels
      dst = &ncclShmem.channel;
      src = &((ncclDevCommAndChannels*)comm)->channels[channelId];
      bytes = sizeof(ncclDevChannel);
      static_assert(sizeof(ncclDevChannel) <= sizeof(uint64_t) * WARP_SIZE, "ncclDevChannel cannot be loaded by a single warp in one insn.");
      break;
    case 2:
      dst = &mscclShmem.work;
      src = &work;
      bytes = sizeof(mscclWork);
      static_assert(sizeof(mscclWork) <= sizeof(uint64_t) * WARP_SIZE, "mscclWork cannot be loaded by a single warp in one insn.");
      break;
    case 3:
      /* set abort flag to 0 */
      if (tid == 3 * WARP_SIZE) ncclShmem.aborted = 0;
      break;
    default:
      break;
    }
    copyToShmem8(tid%WARP_SIZE, dst, src, bytes);
  }
  __synclds(); // publish shmem

  // Deference reduce args if required
  if (tid == 0 && mscclShmem.work.hasReduce && mscclShmem.work.redOpArgIsPtr) {
    switch (sizeof(T)) {
      case 1:
        mscclShmem.work.redOpArg = *reinterpret_cast<uint8_t*>(mscclShmem.work.redOpArg);
        break;
      case 2:
        mscclShmem.work.redOpArg = *reinterpret_cast<uint16_t*>(mscclShmem.work.redOpArg);
        break;
      case 4:
        mscclShmem.work.redOpArg = *reinterpret_cast<uint32_t*>(mscclShmem.work.redOpArg);
        break;
      case 8:
        mscclShmem.work.redOpArg = *reinterpret_cast<uint64_t*>(mscclShmem.work.redOpArg);
        break;
      default:
        break;
    }
  }
  __synclds(); // publish shmem

  int recvPeers[MSCCL_MAX_SEND_RECV_PEERS];
  int sendPeers[MSCCL_MAX_SEND_RECV_PEERS];
  #pragma unroll
  for (int i = 0; i < MSCCL_MAX_SEND_RECV_PEERS; ++i) {
    recvPeers[i] = mscclShmem.mscclTB.recvPeers[i];
    sendPeers[i] = mscclShmem.mscclTB.sendPeers[i];
  }

  // User pointers for primitives
  T* thisInput = (T*)mscclShmem.work.sendBuff;
  T* thisOutput = (T*)mscclShmem.work.recvBuff;
  T* thisScratch = (T*)mscclShmem.work.scratchBuffer;

  constexpr bool isSimple = (Proto::Id == NCCL_PROTO_SIMPLE);
  const ssize_t chunkSize = int(Proto::calcBytePerStep()/sizeof(T) * (isSimple ? MSCCL_CHUNKSTEPS : 1));
  int minChunkSize;
  if (Proto::Id == NCCL_PROTO_LL)
    minChunkSize = nthreads*(Proto::calcBytePerGrain()/sizeof(T));
  if (Proto::Id == NCCL_PROTO_LL128) {
    // We should not need the final /2 but it makes performance much, much smoother. Might be a bug somewhere.
    minChunkSize = nthreads*(Proto::calcBytePerGrain()/sizeof(T))/2;
  }

  const int nrecv = mscclShmem.mscclTB.nrecv;
  const int nsend = mscclShmem.mscclTB.nsend;

  if (nrecv <= 1) {
    if (nsend <= 1) {
      PrimitivesWrapper<T, RedOp, FanAsymmetric<1, 1>, Proto> prims
        (tid, nthreads, recvPeers, sendPeers, thisInput, thisOutput, mscclShmem.work.redOpArg);
      mscclRunInterpreterHelper<T, RedOp, isSimple>((PrimitivesWrapperInterface<T>*) &prims, 
        mscclBarrierNext, mscclBarriers, thisInput, thisOutput, thisScratch, chunkSize, minChunkSize);
    } else if (nsend == 2) {
      PrimitivesWrapper<T, RedOp, FanAsymmetric<1, 2>, Proto> prims
        (tid, nthreads, recvPeers, sendPeers, thisInput, thisOutput, mscclShmem.work.redOpArg);
      mscclRunInterpreterHelper<T, RedOp, isSimple>((PrimitivesWrapperInterface<T>*) &prims, 
        mscclBarrierNext, mscclBarriers, thisInput, thisOutput, thisScratch, chunkSize, minChunkSize);
    } else {
      PrimitivesWrapper<T, RedOp, FanAsymmetric<1, MSCCL_MAX_SEND_RECV_PEERS>, Proto> prims
        (tid, nthreads, recvPeers, sendPeers, thisInput, thisOutput, mscclShmem.work.redOpArg);
      mscclRunInterpreterHelper<T, RedOp, isSimple>((PrimitivesWrapperInterface<T>*) &prims, 
        mscclBarrierNext, mscclBarriers, thisInput, thisOutput, thisScratch, chunkSize, minChunkSize);
    }
  } else if (nsend <= 1) {
    if (nrecv == 2) {
      PrimitivesWrapper<T, RedOp, FanAsymmetric<2, 1>, Proto> prims
        (tid, nthreads, recvPeers, sendPeers, thisInput, thisOutput, mscclShmem.work.redOpArg);
      mscclRunInterpreterHelper<T, RedOp, isSimple>((PrimitivesWrapperInterface<T>*) &prims, 
        mscclBarrierNext, mscclBarriers, thisInput, thisOutput, thisScratch, chunkSize, minChunkSize);
    } else {
      PrimitivesWrapper<T, RedOp, FanAsymmetric<MSCCL_MAX_SEND_RECV_PEERS, 1>, Proto> prims
        (tid, nthreads, recvPeers, sendPeers, thisInput, thisOutput, mscclShmem.work.redOpArg);
      mscclRunInterpreterHelper<T, RedOp, isSimple>((PrimitivesWrapperInterface<T>*) &prims, 
        mscclBarrierNext, mscclBarriers, thisInput, thisOutput, thisScratch, chunkSize, minChunkSize);
    }
  } else {
    PrimitivesWrapper<T, RedOp, FanAsymmetric<MSCCL_MAX_SEND_RECV_PEERS, MSCCL_MAX_SEND_RECV_PEERS>, Proto> prims
      (tid, nthreads, recvPeers, sendPeers, thisInput, thisOutput, mscclShmem.work.redOpArg);
    mscclRunInterpreterHelper<T, RedOp, isSimple>((PrimitivesWrapperInterface<T>*) &prims, 
      mscclBarrierNext, mscclBarriers, thisInput, thisOutput, thisScratch, chunkSize, minChunkSize);
  }
}

#define MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, type) \
__global__ void MSCCL_KERNEL_ENTRY_NAME(devredop, type, LL)(struct ncclDevComm* comm, struct mscclAlgo* algo, struct mscclWork work) { \
  mscclRunInterpreter<type, Func##devredop<type>, ProtoLL>(comm, algo, work); \
} \
__global__ void MSCCL_KERNEL_ENTRY_NAME(devredop, type, LL128)(struct ncclDevComm* comm, struct mscclAlgo* algo, struct mscclWork work) { \
  mscclRunInterpreter<type, Func##devredop<type>, ProtoLL128>(comm, algo, work); \
} \
__global__ void MSCCL_KERNEL_ENTRY_NAME(devredop, type, Simple)(struct ncclDevComm* comm, struct mscclAlgo* algo, struct mscclWork work) { \
  mscclRunInterpreter<type, Func##devredop<type>, ProtoSimple<MSCCL_CHUNKSTEPS/MSCCL_SLICESTEPS, MSCCL_SLICESTEPS>>(comm, algo, work); \
}

#define MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP(devredop) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, int8_t) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, uint8_t) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, int32_t) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, uint32_t) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, int64_t) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, uint64_t) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, half) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, float) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, double) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP_TYPE(devredop, rccl_bfloat16)

#define MSCCL_IMPL_KERNEL_ENTRY_FUNC() \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP(Sum) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP(Prod) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP(Max) \
  MSCCL_IMPL_KERNEL_ENTRY_FUNC_DEVREDOP(Min)

#endif
