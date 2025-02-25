// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/sessions/tab_loader.h"

#include <algorithm>

#include "base/bind.h"
#include "base/memory/memory_coordinator_client_registry.h"
#include "base/memory/memory_coordinator_proxy.h"
#include "base/memory/memory_pressure_monitor.h"
#include "base/no_destructor.h"
#include "base/sys_info.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/time/default_tick_clock.h"
#include "build/build_config.h"
#include "chrome/browser/sessions/session_restore.h"
#include "chrome/browser/sessions/session_restore_stats_collector.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "components/favicon/content/content_favicon_driver.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/content_features.h"

using content::WebContents;
using resource_coordinator::TabLoadTracker;

namespace {

const base::TickClock* GetDefaultTickClock() {
  static base::NoDestructor<base::DefaultTickClock> default_tick_clock;
  return default_tick_clock.get();
}

// Testing seams.
size_t g_max_loaded_tab_count_for_testing = 0;
base::RepeatingCallback<void(TabLoader*)>* g_construction_callback = nullptr;

}  // namespace

// Used for performing lifetime management of the tab loader. Maintains entry
// point counts and also initiates self-destruction of a finished TabLoader.
class TabLoader::ReentrancyHelper {
 public:
  explicit ReentrancyHelper(TabLoader* tab_loader) : tab_loader_(tab_loader) {
    tab_loader_->reentry_depth_++;
  }

  ~ReentrancyHelper() {
    if (--tab_loader_->reentry_depth_ != 0)
      return;

    // Getting here indicates that this is a principle entry point and that we
    // are exiting the outermost scope. In this case we should try to clean
    // things up.
    if (ShouldDestroyTabLoader())
      DestroyTabLoader();
  }

 private:
  bool ShouldDestroyTabLoader() const {
    return tab_loader_->tabs_to_load_.empty() &&
           tab_loader_->tabs_load_initiated_.empty() &&
           tab_loader_->tabs_loading_.empty();
  }

  void DestroyTabLoader() { tab_loader_->this_retainer_ = nullptr; }

  TabLoader* tab_loader_;

  DISALLOW_COPY_AND_ASSIGN(ReentrancyHelper);
};

// static
TabLoader* TabLoader::shared_tab_loader_ = nullptr;

// static
void TabLoader::RestoreTabs(const std::vector<RestoredTab>& tabs,
                            const base::TimeTicks& restore_started) {
  if (tabs.empty())
    return;

  if (!shared_tab_loader_)
    shared_tab_loader_ = new TabLoader(restore_started);

  shared_tab_loader_->stats_collector_->TrackTabs(tabs);

  // TODO(chrisha): Mix overlapping session tab restore priorities. Right now
  // the lowest priority tabs from the first session restore will load before
  // the higher priority tabs from the next session restore.
  shared_tab_loader_->StartLoading(tabs);
}

// static
void TabLoader::SetMaxLoadedTabCountForTesting(size_t value) {
  g_max_loaded_tab_count_for_testing = value;
}

// static
void TabLoader::SetConstructionCallbackForTesting(
    base::RepeatingCallback<void(TabLoader*)>* callback) {
  g_construction_callback = callback;
}

void TabLoader::SetMaxSimultaneousLoadsForTesting(size_t loading_slots) {
  DCHECK_EQ(0u, reentry_depth_);  // Should never be called reentrantly.
  max_simultaneous_loads_ = loading_slots;
}

void TabLoader::SetTickClockForTesting(base::TickClock* tick_clock) {
  clock_ = tick_clock;
}

void TabLoader::MaybeLoadSomeTabsForTesting() {
  ReentrancyHelper lifetime_helper(this);
  MaybeLoadSomeTabs();
}

TabLoader::TabLoader(base::TimeTicks restore_started)
    : memory_pressure_listener_(
          base::Bind(&TabLoader::OnMemoryPressure, base::Unretained(this))),
      clock_(GetDefaultTickClock()) {
  stats_collector_ = new SessionRestoreStatsCollector(
      restore_started,
      std::make_unique<
          SessionRestoreStatsCollector::UmaStatsReportingDelegate>());
  shared_tab_loader_ = this;
  this_retainer_ = this;
  base::MemoryCoordinatorClientRegistry::GetInstance()->Register(this);
  TabLoadTracker::Get()->AddObserver(this);

  // Invoke the post-construction testing callback if it exists. This allows
  // tests to override configuration for the TabLoader (set tick clock, loading
  // slots, etc).
  if (g_construction_callback)
    g_construction_callback->Run(this);
}

