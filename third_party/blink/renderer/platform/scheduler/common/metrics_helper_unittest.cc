// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/scheduler/common/metrics_helper.h"

#include "base/test/metrics/histogram_tester.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using base::sequence_manager::TaskQueue;

namespace blink {
namespace scheduler {

namespace {

class MetricsHelperForTest : public MetricsHelper {
 public:
  MetricsHelperForTest(WebThreadType thread_type)
      : MetricsHelper(thread_type) {}
  ~MetricsHelperForTest() = default;

  using MetricsHelper::RecordCommonTaskMetrics;
};

}  // namespace

TEST(MetricsHelperTest, TaskDurationPerThreadType) {
  base::HistogramTester histogram_tester;

  MetricsHelperForTest main_thread_metrics(WebThreadType::kMainThread);
  MetricsHelperForTest compositor_metrics(WebThreadType::kCompositorThread);
  MetricsHelperForTest worker_metrics(WebThreadType::kUnspecifiedWorkerThread);

  TaskQueue::Task fake_task(
      (TaskQueue::PostedTask(base::OnceClosure(), base::Location())),
      base::TimeTicks());

  main_thread_metrics.RecordCommonTaskMetrics(
      nullptr, fake_task, base::TimeTicks() + base::TimeDelta::FromSeconds(10),
      base::TimeTicks() + base::TimeDelta::FromSeconds(50),
      base::TimeDelta::FromSeconds(15));
  compositor_metrics.RecordCommonTaskMetrics(
      nullptr, fake_task, base::TimeTicks() + base::TimeDelta::FromSeconds(10),
      base::TimeTicks() + base::TimeDelta::FromSeconds(80),
      base::TimeDelta::FromSeconds(5));
  compositor_metrics.RecordCommonTaskMetrics(
      nullptr, fake_task, base::TimeTicks() + base::TimeDelta::FromSeconds(100),
      base::TimeTicks() + base::TimeDelta::FromSeconds(200), base::nullopt);
  worker_metrics.RecordCommonTaskMetrics(
      nullptr, fake_task, base::TimeTicks() + base::TimeDelta::FromSeconds(10),
      base::TimeTicks() + base::TimeDelta::FromSeconds(125),
      base::TimeDelta::FromSeconds(25));

  EXPECT_THAT(
      histogram_tester.GetAllSamples(
          "RendererScheduler.TaskDurationPerThreadType2"),
      testing::UnorderedElementsAre(
          base::Bucket(static_cast<int>(WebThreadType::kMainThread), 40),
          base::Bucket(static_cast<int>(WebThreadType::kCompositorThread), 170),
          base::Bucket(
              static_cast<int>(WebThreadType::kUnspecifiedWorkerThread), 115)));

  EXPECT_THAT(
      histogram_tester.GetAllSamples(
          "RendererScheduler.TaskCPUDurationPerThreadType2"),
      testing::UnorderedElementsAre(
          base::Bucket(static_cast<int>(WebThreadType::kMainThread), 15),
          base::Bucket(static_cast<int>(WebThreadType::kCompositorThread), 5),
          base::Bucket(
              static_cast<int>(WebThreadType::kUnspecifiedWorkerThread), 25)));
}

}  // namespace scheduler
}  // namespace blink
