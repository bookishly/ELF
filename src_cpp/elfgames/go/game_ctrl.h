/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "data_loader.h"

#include "ctrl_eval.h"
#include "ctrl_selfplay.h"
#include "elf/base/context.h"
#include "elf/base/ctrl.h"
#include "elf/concurrency/ConcurrentQueue.h"
#include "elf/concurrency/Counter.h"

#include "game_stats.h"
#include "go_game_specific.h"
#include "go_state_ext.h"

using namespace std::chrono_literals;
using ThreadedCtrlBase =
    elf::ThreadedCtrlBase<elf::concurrency::ConcurrentQueue>;
using Ctrl = elf::CtrlT<elf::concurrency::ConcurrentQueue>;
using Addr = elf::Addr;

struct CtrlInfo {
  int num_games;
  GameOptions options;

  elf::GameClient* client = nullptr;
  elf::shared::ReaderQueuesT<Record>* reader = nullptr;
  elf::shared::Writer* writer = nullptr;

  std::unique_ptr<SelfPlaySubCtrl> selfplay_ctrl;
  std::unique_ptr<EvalSubCtrl> eval_ctrl;
  std::unique_ptr<ClientManager> client_mgr;
  Ctrl ctrl;

  // Server side.
  CtrlInfo(
      int num_games,
      elf::GameClient* client,
      const GameOptions& options,
      const elf::ai::tree_search::TSOptions& mcts_opt,
      elf::shared::ReaderQueuesT<Record>* reader)
      : options(options), client(client), reader(reader) {
    selfplay_ctrl.reset(new SelfPlaySubCtrl(options, mcts_opt));
    eval_ctrl.reset(new EvalSubCtrl(options, mcts_opt));

    client_mgr.reset(new ClientManager(
        num_games,
        options.client_max_delay_sec,
        options.expected_num_clients,
        0.5));
  }

  // client side.
  CtrlInfo(
      elf::GameClient* client,
      const GameOptions& options,
      elf::shared::Writer* writer,
      int num_games)
      : num_games(num_games),
        options(options),
        client(client),
        writer(writer) {}
};

class ThreadedDispatcher : public ThreadedCtrlBase {
 public:
  ThreadedDispatcher(CtrlInfo& info)
      : ThreadedCtrlBase(info.ctrl, 500), ctrl_info_(info) {
    start<std::pair<Addr, MsgRestart>, MsgRestart, MsgRequest>();
  }

  // Called by game threads
  void RegGame(int game_idx) {
    ctrl_.RegMailbox<MsgRequest, MsgRestart>(
        "game_" + std::to_string(game_idx));
    // cout << "Register game " << game_idx << endl;
    game_counter_.increment();
  }

  MsgRestart BroadcastReceiveIfDifferent(
      const MsgRequest& old_request,
      std::function<MsgRestart(MsgRequest&&)> on_receive) {
    MsgRequest request = old_request;
    MsgRestart msg;

    if (!request.vers.wait()) {
      if (!ctrl_.peekMail(&request, 0)) {
        return MsgRestart();
      }
    } else {
      ctrl_.waitMail(&request);
    }

    // Once you receive, you need to send a reply.
    msg = on_receive(std::move(request));
    ctrl_.sendMail(addr_, std::make_pair(ctrl_.getAddr(), msg));

    // Wait for confirm from the other side. If result is nontrivial.
    if (msg.result == RestartReply::UPDATE_MODEL) {
      // std::cout << "[" << msg.game_idx << "] On receive done. " << endl;
      MsgRestart msg2;
      ctrl_.waitMail(&msg2);
      // std::cout << "[" << msg.game_idx << "] Broadcast update complete. " <<
      // endl;
    }
    return msg;
  }

 protected:
  CtrlInfo& ctrl_info_;
  MsgRequest curr_request_;
  elf::concurrency::Counter<int> game_counter_;

  std::string start_target_ = "game_start";

  void before_loop() override {
    // Wait for all games + this processing thread.
    int num_games = ctrl_info_.num_games;
    // std::cout << "Wait all games[" << num_games << "] to register their mailbox"
    //           << std::endl;
    game_counter_.waitUntilCount(num_games);
    game_counter_.reset();
    // std::cout << "All games [" << num_games << "] registered" << std::endl;
  }

  void on_thread() override {
    // cout << "Register Recv threads" << endl;
    MsgRequest msg;
    if (ctrl_.peekMail(&msg, 0)) {
      process_request(msg);
    }
  }

