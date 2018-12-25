/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <assert.h>
#include <algorithm>
#include <deque>

#include "../common/go_game_specific.h"
#include "elf/distri/client_manager.h"
#include "ctrl_utils.h"

#include "elf/ai/tree_search/tree_search_options.h"

using TSOptions = elf::ai::tree_search::TSOptions;
using ClientManager = elf::cs::ClientManager;

/**
 * This class keeps track of the appropriate value threshold for resigning.
 *
 * histSize: how many games to consider
 * falsePositiveTarget: how many false positives to tolerate
 * initialThreshold: the initial value threshold to use
 */
class ResignThresholdCalculator {
 public:
  ResignThresholdCalculator(
      size_t histSize,
      float falsePositiveTarget,
      float initialThreshold,
      float minThreshold,
      float maxThreshold)
      : histSize_(histSize),
        falsePositiveTarget_(falsePositiveTarget),
        curThreshold_(initialThreshold),
        minThreshold_(minThreshold),
        maxThreshold_(maxThreshold) {
    assert(histSize_ > 0);
    assert(falsePositiveTarget_ > 1e-6 && falsePositiveTarget_ < 1 - 1e-6);
    assert(
        0.0 <= minThreshold_ && minThreshold_ <= maxThreshold_ &&
        maxThreshold_ <= 2.0);
  }

  void feed(const Request &request, const Result &result) {
    numGamesFed_++;
    if (result.reward > 0.0)
      numGamesFedBlackWin_++;

    // No never resign. skip.
    if (!result.never_resign)
      return;

    nr_stats_.feed(result.reward);
    feedWinnerMinvalue(NRItem(request, result));
  }

  float getThreshold() const {
    return curThreshold_;
  }

  float updateThreshold(float maxDelta = 0.01) {
    const size_t position =
        static_cast<size_t>(falsePositiveTarget_ * winnerMinValues_.size());

    // who wants to deal with boundary conditions? not me
    if (position < 2 || position + 2 >= winnerMinValues_.size()) {
      return curThreshold_;
    }

    std::vector<NRItem> winnerMinValues(
        winnerMinValues_.begin(), winnerMinValues_.end());
    // Sort the first position element.
    std::nth_element(
        winnerMinValues.begin(),
        winnerMinValues.begin() + position,
        winnerMinValues.end());

    // Update current threshold.
    const float oldThreshold = curThreshold_;
    curThreshold_ = winnerMinValues[position].minValue;

    assert(
        curThreshold_ >
        -1e-9); // if we're getting negative values, there's a big big problem.

    curThreshold_ = std::min(curThreshold_, oldThreshold + maxDelta);
    curThreshold_ = std::max(curThreshold_, oldThreshold - maxDelta);
    curThreshold_ = std::max(curThreshold_, minThreshold_);
    curThreshold_ = std::min(curThreshold_, maxThreshold_);
    return curThreshold_;
  }

  std::string info() const {
    std::stringstream ss;
    ss << "Resign threshold: " << curThreshold_
       << ", FP Target: " << falsePositiveTarget_ << ", #game " << numGamesFed_
       << ", Black win: " << numGamesFedBlackWin_ << " ("
       << static_cast<float>(numGamesFedBlackWin_) * 100 / numGamesFed_ << "%)";
    ss << nr_stats_.info(numGamesFed_);

    return ss.str();
  }

 private:
  struct NRStats {
    size_t fp = 0;
    size_t n = 0;
    size_t blackWin = 0;

    size_t total_n = 0;
    size_t totalBlackWin = 0;

    void feed(float value) {
      total_n++;
      if (value > 0)
        totalBlackWin++;
    }

    std::string info(int n_fed) const {
      std::stringstream ss;
      ss << ", #game never resign: " << total_n << " ("
         << static_cast<float>(total_n) * 100 / n_fed << "%)"
         << ", Black win: " << totalBlackWin << " ("
         << static_cast<float>(totalBlackWin) * 100 / total_n << "%)"
         << ", #game in buffer: " << n << ", #game fp in buffer: " << fp << " ("
         << static_cast<float>(fp) * 100 / n << "%)";
      return ss.str();
    }
  };

  struct NRItem {
    float minValue = 0.0;
    bool fp = false;
    bool blackWin = false;

    NRItem(const Request &request, const Result & result) {
      // Resign calculator stuff
      blackWin = result.reward > 0;

      minValue = 2.0;
      // Only check the winner player's value.
      for (size_t i = (blackWin ? 0 : 1); i < result.values.size(); i += 2) {
        const float value =
            blackWin ? (1.0 + result.values[i]) : (1.0 - result.values[i]);
        minValue = std::min(minValue, value);
      }

      // The winning player would have resigned if never_resign was not set.
      fp = request.resign_thres > minValue;
    }

    void changeStat(NRStats& stats, int delta) const {
      stats.n += delta;
      if (fp)
        stats.fp += delta;
      if (blackWin)
        stats.blackWin += delta;
    }

