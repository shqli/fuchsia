// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_LIB_BACKOFF_BACKOFF_H_
#define SRC_LIB_BACKOFF_BACKOFF_H_

#include <lib/zx/time.h>

#include "src/lib/fxl/macros.h"

namespace backoff {

// Interface for a backoff policy.
class Backoff {
 public:
  Backoff() {}
  virtual ~Backoff() {}

  virtual zx::duration GetNext() = 0;
  virtual void Reset() = 0;

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(Backoff);
};

}  // namespace backoff

#endif  // SRC_LIB_BACKOFF_BACKOFF_H_