  bool process_request(const MsgRequest& request) {
    // Actionable request
    if (request == curr_request_) {
      return false;
    }

    // std::cout << elf_utils::now()
    //           << ", EvalCtrl get new request: " << request.info() << std::endl;
    curr_request_ = request;

    MsgRequest wait_request;
    wait_request.vers.set_wait();

    std::vector<Addr> addrs = ctrl_.filterPrefix(std::string("game"));
    // std::cout << "EvalCtrl: #addrs: " << addrs.size() << std::endl;

    // Check request
    size_t n = curr_request_.client_ctrl.num_game_thread_used < 0
        ? addrs.size()
        : curr_request_.client_ctrl.num_game_thread_used;

    for (size_t i = 0; i < addrs.size(); ++i) {
      const size_t thread_idx = stoi(addrs[i].label.substr(5));
      if (thread_idx < n)
        ctrl_.sendMail(addrs[i], request);
      else
        ctrl_.sendMail(addrs[i], wait_request);
    }

    std::pair<Addr, MsgRestart> msg;
    std::vector<Addr> addrs_to_reply;
    bool update_model = false;

    // Wait until we get all confirmations.
    for (size_t i = 0; i < addrs.size(); ++i) {
      ctrl_.waitMail(&msg);
      // std::cout << "EvalCtrl: Get confirm from " << msg.second.result << ",
      // game_idx = " << msg.second.game_idx << std::endl;
      switch (msg.second.result) {
        case RestartReply::UPDATE_MODEL:
          addrs_to_reply.push_back(msg.first);
          update_model = true;
          break;
        case RestartReply::UPDATE_MODEL_ASYNC:
          update_model = true;
          break;
        default:
          break;
      }
    }

    if (update_model) {
      // Once it is done, send to Python side.
      // std::cout << elf_utils::now() << " Get actionable request: black_ver = "
      //           << request.vers.black_ver
      //           << ", white_ver = " << request.vers.white_ver
      //           << ", #addrs_to_reply: " << addrs_to_reply.size() << std::endl;
      elf::FuncsWithState funcs = ctrl_info_.client->BindStateToFunctions(
          {start_target_}, &request.vers);
      ctrl_info_.client->sendWait({start_target_}, &funcs);
    }

    for (const auto& addr : addrs_to_reply) {
      ctrl_.sendMail(addr, MsgRestart());
    }
    return true;
  }
};

class ThreadedSelfplay : public ThreadedCtrlBase {
 public:
  ThreadedSelfplay(CtrlInfo& info)
      : ThreadedCtrlBase(info.ctrl, 10000), ctrl_info_(info), rng_(time(NULL)) {
    start<int64_t>();
  }

  void waitForSufficientSelfplay(int64_t selfplay_ver) {
    SelfPlaySubCtrl::CtrlResult res;
    while (
        (res = ctrl_info_.selfplay_ctrl->needWaitForMoreSample(selfplay_ver)) ==
        SelfPlaySubCtrl::CtrlResult::INSUFFICIENT_SAMPLE) {
      std::cout << elf_utils::now() << ", Insufficient sample for model "
                << selfplay_ver << "... waiting 30s" << std::endl;
      std::this_thread::sleep_for(30s);
    }

    if (res == SelfPlaySubCtrl::CtrlResult::SUFFICIENT_SAMPLE) {
      std::cout << elf_utils::now() << ", Sufficient sample for model "
                << selfplay_ver << std::endl;
      ctrl_info_.selfplay_ctrl->notifyCurrentWeightUpdate();
    }
  }

  void waitForNewSelfplayModelReady() {
    int dummy;
    confirm_signal_.pop(&dummy);
  }

 protected:
  CtrlInfo& ctrl_info_;
  elf::concurrency::ConcurrentQueueMoodyCamel<int> confirm_signal_;
  std::mt19937 rng_;

  std::string train_ctrl_ = "train_ctrl";

  void on_thread() override {
    int64_t ver;
    if (!ctrl_.peekMail(&ver, 0))
      return;

    ctrl_info_.eval_ctrl->setBaselineModel(ver);
    int64_t old_ver = ctrl_info_.selfplay_ctrl->getCurrModel();
    ctrl_info_.selfplay_ctrl->setCurrModel(ver);

    // After setCurrModel, new model from python side with the old selfplay_ver
    // will not enter the replay buffer
    std::cout << "Updating .. old_ver: " << old_ver << ", new_ver: " << ver
              << std::endl;
    // A better model is found, clean up old games (or not?)
    if (!ctrl_info_.options.keep_prev_selfplay) {
      ctrl_info_.reader->clear();
    }

    // Data now prepared ready,
    // Send message to deblock the caller.
    confirm_signal_.push(0);
    // Then notify the python side for the new selfplay version.
    send_selfplay_version(ver);
  }