    friend bool operator<(const NRItem& v1, const NRItem& v2) {
      return v1.minValue < v2.minValue;
    }
  };

  void feedWinnerMinvalue(NRItem&& item) {
    while (winnerMinValues_.size() >= histSize_) {
      winnerMinValues_.front().changeStat(nr_stats_, -1);
      winnerMinValues_.pop_front();
    }
    winnerMinValues_.push_back(std::move(item));
    winnerMinValues_.back().changeStat(nr_stats_, 1);
  }

  size_t histSize_;
  float falsePositiveTarget_;
  float curThreshold_;
  float minThreshold_;
  float maxThreshold_;
  size_t numGamesFed_ = 0;
  size_t numGamesFedBlackWin_ = 0;

  std::deque<NRItem> winnerMinValues_;
  NRStats nr_stats_;
};

struct SelfPlayRecord {
 public:
  SelfPlayRecord(int ver, const GameOptionsTrain& options)
      : ver_(ver), options_(options) {
    std::string selfplay_prefix = "selfplay-" + options_.common.net.server_id +
        "-" + options_.common.base.time_signature;
    records_.resetPrefix(selfplay_prefix + "-" + std::to_string(ver_));
  }

  void feed(const Request &, const Result &result, const Record& record) {
    const bool didBlackWin = result.reward > 0;
    if (didBlackWin) {
      black_win_++;
    } else {
      white_win_++;
    }

    if (abs(result.reward - 1.0f) < 0.1f) {
      n_white_resign_++;
    } else if (abs(result.reward + 1.0f) < 0.1f) {
      n_black_resign_++;
    }

    counter_++;
    records_.feed(record);

    if (result.num_move < 100)
      move0_100++;
    else if (result.num_move < 200)
      move100_200++;
    else if (result.num_move < 300)
      move200_300++;
    else
      move300_up++;

    if (counter_ - last_counter_shown_ >= 100) {
      std::cout << elf_utils::now() << std::endl;
      std::cout << info() << std::endl;
      last_counter_shown_ = counter_;
    }
  }

  int n() const {
    return counter_;
  }

  bool is_check_point() const {
    if (options_.selfplay_init_num > 0 && options_.selfplay_update_num > 0) {
      return (
          counter_ == options_.selfplay_init_num ||
          ((counter_ > options_.selfplay_init_num) &&
           (counter_ - options_.selfplay_init_num) %
                   options_.selfplay_update_num ==
               0));
    } else {
      // Otherwise just save one every 1000 games.
      return counter_ > 0 && counter_ % 1000 == 0;
    }
  }

  bool checkAndSave() {
    if (is_check_point()) {
      records_.saveCurrent();
      records_.clear();
      return true;
    } else {
      return false;
    }
  }

  bool needWaitForMoreSample() const {
    if (options_.selfplay_init_num <= 0)
      return false;
    if (counter_ < options_.selfplay_init_num)
      return true;

    if (options_.selfplay_update_num <= 0)
      return false;
    return counter_ < options_.selfplay_init_num +
        options_.selfplay_update_num * num_weight_update_;
  }

  void notifyWeightUpdate() {
    num_weight_update_++;
  }

  void fillInRequest(const ClientInfo&, Request* msg) const {
    msg->resign_thres = resign_threshold_;
    msg->never_resign_prob = 0.1;
    msg->async = options_.selfplay_async;
  }

  std::string info() const {
    const int n = black_win_ + white_win_;
    const int n_no_resign = n - n_black_resign_ - n_white_resign_;
    const float black_win_rate = static_cast<float>(black_win_) / (n + 1e-10);
    const float black_resign_rate =
        static_cast<float>(n_black_resign_) / (n + 1e-10);
    const float white_resign_rate =
        static_cast<float>(n_white_resign_) / (n + 1e-10);
    const float no_resign_rate = static_cast<float>(n_no_resign) / (n + 1e-10);

    std::stringstream ss;
    ss << "=== Record Stats (" << ver_ << ") ====" << std::endl;
    ss << "B/W/A: " << black_win_ << "/" << white_win_ << "/" << n << " ("
       << black_win_rate * 100 << "%). ";
    ss << "B #Resign: " << n_black_resign_ << " (" << black_resign_rate * 100
       << "%)"
       << ", W #Resign: " << n_white_resign_ << " (" << white_resign_rate * 100
       << "%)"
       << ", #NoResign: " << n_no_resign << " (" << no_resign_rate * 100 << "%)"
       << std::endl;
    ss << "Dynamic resign threshold: " << resign_threshold_ << std::endl;
    ss << "Move: [0, 100): " << move0_100 << ", [100, 200): " << move100_200
       << ", [200, 300): " << move200_300 << ", [300, up): " << move300_up
       << std::endl;
    ss << "=== End Record Stats ====" << std::endl;

    return ss.str();
  }

  void set_resign_threshold(float resign_threshold) {
    resign_threshold_ = resign_threshold;
  }

 private:
  // statistics.
  const int64_t ver_;
  const GameOptionsTrain& options_;

