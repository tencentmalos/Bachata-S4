// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
namespace Core::Diagnostics {
// Control state only. Metrics and draw resources are owned by each Presenter.
inline std::atomic<bool> status_overlay_enabled{
#ifdef __ANDROID__
    true
#else
    false
#endif
};
} // namespace Core::Diagnostics
