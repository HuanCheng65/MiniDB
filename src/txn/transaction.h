// Minimal transaction object (design doc 11.1).
#pragma once

#include "common/types.h"

namespace minidb {

enum class TxnState { kRunning, kCommitted, kAborted };

class Transaction {
 public:
  explicit Transaction(txn_id_t id = 0) : id_(id) {}

  txn_id_t Id() const { return id_; }
  TxnState state() const { return state_; }
  void SetState(TxnState state) { state_ = state; }

 private:
  txn_id_t id_;
  TxnState state_ = TxnState::kRunning;
};

}  // namespace minidb
