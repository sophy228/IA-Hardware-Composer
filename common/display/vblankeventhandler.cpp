/*
// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "vblankeventhandler.h"

#include <stdlib.h>
#include <time.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "displayqueue.h"
#include "hwcutils.h"
#include "hwctrace.h"

namespace hwcomposer {

static const int64_t kOneSecondNs = 1 * 1000 * 1000 * 1000;

VblankEventHandler::VblankEventHandler(DisplayQueue* display_queue)
    : HWCThread(-8, "VblankEventHandler"),
      display_(0),
      enabled_(false),
      refresh_(0.0),
      fd_(-1),
      pipe_(-1),
      last_timestamp_(-1),
      kms_fence_(0),
      display_queue_(display_queue) {
}

VblankEventHandler::~VblankEventHandler() {
}

void VblankEventHandler::Init(float refresh, int fd, int pipe) {
  spin_lock_.lock();
  refresh_ = refresh;
  fd_ = fd;
  pipe_ = pipe;
  spin_lock_.unlock();
}

bool VblankEventHandler::Initialize() {
  if (!InitWorker()) {
    ETRACE("Failed to initalize thread for KMSFenceEventHandler. %s",
           PRINTERROR());
    return false;
  }

  return true;
}

bool VblankEventHandler::EnsureReadyForNextFrame() {
  CTRACE();
  // Lets ensure the job associated with previous frame
  // has been done, else commit will fail with -EBUSY.
  spin_lock_.lock();
  uint64_t kms_fence = kms_fence_;
  kms_fence_ = 0;
  spin_lock_.unlock();

  if (kms_fence > 0) {
    HWCPoll(kms_fence, -1);
    close(kms_fence);
    display_queue_->HandleCommitUpdate(buffers_);
    std::vector<const OverlayBuffer*>().swap(buffers_);
  }

  return true;
}

void VblankEventHandler::WaitFence(uint64_t kms_fence,
                                   std::vector<OverlayLayer>& layers) {
  CTRACE();
  spin_lock_.lock();
  kms_fence_ = kms_fence;
  for (OverlayLayer& layer : layers) {
    OverlayBuffer* const buffer = layer.GetBuffer();
    buffers_.emplace_back(buffer);
    // Instead of registering again, we mark the buffer
    // released in layer so that it's not deleted till we
    // explicitly unregister the buffer.
    layer.ReleaseBuffer();
  }

  spin_lock_.unlock();
}

void VblankEventHandler::ExitThread() {
  HWCThread::Exit();
}

void VblankEventHandler::HandleExit() {
  EnsureReadyForNextFrame();
}


int VblankEventHandler::RegisterCallback(
    std::shared_ptr<VsyncCallback> callback, uint32_t display) {
  vblank_lock_.lock();
  callback_ = callback;
  display_ = display;
  last_timestamp_ = -1;
  vblank_lock_.unlock();

  return 0;
}

int VblankEventHandler::VSyncControl(bool enabled) {
  IPAGEFLIPEVENTTRACE("VblankEventHandler VSyncControl enabled %d", enabled);
  if (enabled_ == enabled)
    return 0;

  vblank_lock_.lock();
  enabled_ = enabled;
  last_timestamp_ = -1;
  vblank_lock_.unlock();

  return 0;
}

void VblankEventHandler::HandlePageFlipEvent(unsigned int sec,
                                             unsigned int usec) {
  ScopedSpinLock lock(vblank_lock_);
  if (!enabled_ || !callback_)
    return;

  int64_t timestamp = (int64_t)sec * kOneSecondNs + (int64_t)usec * 1000;
  IPAGEFLIPEVENTTRACE("HandleVblankCallBack Frame Time %f",
                      static_cast<float>(timestamp - last_timestamp_) / (1000));
  last_timestamp_ = timestamp;

  IPAGEFLIPEVENTTRACE("Callback called from HandlePageFlipEvent. %lu",
                      timestamp);
  callback_->Callback(display_, timestamp);
}

void VblankEventHandler::HandleWait() {
}

void VblankEventHandler::HandleRoutine() {
  CTRACE();
  vblank_lock_.lock();

  bool enabled = enabled_;
  int fd = fd_;
  int pipe = pipe_;

  vblank_lock_.unlock();

  if (!enabled) {
    EnsureReadyForNextFrame();

    return;
  }

  uint32_t high_crtc = (pipe << DRM_VBLANK_HIGH_CRTC_SHIFT);

  drmVBlank vblank;
  memset(&vblank, 0, sizeof(vblank));
  vblank.request.type = (drmVBlankSeqType)(
      DRM_VBLANK_RELATIVE | (high_crtc & DRM_VBLANK_HIGH_CRTC_MASK));
  vblank.request.sequence = 1;

  int ret = drmWaitVBlank(fd, &vblank);
  if (!ret)
    HandlePageFlipEvent(vblank.reply.tval_sec, (int64_t)vblank.reply.tval_usec);

  EnsureReadyForNextFrame();
}

}  // namespace hwcomposer
