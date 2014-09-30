// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromecast/metrics/cast_metrics_prefs.h"

#include "components/metrics/metrics_service.h"

namespace chromecast {
namespace metrics {

void RegisterPrefs(PrefRegistrySimple* registry) {
  ::metrics::MetricsService::RegisterPrefs(registry);
}

}  // namespace metrics
}  // namespace chromecast
