// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/gamepad_shared_memory_reader.h"

#include "base/metrics/histogram_macros.h"
#include "base/trace_event/trace_event.h"
#include "content/renderer/renderer_blink_platform_impl.h"
#include "third_party/blink/public/platform/interface_provider.h"
#include "third_party/blink/public/platform/platform.h"

namespace content {

GamepadSharedMemoryReader::GamepadSharedMemoryReader() : binding_(this) {
  blink::Platform::Current()->GetInterfaceProvider()->GetInterface(
      mojo::MakeRequest(&gamepad_monitor_));
  device::mojom::GamepadObserverPtr observer;
  binding_.Bind(mojo::MakeRequest(&observer));
  gamepad_monitor_->SetObserver(std::move(observer));
}

void GamepadSharedMemoryReader::SendStartMessage() {
  if (gamepad_monitor_) {
    gamepad_monitor_->GamepadStartPolling(&renderer_shared_buffer_handle_);
  }
}

void GamepadSharedMemoryReader::SendStopMessage() {
  if (gamepad_monitor_) {
    gamepad_monitor_->GamepadStopPolling();
  }
}

void GamepadSharedMemoryReader::Start(blink::WebGamepadListener* listener) {
  DCHECK(!listener_);
  listener_ = listener;

  SendStartMessage();

  // If we don't get a valid handle from the browser, don't try to Map (we're
  // probably out of memory or file handles).
  bool valid_handle = renderer_shared_buffer_handle_.is_valid();
  UMA_HISTOGRAM_BOOLEAN("Gamepad.ValidSharedMemoryHandle", valid_handle);
  if (!valid_handle)
    return;

  renderer_shared_buffer_mapping_ = renderer_shared_buffer_handle_->Map(
      sizeof(device::GamepadHardwareBuffer));
  CHECK(renderer_shared_buffer_mapping_);
  void* memory = renderer_shared_buffer_mapping_.get();
  CHECK(memory);
  gamepad_hardware_buffer_ =
      static_cast<device::GamepadHardwareBuffer*>(memory);
}

void GamepadSharedMemoryReader::Stop() {
  DCHECK(listener_);
  listener_ = nullptr;

  SendStopMessage();
}

void GamepadSharedMemoryReader::SampleGamepads(device::Gamepads& gamepads) {
  // Blink should have started observing at this point.
  CHECK(listener_);

  // ==========
  //   DANGER
  // ==========
  //
  // This logic is duplicated in Pepper as well. If you change it, that also
  // needs to be in sync. See ppapi/proxy/gamepad_resource.cc.
  device::Gamepads read_into;
  TRACE_EVENT0("GAMEPAD", "SampleGamepads");

  if (!renderer_shared_buffer_handle_.is_valid())
    return;

  // Only try to read this many times before failing to avoid waiting here
  // very long in case of contention with the writer. TODO(scottmg) Tune this
  // number (as low as 1?) if histogram shows distribution as mostly
  // 0-and-maximum.
  const int kMaximumContentionCount = 10;
  int contention_count = -1;
  base::subtle::Atomic32 version;
  do {
    version = gamepad_hardware_buffer_->seqlock.ReadBegin();
    memcpy(&read_into, &gamepad_hardware_buffer_->data, sizeof(read_into));
    ++contention_count;
    if (contention_count == kMaximumContentionCount)
      break;
  } while (gamepad_hardware_buffer_->seqlock.ReadRetry(version));
  UMA_HISTOGRAM_COUNTS("Gamepad.ReadContentionCount", contention_count);

  if (contention_count >= kMaximumContentionCount) {
    // We failed to successfully read, presumably because the hardware
    // thread was taking unusually long. Don't copy the data to the output
    // buffer, and simply leave what was there before.
    return;
  }

  // New data was read successfully, copy it into the output buffer.
  memcpy(&gamepads, &read_into, sizeof(gamepads));

  if (!ever_interacted_with_) {
    // Clear the connected flag if the user hasn't interacted with any of the
    // gamepads to prevent fingerprinting. The actual data is not cleared.
    // WebKit will only copy out data into the JS buffers for connected
    // gamepads so this is sufficient.
    for (unsigned i = 0; i < device::Gamepads::kItemsLengthCap; i++)
      gamepads.items[i].connected = false;
  }
}

GamepadSharedMemoryReader::~GamepadSharedMemoryReader() {
  if (listener_)
    Stop();
}

void GamepadSharedMemoryReader::GamepadConnected(
    int index,
    const device::Gamepad& gamepad) {
  // The browser already checks if the user actually interacted with a device.
  ever_interacted_with_ = true;

  if (listener_)
    listener_->DidConnectGamepad(index, gamepad);
}

void GamepadSharedMemoryReader::GamepadDisconnected(
    int index,
    const device::Gamepad& gamepad) {
  if (listener_)
    listener_->DidDisconnectGamepad(index, gamepad);
}

} // namespace content