  RecordBuffer records_;

  int black_win_ = 0, white_win_ = 0;
  int n_black_resign_ = 0, n_white_resign_ = 0;
  int move0_100 = 0, move100_200 = 0, move200_300 = 0, move300_up = 0;
  int counter_ = 0;
  int last_counter_shown_ = 0;
  int num_weight_update_ = 0;
  float resign_threshold_ = 0.0;
};

class SelfPlaySubCtrl {
 public:
  enum CtrlResult {
    VERSION_OLD,
    VERSION_INVALID,
    INSUFFICIENT_SAMPLE,
    SUFFICIENT_SAMPLE
  };

  SelfPlaySubCtrl(const GameOptionsTrain& options)
      : options_(options),
        curr_ver_(-1),
        resignThresholdCalculator_(
            options.resign_target_hist_size,
            options.resign_target_fp_rate,
            options.resign_thres,
            options.resign_thres_lower_bound,
            options.resign_thres_upper_bound) {}

  FeedResult feed(const Request &request, const Result &result, const Record& r) {
    std::lock_guard<std::mutex> lock(mutex_);

    resignThresholdCalculator_.feed(request, result);

    if (!request.vers.is_selfplay())
      return NOT_SELFPLAY;
    if (curr_ver_ != request.vers.black_ver)
      return VERSION_MISMATCH;

    auto* perf = find_or_null(request.vers.black_ver);
    if (perf == nullptr)
      return NOT_REQUESTED;

    perf->feed(request, result, r);
    total_selfplay_++;
    if (total_selfplay_ % 1000 == 0) {
      std::cout << elf_utils::now()
                << " SelfPlaySubCtrl: #total selfplay feeded: "
                << total_selfplay_ << ", " << resignThresholdCalculator_.info()
                << std::endl;
    }
    perf->checkAndSave();
    return FEEDED;
  }

  float getResignThreshold() const {
    return resignThresholdCalculator_.getThreshold();
  }

  int64_t getCurrModel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return curr_ver_;
  }

  bool setCurrModel(int64_t ver) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ver != curr_ver_) {
      std::cout << "SelfPlay: " << curr_ver_ << " -> " << ver << std::endl;
      curr_ver_ = ver;
      find_or_create(curr_ver_);
      return true;
    }
    return false;
  }

  CtrlResult needWaitForMoreSample(int64_t selfplay_ver) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (selfplay_ver < curr_ver_)
      return VERSION_OLD;

    const auto* perf = find_or_null(curr_ver_);
    if (perf == nullptr)
      return VERSION_INVALID;
    return perf->needWaitForMoreSample() ? INSUFFICIENT_SAMPLE
                                         : SUFFICIENT_SAMPLE;
  }

  void notifyCurrentWeightUpdate() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* perf = find_or_null(curr_ver_);
    assert(perf != nullptr);
    return perf->notifyWeightUpdate();
  }

  int getNumSelfplayCurrModel() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto* perf = find_or_null(curr_ver_);
    if (perf != nullptr)
      return perf->n();
    else
      return 0;
  }

  void fillInRequest(const ClientInfo& info, Request* msg) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (curr_ver_ < 0) {
      msg->vers.set_wait();
    } else {
      auto* perf = find_or_null(curr_ver_);
      assert(perf != nullptr);
      msg->vers.black_ver = curr_ver_;
      msg->vers.white_ver = -1;
      msg->vers.mcts_opt = options_.common.mcts;
      perf->fillInRequest(info, msg);
      /*
         if (perf.n() % 10 == 0) {
         cout << elf_util::now() << ", #game: " << perf.n() << ", send msg: " <<
         msg->info() << endl;
         }
         */
    }
  }

 private:
  mutable std::mutex mutex_;

  GameOptionsTrain options_;
  int64_t curr_ver_;
  std::unordered_map<int64_t, std::unique_ptr<SelfPlayRecord>> perfs_;
  ResignThresholdCalculator resignThresholdCalculator_;

  int64_t total_selfplay_ = 0;

  SelfPlayRecord& find_or_create(int64_t ver) {
    auto it = perfs_.find(ver);
    if (it != perfs_.end()) {
      return *it->second;
    }
    auto* record = new SelfPlayRecord(ver, options_);
    perfs_[ver].reset(record);
    record->set_resign_threshold(resignThresholdCalculator_.updateThreshold());
    return *record;
  }

  SelfPlayRecord* find_or_null(int64_t ver) {
    auto it = perfs_.find(ver);
    if (it == perfs_.end()) {
      std::cout << "The version " + std::to_string(ver) +
              " was not sent before!"
                << std::endl;
      return nullptr;
    }
    return it->second.get();
  }

  const SelfPlayRecord* find_or_null(int64_t ver) const {
    auto it = perfs_.find(ver);
    if (it == perfs_.end()) {
      std::cout << "The version " + std::to_string(ver) +
              " was not sent before!"
                << std::endl;
      return nullptr;
    }
    return it->second.get();
  }
};
