/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// TODO: Figure out how to remove this (ssengupta@fb)
#include <time.h>

#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <vector>
#include <mutex>
#include <unordered_map>

#include "elf/base/sharedmem_serializer.h"
#include "elf/distributed/addrs.h"
#include "elf/distributed/shared_rw_buffer3.h"
#include "game_context.h"
#include "game_interface.h"

#include "remote_sender.h"
#include "remote_receiver.h"

namespace elf {

using json = nlohmann::json;

class BatchSender : public elf::remote::RemoteSender {
 public:
  BatchSender(const Options& options, const msg::Options& net) 
    : elf::remote::RemoteSender(options, net, elf::remote::RAND_ONE) {
  }
  void setRemoteLabels(const std::set<std::string>& remote_labels) {
    remote_labels_ = remote_labels;
  }

  SharedMemData& allocateSharedMem(
      const SharedMemOptions& options,
      const std::vector<std::string>& keys) override {
    GameStateCollector::BatchCollectFunc func = nullptr;

    const std::string& label = options.getRecvOptions().label;

    auto it = remote_labels_.find(label);
    if (it == remote_labels_.end()) {
      BatchClient* batch_client = getBatchContext()->getClient();
      func = [batch_client](SharedMemData* smem_data) {
        return batch_client->sendWait(smem_data, {""});
      };
    } else {
      // Send to the client and wait for its response.
      int reply_idx = addQueue();

      func = [this, reply_idx](SharedMemData* smem_data) {
        json j;
        elf::SMemToJson(*smem_data, input_keys_, j);
        
        sendToClient(j.dump());
        std::string reply;
        getFromClient(reply_idx, &reply);
        // std::cout << ", got reply_j: "<< std::endl;
        elf::SMemFromJson(json::parse(reply), *smem_data);
        // std::cout << ", after parsing smem: "<< std::endl;
        return comm::SUCCESS;
      };
    }

    return getCollectorContext()->allocateSharedMem(options, keys, func);
  }

private:
  std::set<std::string> remote_labels_;
  std::set<std::string> input_keys_ {"s", "hash"};
};

class Stats {
 public:
  void feed(int idx, int batchsize) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_[idx] ++;
    sum_batchsize_ += batchsize;
    total_batchsize_ += batchsize;
    count_ ++;
    if (count_ >= 5000) {
      int min_val = std::numeric_limits<int>::max();
      int max_val = -std::numeric_limits<int>::max();
      for (const auto &p : stats_) {
        if (p.first > max_val) max_val = p.first;
        if (p.first < min_val) min_val = p.first;
      }
      // std::cout << elf_utils::now() << " Key range: [" << min_val << ", " << max_val << "]: ";
      std::vector<int> zero_entries;
      for (int i = min_val; i <= max_val; ++i) {
        // std::cout << stats_[i] << ",";
        if (stats_[i] == 0) zero_entries.push_back(i);
      }
      if (!zero_entries.empty()) {
        std::cout << elf_utils::now() << ", zero entry: ";
        for (const auto &entry : zero_entries) {
          std::cout << entry << ",";
        }
        std::cout << std::endl;
      }

      std::cout << elf_utils::now() << " Avg batchsize: " 
        << static_cast<float>(sum_batchsize_) / count_ 
        << ", #sample: " << total_batchsize_ 
        << ", #replied: " << total_release_batchsize_ 
        << ", #in_queue: " << total_batchsize_ - total_release_batchsize_ 
        << std::endl;

      reset();
    }
  }

  void recordRelease(int batchsize) {
    std::lock_guard<std::mutex> lock(mutex_);
    total_release_batchsize_ += batchsize;
  }

 private:
  std::mutex mutex_;
  std::unordered_map<int, int> stats_;
  int count_ = 0;
  int sum_batchsize_ = 0;

  int total_batchsize_ = 0;
  int total_release_batchsize_ = 0;

  void reset() {
    stats_.clear();
    count_ = 0;
    sum_batchsize_ = 0;
  }
};

using ReplyRecvFunc = std::function<void (int, std::string &&)>;

class SharedMemRemote : public SharedMem {
 public:
  enum Mode { RECV_SMEM, RECV_ENTRY };

  SharedMemRemote(
      const SharedMemOptions& opts,
      const std::unordered_map<std::string, AnyP>& mem,
      ReplyRecvFunc reply_recv, Stats *stats, Mode mode = RECV_ENTRY)
      : SharedMem(opts, mem),
        mode_(mode),
        reply_recv_(reply_recv),
        stats_(stats) {

      // We need to have a (or one per sample) remote_smem_ and a local smem_
      // Note that all remote_smem_ shared memory with smem_.
      switch (mode_) {
        case RECV_SMEM:
          remote_smem_.emplace_back(smem_);
          break;
        case RECV_ENTRY:
          for (int i = 0; i < opts.getBatchSize(); ++i) {
            remote_smem_.emplace_back(smem_.copySlice(i));
          }
          break;
      }
  }