TabLoader::~TabLoader() {
  DCHECK_EQ(0u, reentry_depth_);
  DCHECK(tabs_to_load_.empty());
  DCHECK(tabs_load_initiated_.empty());
  DCHECK(tabs_loading_.empty());
  DCHECK(!force_load_timer_.IsRunning());

  shared_tab_loader_ = nullptr;
  TabLoadTracker::Get()->RemoveObserver(this);
  base::MemoryCoordinatorClientRegistry::GetInstance()->Unregister(this);
  SessionRestore::OnTabLoaderFinishedLoadingTabs();
}

void TabLoader::SetTabLoadingEnabled(bool loading_enabled) {
  ReentrancyHelper lifetime_helper(this);

  // TODO(chrisha): Make the SessionRestoreStatsCollector aware that tab loading
  // was explicitly stopped or restarted. This can be used to invalidate various
  // metrics.
  if (loading_enabled == is_loading_enabled_)
    return;
  is_loading_enabled_ = loading_enabled;
  if (is_loading_enabled_) {
    StartTimerIfNeeded();
    MaybeLoadSomeTabs();
  } else {
    // When active tab loading is reenabled all loads that were initiated before
    // or during the period when it was disabled can be ignored for timeout
    // purposes. Otherwise a bunch of tabs may simultaneously timeout and cause
    // a lot of simultaneous loads.
    tabs_loading_.clear();
    StartTimerIfNeeded();
  }
}

void TabLoader::StartLoading(const std::vector<RestoredTab>& tabs) {
  DCHECK(!tabs.empty());
  ReentrancyHelper lifetime_helper(this);

  // Create a TabLoaderDelegate which will allow OS specific behavior for tab
  // loading. This needs to be done before any calls to AddTab, as the delegate
  // is used there.
  bool delegate_existed = true;
  if (!delegate_) {
    delegate_ = TabLoaderDelegate::Create(this);
    if (max_simultaneous_loads_ == 0)
      max_simultaneous_loads_ = delegate_->GetMaxSimultaneousTabLoads();
    delegate_existed = false;
  }

  // Add the tabs to the list of tabs loading/to load. Also, restore the
  // favicons of the background tabs (the title has already been set by now).
  // This avoids having blank icons in case the restore is halted due to memory
  // pressure. Also, when multiple tabs are restored to a single window, the
  // title may not appear, and the user will have no way of finding out which
  // tabs corresponds to which page if the icon is a generic grey one.
  for (auto& restored_tab : tabs) {
    if (!restored_tab.is_active()) {
      favicon::ContentFaviconDriver* favicon_driver =
          favicon::ContentFaviconDriver::FromWebContents(
              restored_tab.contents());

      // |favicon_driver| might be null when testing.
      if (favicon_driver) {
        favicon_driver->FetchFavicon(favicon_driver->GetActiveURL(),
                                     false /* is_same_document */);
      }
    }

    AddTab(restored_tab.contents(), restored_tab.is_active());
  }

  StartTimerIfNeeded();

  // When multiple profiles are using the same TabLoader, another profile might
  // already have started loading. In that case a delegate was already created
  // and tab loading had already started. Only the initial call to StartLoading
  // needs to kick off tab loads, as otherwise the state machine is already in
  // operation.
  if (!delegate_existed)
    MaybeLoadSomeTabs();
}

void TabLoader::OnLoadingStateChange(WebContents* contents,
                                     LoadingState old_loading_state,
                                     LoadingState new_loading_state) {
  ReentrancyHelper lifetime_helper(this);

  // Calls into this can come from observers that are still running even if
  // |is_loading_enabled_| is false.
  switch (new_loading_state) {
    // It could be that a tab starts loading from outside of our control. In
    // this case we can consider it as having started to load, and the load
    // start doesn't need to be initiated by us.
    case LoadingState::LOADING: {
      // The contents may not be one that we're tracking, but MarkTabAsLoading
      // can handle this.
      MarkTabAsLoading(contents);
    } break;

    // A tab that transitions here means that loading was aborted or errored
    // out. Either way, we consider it "loaded" from our point of view.
    case LoadingState::UNLOADED:
    // A tab that completes loading successfully will transition to this state.
    case LoadingState::LOADED: {
      // Once a first tab has loaded change the timeout that is used.
      did_one_tab_load_ = true;

      // The contents may not be one that we're tracking, but RemoveTab can
      // handle this.
      RemoveTab(contents);
    } break;
  }

  StartTimerIfNeeded();
  MaybeLoadSomeTabs();
}