  // Call by game thread
  void send_selfplay_version(int64_t ver) {
    // Then we send information to Python side.
    MsgVersion msg;
    msg.model_ver = ver;
    elf::FuncsWithState funcs =
        ctrl_info_.client->BindStateToFunctions({train_ctrl_}, &msg);
    ctrl_info_.client->sendWait({train_ctrl_}, &funcs);
  }
};

class ThreadedWriterCtrl : public ThreadedCtrlBase {
 public:
  ThreadedWriterCtrl(CtrlInfo& info, const Addr& request_dest)
      : ThreadedCtrlBase(info.ctrl, 0),
        ctrl_info_(info),
        request_destination_(request_dest),
        records_(info.writer->identity()) {
    start<>();
  }

  void feed(const GoStateExt& s) {
    std::lock_guard<std::mutex> lock(record_mutex_);
    records_.addRecord(s.dumpRecord());
  }

  void updateState(const ThreadState& ts) {
    std::lock_guard<std::mutex> lock(record_mutex_);
    records_.updateState(ts);
  }

 protected:
  CtrlInfo& ctrl_info_;
  Addr request_destination_;

  std::mutex record_mutex_;
  Records records_;
  int64_t seq_ = 0;

  void on_thread() {
    std::string smsg;
    // Will block..
    if (!ctrl_info_.writer->getReplyNoblock(&smsg)) {
      std::cout << elf_utils::now()
                << ", WriterCtrl: no message, sleep for a while .. "
                << std::endl;
      std::this_thread::sleep_for(10s);
      return;
    }

    std::cout << elf_utils::now() << " In reply func: Message got..."
              << std::endl;
    // cout << smsg << endl;

    json j = json::parse(smsg);
    MsgRequestSeq msg = MsgRequestSeq::createFromJson(j);
    if (msg.seq != seq_) {
      std::cout << "Warning! The sequence number [" << msg.seq
                << "] in the msg is different from " << seq_ << std::endl;
    }

    ctrl_.sendMail(request_destination_, msg.request);

    if (msg.request.vers.wait()) {
      std::this_thread::sleep_for(30s);
    } else {
      if (records_.isRecordEmpty())
        std::this_thread::sleep_for(60s);
    }

    // Send data.
    std::lock_guard<std::mutex> lock(record_mutex_);

    /*
    std::cout << "Sending state update[" << records_.identity << "][" <<
    elf_utils::now() << "]"; for (const auto& s : records_.states) { std::cout
    << s.second.info() << ", ";
    }
    std::cout << std::endl;
    */
    std::cout << "Sending state update[" << elf_utils::now()
              << "], #records: " << records_.records.size()
              << ", #states: " << records_.states.size() << std::endl;

    ctrl_info_.writer->Insert(records_.dumpJsonString());
    records_.clear();
    seq_ = msg.seq + 1;
  }
};

class TrainCtrl {
 public:
  TrainCtrl(
      int num_games,
      elf::GameClient* client,
      elf::shared::ReaderQueuesT<Record>* reader,
      const GameOptions& options,
      const elf::ai::tree_search::TSOptions& mcts_opt)
      : ctrl_info_(num_games, client, options, mcts_opt, reader),
        rng_(time(NULL)) {
    using std::placeholders::_1;
    using std::placeholders::_2;

    // Several callbacks:
    // Register callback functions.
    ctrl_info_.ctrl.RegCallback<Records>(
        std::bind(&TrainCtrl::on_receive_data, this, _1, _2));

    // Register sender for python thread.
    threaded_selfplay_.reset(new ThreadedSelfplay(ctrl_info_));
  }

  void RegRecordSender() {
    // Call by shared_rw thread.
    ctrl_info_.ctrl.RegMailbox<>();
  }

  const SelfPlaySubCtrl& selfplay_ctrl() const {
    return *ctrl_info_.selfplay_ctrl;
  }
  const EvalSubCtrl& eval_ctrl() const {
    return *ctrl_info_.eval_ctrl;
  }

  bool setInitialVersion(int64_t init_version) {
    std::cout << "Setting init version: " << init_version << std::endl;
    ctrl_info_.eval_ctrl->setBaselineModel(init_version);

    if (ctrl_info_.selfplay_ctrl->getCurrModel() < 0) {
      ctrl_info_.selfplay_ctrl->setCurrModel(
          ctrl_info_.eval_ctrl->getBestModel());
    }
    return true;
  }

