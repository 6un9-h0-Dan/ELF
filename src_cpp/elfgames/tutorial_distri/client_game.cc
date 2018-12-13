/**
 * Copyright (c) 2018-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "client_game.h"

////////////////// GoGame /////////////////////
ClientGame::ClientGame(
    int game_idx,
    const GameOptions& options,
    CollectFunc func,
    ThreadedDispatcher* dispatcher)
    : game_idx_(game_idx), 
      dispatcher_(dispatcher), 
      collect_func_(func),
      options_(options) {
    }

bool ClientGame::OnReceive(const MsgRequest& request, MsgReply* reply) {
  (void)reply;
  state_ = request.state;
  // No next section.
  return false;
}

void ClientGame::OnAct(elf::game::Base* base) {
  // elf::GameClient* client = base->ctx().client;
  base_ = base;

  if (counter_ % 5 == 0) {
    using std::placeholders::_1;
    using std::placeholders::_2;
    auto f = std::bind(&ClientGame::OnReceive, this, _1, _2);
    bool block_if_no_message = false;

    do {
      dispatcher_->checkMessage(block_if_no_message, f);
    } while (false);
  }
  counter_ ++;

  elf::GameClient *client = base->client();

  // Simply use info to construct random samples.
  auto binder = client->getBinder();
  elf::FuncsWithState funcs = binder.BindStateToFunctions({"actor"}, &state_);
  Reply reply;
  elf::FuncsWithState funcs_reply = binder.BindStateToFunctions({"actor"}, &reply);
  funcs.add(funcs_reply);

  // std::cout << "[" << game_idx_ << "] Sending client data " << std::endl;
  client->sendWait({"actor"}, &funcs);

  collect_func_(state_, reply);
  
  // Now reply has content.
  state_.content += reply.a;
}
