// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_VERSIONED_TIMELINE_FUNCTION_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_VERSIONED_TIMELINE_FUNCTION_H_

#include <lib/media/cpp/timeline_function.h>

#include <mutex>

#include <fbl/ref_counted.h>

#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/media/audio/audio_core/utils.h"

namespace media::audio {

class VersionedTimelineFunction : public fbl::RefCounted<VersionedTimelineFunction> {
 public:
  VersionedTimelineFunction() = default;

  explicit VersionedTimelineFunction(TimelineFunction initial_function)
      : function_(initial_function) {}

  void Update(TimelineFunction fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fn != function_) {
      function_ = fn;
      generation_.Next();
    }
  }

  std::pair<TimelineFunction, uint32_t> get() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::make_pair(function_, generation_.get());
  }

  int64_t Apply(int64_t reference_input) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return function_.Apply(reference_input);
  }

 private:
  mutable std::mutex mutex_;
  TimelineFunction function_ FXL_GUARDED_BY(mutex_);
  GenerationId generation_ FXL_GUARDED_BY(mutex_);
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_VERSIONED_TIMELINE_FUNCTION_H_
