// Experiment-1 transaction manager: ids and state only (design doc 11.1).
#pragma once

#include <cstdint>

#include "txn/transaction.h"

namespace minidb {

class TxnManager {
 public:
  Transaction Begin() { return Transaction(next_txn_id_++); }
  void Commit(Transaction* txn) {
    if (txn != nullptr) txn->SetState(TxnState::kCommitted);
  }
  void Abort(Transaction* txn) {
    if (txn != nullptr) txn->SetState(TxnState::kAborted);
  }

 private:
  txn_id_t next_txn_id_ = 1;
};

}  // namespace minidb
