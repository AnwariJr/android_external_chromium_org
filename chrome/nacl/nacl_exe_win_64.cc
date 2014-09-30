// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/lazy_instance.h"
#include "chrome/app/chrome_crash_reporter_client.h"
#include "components/crash/app/breakpad_win.h"
#include "components/nacl/loader/nacl_helper_win_64.h"
#include "content/public/common/content_switches.h"

namespace {

base::LazyInstance<chrome::ChromeCrashReporterClient>::Leaky
    g_chrome_crash_client = LAZY_INSTANCE_INITIALIZER;

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int) {
  base::AtExitManager exit_manager;
  CommandLine::Init(0, NULL);

  std::string process_type =
      CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
          switches::kProcessType);
  crash_reporter::SetCrashReporterClient(g_chrome_crash_client.Pointer());
  breakpad::InitCrashReporter(process_type);

  return nacl::NaClWin64Main();
}
