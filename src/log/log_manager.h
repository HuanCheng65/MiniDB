// LogManager: experiment 1 empty implementation (design doc 11).
#pragma once

#include "common/types.h"

namespace minidb {

class LogManager {
 public:
  // Returns kInvalidLsn; recovery is a later experiment.
  lsn_t Append() { return kInvalidLsn; }
  void FlushUntil(lsn_t /*lsn*/) {}
};

}  // namespace minidb