  void start() override {}

  void push(const std::string& msg) {
    q_.push(msg);
  }

  void waitBatchFillMem() override {
    std::string msg;
    do {
      q_.pop(&msg);
      elf::SMemFromJson(json::parse(msg), remote_smem_[next_remote_smem_++]);
    } while (next_remote_smem_ < (int)remote_smem_.size());

    const auto& opt = smem_.getSharedMemOptions();
    if (stats_ != nullptr) 
      stats_->feed(opt.getLabelIdx(), smem_.getEffectiveBatchSize());
    next_remote_smem_ = 0;

    // Note that all remote_smem_ shared memory with smem_.
    // After waitBatchFillMem() return, smem_ has all the content ready.
  }

  void waitReplyReleaseBatch(comm::ReplyStatus batch_status) override {
    (void)batch_status;
    assert(reply_recv_ != nullptr);

    if (stats_ != nullptr) {
      stats_->recordRelease(smem_.getEffectiveBatchSize());
    }

    for (const auto &remote : remote_smem_) {
      json j;
      elf::SMemToJsonExclude(remote, input_keys_, j);
      // Notify that we should send the content in remote back.
      reply_recv_(remote.getSharedMemOptionsC().getLabelIdx(), j.dump());
    }
  }

 private:
  const Mode mode_;
  std::vector<SharedMemData> remote_smem_;
  int next_remote_smem_ = 0;

  remote::Queue<std::string> q_;
  std::set<std::string> input_keys_{"s", "hash"};
  ReplyRecvFunc reply_recv_ = nullptr;
  Stats *stats_ = nullptr;
};

class BatchReceiver : public elf::remote::RemoteReceiver {
 public:
  BatchReceiver(const Options& options, const msg::Options& net) 
    : elf::remote::RemoteReceiver(options, net), rng_(time(NULL)) {
    batchContext_.reset(new BatchContext);
    collectors_.reset(new Collectors);

    auto recv_func = [&](const std::string &msg) {
      int idx = rng_() % collectors_->size();
      // std::cout << timestr() << ", Forward data to " << idx << "/" <<
      //     collectors_->size() << std::endl;
      dynamic_cast<SharedMemRemote&>(collectors_->getSMem(idx))
          .push(msg);
    };

    initClients(recv_func);
  }

  void start() override {
    batchContext_->start();
    elf::remote::RemoteReceiver::start();
    collectors_->start();
  }

  void stop() override {
    batchContext_->stop(nullptr);
  }

  SharedMemData* wait(int time_usec = 0) override {
    return batchContext_->getWaiter()->wait(time_usec);
  }

  void step(comm::ReplyStatus success = comm::SUCCESS) override {
    return batchContext_->getWaiter()->step(success);
  }

  SharedMemData& allocateSharedMem(
      const SharedMemOptions& opt,
      const std::vector<std::string>& keys) override {
    const std::string& label = opt.getRecvOptions().label;

    // Allocate data.
    auto idx = collectors_->getNextIdx(label);
    int client_idx = idx.second % getNumClients();

    SharedMemOptions options_with_idx = opt;
    options_with_idx.setIdx(idx.first);
    options_with_idx.setLabelIdx(idx.second);

    // std::cout << "client_idx: " << client_idx << std::endl;

    auto reply_func = [&, client_idx](int idx, std::string &&msg) {
      addReplyMsg(client_idx, idx, std::move(msg));
    };

    auto creator = [&, reply_func](
                       const SharedMemOptions& options,
                       const std::unordered_map<std::string, AnyP>& anyps) {
      return std::unique_ptr<SharedMemRemote>(
          new SharedMemRemote(options, anyps, reply_func, &stats_, SharedMemRemote::RECV_SMEM));
    };

    BatchClient* batch_client = batchContext_->getClient();
    auto collect_func = [batch_client](SharedMemData* smem_data) {
      return batch_client->sendWait(smem_data, {""});
    };

    return collectors_->allocateSharedMem(options_with_idx, keys, creator, collect_func);
  }

  Extractor& getExtractor() override {
    return collectors_->getExtractor();
  }

private:
  std::unique_ptr<BatchContext> batchContext_;
  std::unique_ptr<Collectors> collectors_;
  std::mt19937 rng_;
  Stats stats_;
};

} // namespace elf