  bool setEvalMode(int64_t new_ver, int64_t old_ver) {
    std::cout << "Setting eval mode: new: " << new_ver << ", old: " << old_ver
              << std::endl;
    ctrl_info_.client_mgr->setSelfplayOnlyRatio(0.0);
    ctrl_info_.eval_ctrl->setBaselineModel(old_ver);
    ctrl_info_.eval_ctrl->addNewModelForEvaluation(old_ver, new_ver);
    eval_mode_ = true;
    return true;
  }

  void addNewModelForEvaluation(int64_t selfplay_ver, int64_t new_version) {
    if (ctrl_info_.options.eval_num_games == 0) {
      // And send a message to start the process.
      threaded_selfplay_->template sendToThread<int64_t>(new_version);
      threaded_selfplay_->waitForNewSelfplayModelReady();
    } else {
      ctrl_info_.eval_ctrl->addNewModelForEvaluation(selfplay_ver, new_version);
      // For offline training, we don't need to wait..
      if (ctrl_info_.options.mode != "offline_train") {
        threaded_selfplay_->waitForSufficientSelfplay(selfplay_ver);
      }
    }
  }

  void waitForSufficientSelfplay(int64_t selfplay_ver) {
    threaded_selfplay_->waitForSufficientSelfplay(selfplay_ver);
  }

  // Call by writer thread.
  // Return invalid indices.
  std::vector<FeedResult> onSelfplayGames(const std::vector<Record>& records) {
    // Receive selfplay/evaluation games.
    std::vector<FeedResult> res(records.size());

    for (size_t i = 0; i < records.size(); ++i) {
      const auto& r = records[i];
      res[i] = ctrl_info_.selfplay_ctrl->feed(r);
    }

    return res;
  }

  std::vector<FeedResult> onEvalGames(
      const ClientInfo& info,
      const std::vector<Record>& records) {
    // Receive selfplay/evaluation games.
    std::vector<FeedResult> res(records.size());

    for (size_t i = 0; i < records.size(); ++i) {
      const auto& r = records[i];
      res[i] = ctrl_info_.eval_ctrl->feed(info, r);
    }

    return res;
  }

  void onReceive(const std::string& s) {
    Records records = Records::createFromJsonString(s);
    ctrl_info_.ctrl.process(records);
  }

  void onReply(const std::string& identity, std::string* msg) {
    ClientInfo& info = ctrl_info_.client_mgr->getClient(identity);

    if (info.justAllocated()) {
      std::cout << "New allocated: " << identity << ", "
                << ctrl_info_.client_mgr->info() << std::endl;
    }

    MsgRequestSeq request;
    fill_in_request(info, &request.request);
    request.seq = info.seq();
    *msg = request.dumpJsonString();
    info.incSeq();
  }

 private:
  bool on_receive_data(const Addr&, const Records& rs) {
    ClientInfo& info = ctrl_info_.client_mgr->getClient(rs.identity);

    // Print out the stats.
    /*
    std::cout << "State update[" << rs.identity << "][" << elf_utils::now() <<
    "]"; for (const auto& s : rs.states) { std::cout << s.second.info() << ", ";
    }
    std::cout << std::endl;
    */

    for (const auto& s : rs.states) {
      info.stateUpdate(s.second);
    }

    // A client is considered dead after 20 min.
    ctrl_info_.client_mgr->updateClients();
    std::vector<FeedResult> selfplay_res = onSelfplayGames(rs.records);
    for (size_t i = 0; i < rs.records.size(); ++i) {
      if (selfplay_res[i] == FeedResult::FEEDED ||
          selfplay_res[i] == FeedResult::VERSION_MISMATCH) {
        bool black_win = rs.records[i].result.reward > 0;
        ctrl_info_.reader->InsertWithParity(
            Record(rs.records[i]), &rng_, black_win);
      }
    }

    std::vector<FeedResult> eval_res = onEvalGames(info, rs.records);
    check_new_model();

    recv_count_++;
    if (recv_count_ % 1000 == 0) {
      int valid_selfplay = 0, valid_eval = 0;
      for (size_t i = 0; i < rs.records.size(); ++i) {
        if (selfplay_res[i] == FeedResult::FEEDED)
          valid_selfplay++;
        if (eval_res[i] == FeedResult::FEEDED)
          valid_eval++;
      }

      std::cout << "TrainCtrl: Receive data[" << recv_count_ << "] from "
                << rs.identity << ", #state_update: " << rs.states.size()
                << ", #records: " << rs.records.size()
                << ", #valid_selfplay: " << valid_selfplay
                << ", #valid_eval: " << valid_eval << std::endl;
    }
    return true;
  }