void TabLoader::OnStopTracking(WebContents* web_contents,
                               LoadingState loading_state) {
  ReentrancyHelper lifetime_helper(this);
  RemoveTab(web_contents);
  StartTimerIfNeeded();
  MaybeLoadSomeTabs();
}

void TabLoader::OnMemoryPressure(
    base::MemoryPressureListener::MemoryPressureLevel memory_pressure_level) {
  ReentrancyHelper lifetime_helper(this);
  switch (memory_pressure_level) {
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_NONE:
      break;
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_MODERATE:
    case base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_CRITICAL:
      StopLoadingTabs();
      break;
  }
}

void TabLoader::OnMemoryStateChange(base::MemoryState state) {
  ReentrancyHelper lifetime_helper(this);
  switch (state) {
    case base::MemoryState::NORMAL:
      break;
    case base::MemoryState::THROTTLED:
      StopLoadingTabs();
      break;
    case base::MemoryState::SUSPENDED:
    // Note that SUSPENDED never occurs in the main browser process so far.
    // Fall through.
    case base::MemoryState::UNKNOWN:
      NOTREACHED();
      break;
  }
}

bool TabLoader::ShouldStopLoadingTabs() const {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.
  if (g_max_loaded_tab_count_for_testing != 0 &&
      scheduled_to_load_count_ >= g_max_loaded_tab_count_for_testing)
    return true;
  if (base::FeatureList::IsEnabled(features::kMemoryCoordinator)) {
    return base::MemoryCoordinatorProxy::GetInstance()
               ->GetCurrentMemoryState() != base::MemoryState::NORMAL;
  }
  if (base::MemoryPressureMonitor::Get()) {
    return base::MemoryPressureMonitor::Get()->GetCurrentPressureLevel() !=
           base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_NONE;
  }
  return false;
}

size_t TabLoader::GetMaxNewTabLoads() const {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.

  // This takes into account all tabs currently loading across the browser,
  // including ones that TabLoader isn't explicitly managing. This ensures
  // that TabLoader respects user interaction first and foremost. There's a
  // small race between when we initiated loading and when TabLoadTracker
  // notifies us that it has actually started, so we also make use of
  // |tabs_load_initiated_| to track these.
  size_t loading_tab_count =
      TabLoadTracker::Get()->GetLoadingTabCount() + tabs_load_initiated_.size();

  // If a first tab hasn't been loaded and there are loads underway then no new
  // tab loads should be initiated. This provides an exclusive period of time
  // during which only visible tabs are loading, which minimizes their time to
  // load.
  if (loading_tab_count > 0 && !did_one_tab_load_)
    return 0;

  // Determine the number of free loading slots available.
  size_t tabs_to_load = 0;
  if (loading_tab_count < max_simultaneous_loads_)
    tabs_to_load = max_simultaneous_loads_ - loading_tab_count;

  // Cap the number of loads by the actual number of tabs remaining.
  tabs_to_load = std::min(tabs_to_load, tabs_to_load_.size());

  // Finally, enforce testing tab load limits.
  if (g_max_loaded_tab_count_for_testing != 0) {
    size_t tabs_remaining_for_testing =
        g_max_loaded_tab_count_for_testing - scheduled_to_load_count_;
    tabs_to_load = std::min(tabs_to_load, tabs_remaining_for_testing);
  }

  return tabs_to_load;
}

void TabLoader::AddTab(WebContents* contents, bool loading_initiated) {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.

  // Handle tabs that have already started or finished loading.
  auto loading_state = TabLoadTracker::Get()->GetLoadingState(contents);
  if (loading_state != LoadingState::UNLOADED) {
    delegate_->NotifyTabLoadStarted();
    ++scheduled_to_load_count_;
    if (loading_state == LoadingState::LOADING)
      tabs_loading_.insert(LoadingTab{clock_->NowTicks(), contents});
    return;
  }

  // Otherwise place it in one of the |tabs_load_initiated_| or
  // |tabs_to_load_| containers.
  if (loading_initiated) {
    delegate_->NotifyTabLoadStarted();
    ++scheduled_to_load_count_;
    tabs_load_initiated_.insert(contents);
  } else {
    tabs_to_load_.push_back(contents);
  }
}

