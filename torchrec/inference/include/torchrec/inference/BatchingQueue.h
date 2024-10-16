/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>

#include <ATen/ATen.h>
#include <ATen/cuda/CUDAEvent.h> // @manual
#include <boost/noncopyable.hpp>
#include <c10/cuda/CUDAStream.h>
#include <folly/MPMCQueue.h>
#include <folly/Synchronized.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseThread.h>
#include <folly/synchronization/Baton.h>
#include "torchrec/inference/Batching.h"
#include "torchrec/inference/ResourceManager.h"
#include "torchrec/inference/Types.h"

namespace torchrec {

// noncopyable because we only want to move PredictionBatch around
// as it holds a reference to ResourceManagerGuard. We wouldn't want
// to inadvertently increase the reference count to ResourceManagerGuard
// with copies of this struct.
struct PredictionBatch : public boost::noncopyable {
  size_t batchSize;

  c10::Dict<std::string, at::Tensor> forwardArgs;

  std::vector<RequestContext> contexts;

  std::unique_ptr<ResourceManagerGuard> resourceManagerGuard = nullptr;

  std::chrono::time_point<std::chrono::steady_clock> enqueueTime =
      std::chrono::steady_clock::now();

  Event event;

  // Need a constructor to use make_shared/unique with
  // noncopyable struct and not trigger copy-constructor.
  explicit PredictionBatch(
      size_t bs,
      c10::Dict<std::string, at::Tensor> fa,
      std::vector<RequestContext> ctxs,
      std::unique_ptr<ResourceManagerGuard> rmg = nullptr)
      : batchSize(bs),
        forwardArgs(std::move(fa)),
        contexts(std::move(ctxs)),
        resourceManagerGuard(std::move(rmg)) {}

  void cuda();

  size_t size() const;
};

using BatchQueueCb = std::function<void(std::shared_ptr<PredictionBatch>)>;

class BatchingQueue {
 public:
  struct Config {
    std::chrono::milliseconds batchingInterval = std::chrono::milliseconds(10);
    std::chrono::milliseconds queueTimeout = std::chrono::milliseconds(500);
    int numExceptionThreads = 4;
    int numMemPinnerThreads = 4;
    int maxBatchSize = 2000;
    // For feature name to BatchingFunc name.
    const std::unordered_map<std::string, std::string> batchingMetadata;
    std::function<Event(at::DeviceIndex)> eventCreationFn;
    std::function<void()> warmupFn;
  };

  BatchingQueue(const BatchingQueue&) = delete;
  BatchingQueue& operator=(const BatchingQueue&) = delete;

  BatchingQueue(
      std::vector<BatchQueueCb> cbs,
      const Config& config,
      int worldSize,
      std::shared_ptr<ResourceManager> resourceManager = nullptr);
  ~BatchingQueue();

  void add(
      std::shared_ptr<PredictionRequest> request,
      folly::Promise<std::unique_ptr<PredictionResponse>> promise);

  void stop();

 private:
  struct QueryQueueEntry {
    std::shared_ptr<PredictionRequest> request;
    RequestContext context;
    std::chrono::time_point<std::chrono::steady_clock> addedTime;
  };

  struct BatchingQueueEntry {
    std::vector<std::shared_ptr<PredictionRequest>> requests;
    std::vector<RequestContext> contexts;
    std::chrono::time_point<std::chrono::steady_clock> addedTime;
  };

  void createBatch();

  void pinMemory(int gpuIdx);

  const Config config_;

  // Batching func name to batching func instance.
  std::unordered_map<std::string, std::unique_ptr<torchrec::BatchingFunc>>
      batchingFuncs_;
  std::vector<BatchQueueCb> cbs_;
  std::thread batchingThread_;
  std::vector<std::thread> memPinnerThreads_;
  std::unique_ptr<folly::CPUThreadPoolExecutor> rejectionExecutor_;
  folly::Synchronized<std::queue<QueryQueueEntry>> requestQueue_;
  std::vector<std::shared_ptr<folly::MPMCQueue<BatchingQueueEntry>>>
      batchingQueues_;
  std::atomic<bool> stopping_;
  int worldSize_;
  std::shared_ptr<ResourceManager> resourceManager_;
};

} // namespace torchrec