  bool check_new_model() {
    int64_t new_model =
        ctrl_info_.eval_ctrl->updateState(*ctrl_info_.client_mgr);

    // If there is at least one true eval.
    if (new_model >= 0) {
      threaded_selfplay_->template sendToThread<int64_t>(new_model);
      threaded_selfplay_->waitForNewSelfplayModelReady();
      return true;
    }

    return false;
  }

  void fill_in_request(const ClientInfo& info, MsgRequest* request) {
    request->vers.set_wait();
    request->client_ctrl.client_type = info.type();

    switch (info.type()) {
      case CLIENT_SELFPLAY_ONLY:
        if (!eval_mode_) {
          ctrl_info_.selfplay_ctrl->fillInRequest(info, request);
        }
        break;
      case CLIENT_EVAL_THEN_SELFPLAY:
        ctrl_info_.eval_ctrl->fillInRequest(info, request);
        if (request->vers.wait() && !eval_mode_) {
          ctrl_info_.selfplay_ctrl->fillInRequest(info, request);
        }
        break;
      case CLIENT_INVALID:
        std::cout << "Warning! Invalid client_type! " << std::endl;
        break;
    }
  }

  CtrlInfo ctrl_info_;

  bool eval_mode_ = false;

  std::unique_ptr<ThreadedSelfplay> threaded_selfplay_;
  int recv_count_ = 0;
  std::mt19937 rng_;
};

// Ctrl for evaluation/selfplay client.
class EvalCtrl {
 public:
  EvalCtrl(
      elf::GameClient* client,
      elf::shared::Writer* writer,
      const GameOptions& options,
      int num_games)
      : ctrl_info_(client, options, writer, num_games) {
    using std::placeholders::_1;
    using std::placeholders::_2;

    ctrl_info_.ctrl.RegCallback<GoStateExt>(
        std::bind(&EvalCtrl::on_game_end, this, _1, _2));

    recv_threaded_ctrl_.reset(new ThreadedDispatcher(ctrl_info_));

    if (ctrl_info_.writer != nullptr) {
      writer_threaded_ctrl_.reset(
          new ThreadedWriterCtrl(ctrl_info_, recv_threaded_ctrl_->addr()));
    }
  }

  GameStats& getGameStats() {
    return game_stats_;
  }

  void sendRequest(const MsgRequest& request) {
    recv_threaded_ctrl_->sendToThread(request);
  }

  // Call by game threads.
  void RegGame(int game_idx) {
    recv_threaded_ctrl_->RegGame(game_idx);
  }

  MsgRestart BroadcastReceiveIfDifferent(
      const MsgRequest& old_request,
      std::function<MsgRestart(MsgRequest&&)> on_receive) {
    assert(recv_threaded_ctrl_ != nullptr);
    return recv_threaded_ctrl_->BroadcastReceiveIfDifferent(
        old_request, on_receive);
  }

  void updateState(const ThreadState& ts) {
    if (writer_threaded_ctrl_ != nullptr) {
      writer_threaded_ctrl_->updateState(ts);
    }
  }

  Ctrl* ctrl() {
    return &ctrl_info_.ctrl;
  }

 private:
  // C++ game threads -> Python / Remote
  bool on_game_end(const Addr&, const GoStateExt& s) {
    // Tell python side that one game has ended.
    if (writer_threaded_ctrl_ != nullptr) {
      writer_threaded_ctrl_->feed(s);
    }

    game_stats_.resetRankingIfNeeded(ctrl_info_.options.num_reset_ranking);
    game_stats_.feedWinRate(s.state().getFinalValue());
    // game_stats_.feedSgf(s.dumpSgf(""));

    // Report winrate (so that Python side could know).
    elf::FuncsWithState funcs =
        ctrl_info_.client->BindStateToFunctions({end_target_}, &s);
    ctrl_info_.client->sendWait({end_target_}, &funcs);

    return true;
  }

  CtrlInfo ctrl_info_;

  std::string end_target_ = "game_end";

  std::unique_ptr<ThreadedDispatcher> recv_threaded_ctrl_;
  std::unique_ptr<ThreadedWriterCtrl> writer_threaded_ctrl_;

  GameStats game_stats_;
};