void TabLoader::RemoveTab(WebContents* contents) {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.

  {
    auto it = tabs_loading_.begin();
    for (; it != tabs_loading_.end(); ++it) {
      if (it->contents == contents)
        break;
    }
    if (it != tabs_loading_.end())
      tabs_loading_.erase(it);
  }

  tabs_load_initiated_.erase(contents);

  {
    auto it = std::find(tabs_to_load_.begin(), tabs_to_load_.end(), contents);
    if (it != tabs_to_load_.end())
      tabs_to_load_.erase(it);
  }
}

void TabLoader::MarkTabAsLoadInitiated(WebContents* contents) {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.

  // This can only be called for a tab that is waiting to be loaded so this
  // should never fail.
  auto it = std::find(tabs_to_load_.begin(), tabs_to_load_.end(), contents);
  DCHECK(it != tabs_to_load_.end());
  tabs_to_load_.erase(it);

  // Tabs are considered as starting to load the moment we schedule the load.
  // The actual load notification from TabLoadTracker comes some point after
  // this.
  delegate_->NotifyTabLoadStarted();
  ++scheduled_to_load_count_;
  tabs_load_initiated_.insert(contents);
}

void TabLoader::MarkTabAsLoading(WebContents* contents) {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.

  // Calls into this can come from observers that are still running even if
  // |is_loading_enabled_| is false.

  // We get notifications for tabs that we're not explicitly tracking, so
  // gracefully handle this.
  auto it = tabs_load_initiated_.find(contents);
  if (it == tabs_load_initiated_.end())
    return;
  tabs_load_initiated_.erase(it);
  tabs_loading_.insert(LoadingTab{clock_->NowTicks(), contents});
}

void TabLoader::MarkTabAsDeferred(content::WebContents* contents) {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.

  // This can only be called for a tab that is waiting to be loaded so this
  // should never fail.
  auto it = std::find(tabs_to_load_.begin(), tabs_to_load_.end(), contents);
  DCHECK(it != tabs_to_load_.end());
  tabs_to_load_.erase(it);
  stats_collector_->DeferTab(&contents->GetController());
}

void TabLoader::MaybeLoadSomeTabs() {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.

  if (!is_loading_enabled_ || tabs_to_load_.empty())
    return;

  // Continue to load tabs while possible. This is in a loop with a
  // recalculation of GetMaxNewTabLoads() as reentrancy can cause conditions
  // to change as each tab load is initiated.
  while (GetMaxNewTabLoads() > 0)
    LoadNextTab(false /* due_to_timeout */);
}

void TabLoader::ForceLoadTimerFired() {
  ReentrancyHelper lifetime_helper(this);

  // CheckInvariants can't be called directly as the timer is no longer
  // running at this point. However, the condition under which the timer
  // should be running can be checked.
  DCHECK(is_loading_enabled_ && !tabs_to_load_.empty() &&
         !tabs_loading_.empty());
  DCHECK(!force_load_time_.is_null());

  // A timeout is in some sense equivalent to a "load" event, in that it means
  // that a tab is now being considered as loaded. This is used in the
  // selection of timeout values in RestoreTimerInvariant.
  did_one_tab_load_ = true;

  // Reset the time associated with the timer for consistency.
  force_load_time_ = base::TimeTicks();
  force_load_delay_multiplier_ *= 2;

  // Remove the expired tab from the set of loading tabs so that this tab can't
  // be detected as having timed out a second time in the next call to
  // StartTimerIfNeeded.
  tabs_loading_.erase(tabs_loading_.begin());

  // Load a new tab, ignoring the number of open loading slots. This prevents
  // loading from being blocked indefinitely by slow to load tabs. Note that
  // this can exceed the soft-cap on simultaneously loading tabs.
  LoadNextTab(true /* due_to_timeout */);
}

void TabLoader::StopLoadingTabs() {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.

  // Calls into this can come from observers that are still running even if
  // |is_loading_enabled_| is false.

  // Stop the timer and suppress any tab loads while we clean the list.
  SetTabLoadingEnabled(false);

  // Notify the stats collector of deferred tabs.
  for (auto* contents : tabs_to_load_)
    stats_collector_->DeferTab(&contents->GetController());

  // Clear out the remaining tabs to load and clean ourselves up.
  tabs_to_load_.clear();

  // Restore invariants. This will stop the timer and schedule a self-destroy.
  StartTimerIfNeeded();
}

content::WebContents* TabLoader::GetNextTabToLoad() {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.
  DCHECK(!tabs_to_load_.empty());

  // Find the next tab to load. This skips tabs that the delegate decides
  // shouldn't be loaded at this moment.
  while (!tabs_to_load_.empty()) {
    WebContents* contents = tabs_to_load_.front();
    if (delegate_->ShouldLoad(contents))
      return contents;
    MarkTabAsDeferred(contents);
  }

  // It's possible the delegate decided none of the remaining tabs should be
  // loaded, in which case the TabLoader is done and will clean itself up as
  // the stack unwinds to the outermost frame.
  return nullptr;
}

void TabLoader::LoadNextTab(bool due_to_timeout) {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.
  DCHECK(!tabs_to_load_.empty());

  // This is checked before loading every single tab to ensure that responses
  // to memory pressure are immediate.
  if (ShouldStopLoadingTabs()) {
    StopLoadingTabs();
    return;
  }

  // Find the next tab to load. This skips tabs that the delegate decides
  // shouldn't be loaded at this moment.
  WebContents* contents = GetNextTabToLoad();
  if (!contents)
    return;

  stats_collector_->OnWillLoadNextTab(due_to_timeout);
  MarkTabAsLoadInitiated(contents);
  StartTimerIfNeeded();

  // This is done last as the calls out of us can be reentrant. To make life
  // easier we ensure the timer invariant is valid before calling out.
  contents->GetController().LoadIfNecessary();
  Browser* browser = chrome::FindBrowserWithWebContents(contents);
  if (browser &&
      browser->tab_strip_model()->GetActiveWebContents() != contents) {
    // By default tabs are marked as visible. As only the active tab is
    // visible we need to explicitly tell non-active tabs they are hidden.
    // Without this call non-active tabs are not marked as backgrounded.
    //
    // NOTE: We need to do this here rather than when the tab is added to
    // the Browser as at that time not everything has been created, so that
    // the call would do nothing.
    contents->WasHidden();
  }
}

base::TimeDelta TabLoader::GetLoadTimeoutPeriod() const {
  base::TimeDelta timeout = delegate_->GetTimeoutBeforeLoadingNextTab() *
                            force_load_delay_multiplier_;
  if (!did_one_tab_load_)
    timeout = delegate_->GetFirstTabLoadingTimeout();
  return timeout;
}

void TabLoader::StartTimerIfNeeded() {
  DCHECK(reentry_depth_ > 0);  // This can only be called internally.

  if (!is_loading_enabled_ || tabs_to_load_.empty() || tabs_loading_.empty()) {
    if (force_load_timer_.IsRunning()) {
      force_load_time_ = base::TimeTicks();
      force_load_timer_.Stop();
    }
    return;
  }

  // Determine the time at which the earliest loading tab will timeout. If
  // this is the same as the time at which the currently running timer is
  // scheduled to fire then do nothing and simply let the timer fire. This
  // minimizes timer cancelations which cause orphaned tasks.
  base::TimeDelta timeout = GetLoadTimeoutPeriod();
  base::TimeTicks expiry_time =
      tabs_loading_.begin()->loading_start_time + timeout;
  if (expiry_time == force_load_time_) {
    DCHECK(force_load_timer_.IsRunning());
    return;
  }

  // Get the time remaining to the expiry, lower bounded by zero.
  base::TimeDelta expiry_delta =
      std::max(base::TimeDelta(), expiry_time - clock_->NowTicks());
  force_load_time_ = expiry_time;
  force_load_timer_.Stop();

  // If the timer has already elapsed then fire it manually right now,
  // otherwise start the timer (which posts a delayed task).
  if (expiry_delta.is_zero()) {
    ForceLoadTimerFired();
  } else {
    force_load_timer_.Start(FROM_HERE, expiry_delta, this,
                            &TabLoader::ForceLoadTimerFired);
  }
}
