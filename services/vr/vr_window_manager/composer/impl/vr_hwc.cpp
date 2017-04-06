/*
 * Copyright 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "vr_hwc.h"

#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <ui/GraphicBufferMapper.h>

#include <mutex>

#include <private/dvr/display_client.h>

#include "vr_composer_client.h"

using namespace android::hardware::graphics::common::V1_0;
using namespace android::hardware::graphics::composer::V2_1;

using android::hardware::hidl_handle;
using android::hardware::hidl_string;
using android::hardware::hidl_vec;
using android::hardware::Return;
using android::hardware::Void;

namespace android {
namespace dvr {
namespace {

using android::hardware::graphics::common::V1_0::PixelFormat;

const Display kDefaultDisplayId = 1;
const Config kDefaultConfigId = 1;

sp<GraphicBuffer> GetBufferFromHandle(const native_handle_t* handle) {
  uint32_t width = 0, height = 0, stride = 0, layer_count = 1;
  uint64_t producer_usage = 0, consumer_usage = 0;
  int32_t format = 0;

  GraphicBufferMapper& mapper = GraphicBufferMapper::get();
  // Need to register |handle| otherwise we can't read its properties.
  if (mapper.registerBuffer(handle) != OK) {
    ALOGE("Failed to register buffer");
    return nullptr;
  }

  if (mapper.getDimensions(handle, &width, &height) ||
      mapper.getStride(handle, &stride) ||
      mapper.getFormat(handle, &format) ||
      mapper.getProducerUsage(handle, &producer_usage) ||
      mapper.getConsumerUsage(handle, &consumer_usage)) {
    ALOGE("Failed to read handle properties");
    return nullptr;
  }

  // This will only succeed if gralloc has GRALLOC1_CAPABILITY_LAYERED_BUFFERS
  // capability. Otherwise assume a count of 1.
  mapper.getLayerCount(handle, &layer_count);

  // NOTE: Can't re-use |handle| since we don't own it.
  sp<GraphicBuffer> buffer = new GraphicBuffer(handle,
      GraphicBuffer::CLONE_HANDLE, width, height, format, layer_count,
      producer_usage, consumer_usage, stride);
  if (buffer->initCheck() != OK) {
    ALOGE("Failed to register cloned buffer");
    return nullptr;
  }

  return buffer;
}

void GetPrimaryDisplaySize(int32_t* width, int32_t* height) {
  *width = 1080;
  *height = 1920;

  int error = 0;
  auto display_client = DisplayClient::Create(&error);
  SystemDisplayMetrics metrics;

  if (error) {
    ALOGE("Could not connect to display service : %s(%d)", strerror(error),
          error);
    return;
  }

  error = display_client->GetDisplayMetrics(&metrics);
  if (error) {
    ALOGE("Could not get display metrics from display service : %s(%d)",
          strerror(error), error);
    return;
  }

  *width = metrics.display_native_width;
  *height = metrics.display_native_height;
}

}  // namespace

HwcDisplay::HwcDisplay(int32_t width, int32_t height)
    : width_(width), height_(height) {}

HwcDisplay::~HwcDisplay() {}

bool HwcDisplay::SetClientTarget(const native_handle_t* handle,
                                 base::unique_fd fence) {
  if (handle)
    buffer_ = GetBufferFromHandle(handle);

  fence_ = new Fence(fence.release());
  return true;
}

HwcLayer* HwcDisplay::CreateLayer() {
  uint64_t layer_id = layer_ids_++;
  layers_.push_back(HwcLayer(layer_id));
  return &layers_.back();
}

HwcLayer* HwcDisplay::GetLayer(Layer id) {
  for (size_t i = 0; i < layers_.size(); ++i)
    if (layers_[i].info.id == id)
      return &layers_[i];

  return nullptr;
}

bool HwcDisplay::DestroyLayer(Layer id) {
  for (auto it = layers_.begin(); it != layers_.end(); ++it) {
    if (it->info.id == id) {
      layers_.erase(it);
      return true;
    }
  }

  return false;
}

void HwcDisplay::GetChangedCompositionTypes(
    std::vector<Layer>* layer_ids,
    std::vector<IComposerClient::Composition>* types) {
  std::sort(layers_.begin(), layers_.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.z_order < rhs.z_order;
            });

  int first_client_layer = -1, last_client_layer = -1;
  for (size_t i = 0; i < layers_.size(); ++i) {
    switch (layers_[i].composition_type) {
      case IComposerClient::Composition::SOLID_COLOR:
      case IComposerClient::Composition::CURSOR:
      case IComposerClient::Composition::SIDEBAND:
        if (first_client_layer < 0)
          first_client_layer = i;

        last_client_layer = i;
        break;
      default:
        break;
    }
  }

  for (size_t i = 0; i < layers_.size(); ++i) {
    if (i >= first_client_layer && i <= last_client_layer) {
      if (layers_[i].composition_type != IComposerClient::Composition::CLIENT) {
        layer_ids->push_back(layers_[i].info.id);
        types->push_back(IComposerClient::Composition::CLIENT);
        layers_[i].composition_type = IComposerClient::Composition::CLIENT;
      }

      continue;
    }

    if (layers_[i].composition_type != IComposerClient::Composition::DEVICE) {
      layer_ids->push_back(layers_[i].info.id);
      types->push_back(IComposerClient::Composition::DEVICE);
      layers_[i].composition_type = IComposerClient::Composition::DEVICE;
    }
  }
}

Error HwcDisplay::GetFrame(
    std::vector<ComposerView::ComposerLayer>* out_frames) {
  bool queued_client_target = false;
  std::vector<ComposerView::ComposerLayer> frame;
  for (const auto& layer : layers_) {
    if (layer.composition_type == IComposerClient::Composition::CLIENT) {
      if (queued_client_target)
        continue;

      if (!buffer_.get()) {
        ALOGE("Client composition requested but no client target buffer");
        return Error::BAD_LAYER;
      }

      ComposerView::ComposerLayer client_target_layer = {
          .buffer = buffer_,
          .fence = fence_.get() ? fence_ : new Fence(-1),
          .display_frame = {0, 0, static_cast<int32_t>(buffer_->getWidth()),
            static_cast<int32_t>(buffer_->getHeight())},
          .crop = {0.0f, 0.0f, static_cast<float>(buffer_->getWidth()),
            static_cast<float>(buffer_->getHeight())},
          .blend_mode = IComposerClient::BlendMode::NONE,
      };

      frame.push_back(client_target_layer);
      queued_client_target = true;
    } else {
      if (!layer.info.buffer.get() || !layer.info.fence.get()) {
        ALOGV("Layer requested without valid buffer");
        continue;
      }

      frame.push_back(layer.info);
    }
  }

  out_frames->swap(frame);
  return Error::NONE;
}

std::vector<Layer> HwcDisplay::UpdateLastFrameAndGetLastFrameLayers() {
  std::vector<Layer> last_frame_layers;
  last_frame_layers.swap(last_frame_layers_ids_);

  for (const auto& layer : layers_)
    last_frame_layers_ids_.push_back(layer.info.id);

  return last_frame_layers;
}

////////////////////////////////////////////////////////////////////////////////
// VrHwcClient

VrHwc::VrHwc() {}

VrHwc::~VrHwc() {}

bool VrHwc::hasCapability(Capability capability) const { return false; }

void VrHwc::removeClient() {
  std::lock_guard<std::mutex> guard(mutex_);
  client_ = nullptr;
}

void VrHwc::enableCallback(bool enable) {
  if (enable && client_ != nullptr) {
    {
      int32_t width, height;
      GetPrimaryDisplaySize(&width, &height);
      std::lock_guard<std::mutex> guard(mutex_);
      // Create the primary display late to avoid initialization issues between
      // VR HWC and SurfaceFlinger.
      displays_[kDefaultDisplayId].reset(new HwcDisplay(width, height));
    }
    client_.promote()->onHotplug(kDefaultDisplayId,
                                 IComposerCallback::Connection::CONNECTED);
  }
}

uint32_t VrHwc::getMaxVirtualDisplayCount() { return 1; }

Error VrHwc::createVirtualDisplay(uint32_t width, uint32_t height,
                                  PixelFormat* format, Display* outDisplay) {
  *format = PixelFormat::RGBA_8888;
  *outDisplay = display_count_;
  displays_[display_count_].reset(new HwcDisplay(width, height));
  display_count_++;
  return Error::NONE;
}

Error VrHwc::destroyVirtualDisplay(Display display) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (display == kDefaultDisplayId || displays_.erase(display) == 0)
    return Error::BAD_DISPLAY;
  ComposerView::Frame frame;
  frame.display_id = display;
  frame.removed = true;
  if (observer_)
    observer_->OnNewFrame(frame);
  return Error::NONE;
}

Error VrHwc::createLayer(Display display, Layer* outLayer) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  HwcLayer* layer = display_ptr->CreateLayer();
  *outLayer = layer->info.id;
  return Error::NONE;
}

Error VrHwc::destroyLayer(Display display, Layer layer) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr) {
    return Error::BAD_DISPLAY;
  }

  return display_ptr->DestroyLayer(layer) ? Error::NONE : Error::BAD_LAYER;
}

Error VrHwc::getActiveConfig(Display display, Config* outConfig) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  *outConfig = kDefaultConfigId;
  return Error::NONE;
}

Error VrHwc::getClientTargetSupport(Display display, uint32_t width,
                                    uint32_t height, PixelFormat format,
                                    Dataspace dataspace) {
  return Error::NONE;
}

Error VrHwc::getColorModes(Display display, hidl_vec<ColorMode>* outModes) {
  std::vector<ColorMode> color_modes(1, ColorMode::NATIVE);
  *outModes = hidl_vec<ColorMode>(color_modes);
  return Error::NONE;
}

Error VrHwc::getDisplayAttribute(Display display, Config config,
                                 IComposerClient::Attribute attribute,
                                 int32_t* outValue) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr) {
    return Error::BAD_DISPLAY;
  }
  if (config != kDefaultConfigId) {
    return Error::BAD_CONFIG;
  }

  switch (attribute) {
    case IComposerClient::Attribute::WIDTH:
      *outValue = display_ptr->width();
      break;
    case IComposerClient::Attribute::HEIGHT:
      *outValue = display_ptr->height();
      break;
    case IComposerClient::Attribute::VSYNC_PERIOD:
      *outValue = 1000 * 1000 * 1000 / 30;  // 30fps
      break;
    case IComposerClient::Attribute::DPI_X:
    case IComposerClient::Attribute::DPI_Y:
      *outValue = 300 * 1000;  // 300dpi
      break;
    default:
      return Error::BAD_PARAMETER;
  }

  return Error::NONE;
}

Error VrHwc::getDisplayConfigs(Display display, hidl_vec<Config>* outConfigs) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  std::vector<Config> configs(1, kDefaultConfigId);
  *outConfigs = hidl_vec<Config>(configs);
  return Error::NONE;
}

Error VrHwc::getDisplayName(Display display, hidl_string* outName) {
  *outName = hidl_string();
  return Error::NONE;
}

Error VrHwc::getDisplayType(Display display,
                            IComposerClient::DisplayType* outType) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr) {
    *outType = IComposerClient::DisplayType::INVALID;
    return Error::BAD_DISPLAY;
  }

  if (display == kDefaultDisplayId)
    *outType = IComposerClient::DisplayType::PHYSICAL;
  else
    *outType = IComposerClient::DisplayType::VIRTUAL;

  return Error::NONE;
}

Error VrHwc::getDozeSupport(Display display, bool* outSupport) {
  *outSupport = false;
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::getHdrCapabilities(Display display, hidl_vec<Hdr>* outTypes,
                                float* outMaxLuminance,
                                float* outMaxAverageLuminance,
                                float* outMinLuminance) {
  *outMaxLuminance = 0;
  *outMaxAverageLuminance = 0;
  *outMinLuminance = 0;
  return Error::NONE;
}

Error VrHwc::setActiveConfig(Display display, Config config) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  if (config != kDefaultConfigId)
    return Error::BAD_CONFIG;

  return Error::NONE;
}

Error VrHwc::setColorMode(Display display, ColorMode mode) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setPowerMode(Display display, IComposerClient::PowerMode mode) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setVsyncEnabled(Display display, IComposerClient::Vsync enabled) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setColorTransform(Display display, const float* matrix,
                               int32_t hint) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setClientTarget(Display display, buffer_handle_t target,
                             int32_t acquireFence, int32_t dataspace,
                             const std::vector<hwc_rect_t>& damage) {
  base::unique_fd fence(acquireFence);
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  if (target == nullptr)
    return Error::NONE;

  if (!display_ptr->SetClientTarget(target, std::move(fence)))
    return Error::BAD_PARAMETER;

  return Error::NONE;
}

Error VrHwc::setOutputBuffer(Display display, buffer_handle_t buffer,
                             int32_t releaseFence) {
  base::unique_fd fence(releaseFence);
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  // TODO(dnicoara): Is it necessary to do anything here?
  return Error::NONE;
}

Error VrHwc::validateDisplay(
    Display display, std::vector<Layer>* outChangedLayers,
    std::vector<IComposerClient::Composition>* outCompositionTypes,
    uint32_t* outDisplayRequestMask, std::vector<Layer>* outRequestedLayers,
    std::vector<uint32_t>* outRequestMasks) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  display_ptr->GetChangedCompositionTypes(outChangedLayers,
                                          outCompositionTypes);
  return Error::NONE;
}

Error VrHwc::acceptDisplayChanges(Display display) { return Error::NONE; }

Error VrHwc::presentDisplay(Display display, int32_t* outPresentFence,
                            std::vector<Layer>* outLayers,
                            std::vector<int32_t>* outReleaseFences) {
  *outPresentFence = -1;
  outLayers->clear();
  outReleaseFences->clear();

  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);

  if (!display_ptr)
    return Error::BAD_DISPLAY;

  ComposerView::Frame frame;
  std::vector<Layer> last_frame_layers;
  Error status = display_ptr->GetFrame(&frame.layers);
  frame.display_id = display;
  frame.display_width = display_ptr->width();
  frame.display_height = display_ptr->height();
  if (status != Error::NONE)
    return status;

  last_frame_layers = display_ptr->UpdateLastFrameAndGetLastFrameLayers();

  base::unique_fd fence;
  if (observer_)
    fence = observer_->OnNewFrame(frame);

  if (fence.get() < 0)
    return Error::NONE;

  *outPresentFence = dup(fence.get());
  outLayers->swap(last_frame_layers);
  for (size_t i = 0; i < outLayers->size(); ++i)
    outReleaseFences->push_back(dup(fence.get()));

  return Error::NONE;
}

Error VrHwc::setLayerCursorPosition(Display display, Layer layer, int32_t x,
                                    int32_t y) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setLayerBuffer(Display display, Layer layer,
                            buffer_handle_t buffer, int32_t acquireFence) {
  base::unique_fd fence(acquireFence);
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  HwcLayer* hwc_layer = display_ptr->GetLayer(layer);
  if (!hwc_layer)
    return Error::BAD_LAYER;

  hwc_layer->info.buffer = GetBufferFromHandle(buffer);
  hwc_layer->info.fence = new Fence(fence.release());

  return Error::NONE;
}

Error VrHwc::setLayerSurfaceDamage(Display display, Layer layer,
                                   const std::vector<hwc_rect_t>& damage) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setLayerBlendMode(Display display, Layer layer, int32_t mode) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  HwcLayer* hwc_layer = display_ptr->GetLayer(layer);
  if (!hwc_layer)
    return Error::BAD_LAYER;

  hwc_layer->info.blend_mode =
      static_cast<ComposerView::ComposerLayer::BlendMode>(mode);

  return Error::NONE;
}

Error VrHwc::setLayerColor(Display display, Layer layer,
                           IComposerClient::Color color) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setLayerCompositionType(Display display, Layer layer,
                                     int32_t type) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  HwcLayer* hwc_layer = display_ptr->GetLayer(layer);
  if (!hwc_layer)
    return Error::BAD_LAYER;

  hwc_layer->composition_type = static_cast<HwcLayer::Composition>(type);

  return Error::NONE;
}

Error VrHwc::setLayerDataspace(Display display, Layer layer,
                               int32_t dataspace) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setLayerDisplayFrame(Display display, Layer layer,
                                  const hwc_rect_t& frame) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  HwcLayer* hwc_layer = display_ptr->GetLayer(layer);
  if (!hwc_layer)
    return Error::BAD_LAYER;

  hwc_layer->info.display_frame =
      {frame.left, frame.top, frame.right, frame.bottom};

  return Error::NONE;
}

Error VrHwc::setLayerPlaneAlpha(Display display, Layer layer, float alpha) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  HwcLayer* hwc_layer = display_ptr->GetLayer(layer);
  if (!hwc_layer)
    return Error::BAD_LAYER;

  hwc_layer->info.alpha = alpha;

  return Error::NONE;
}

Error VrHwc::setLayerSidebandStream(Display display, Layer layer,
                                    buffer_handle_t stream) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setLayerSourceCrop(Display display, Layer layer,
                                const hwc_frect_t& crop) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  HwcLayer* hwc_layer = display_ptr->GetLayer(layer);
  if (!hwc_layer)
    return Error::BAD_LAYER;

  hwc_layer->info.crop = {crop.left, crop.top, crop.right, crop.bottom};

  return Error::NONE;
}

Error VrHwc::setLayerTransform(Display display, Layer layer,
                               int32_t transform) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setLayerVisibleRegion(Display display, Layer layer,
                                   const std::vector<hwc_rect_t>& visible) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (!FindDisplay(display))
    return Error::BAD_DISPLAY;
  return Error::NONE;
}

Error VrHwc::setLayerZOrder(Display display, Layer layer, uint32_t z) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  HwcLayer* hwc_layer = display_ptr->GetLayer(layer);
  if (!hwc_layer)
    return Error::BAD_LAYER;

  hwc_layer->z_order = z;

  return Error::NONE;
}

Error VrHwc::setLayerInfo(Display display, Layer layer, uint32_t type,
                          uint32_t appId) {
  std::lock_guard<std::mutex> guard(mutex_);
  auto display_ptr = FindDisplay(display);
  if (!display_ptr)
    return Error::BAD_DISPLAY;

  HwcLayer* hwc_layer = display_ptr->GetLayer(layer);
  if (!hwc_layer)
    return Error::BAD_LAYER;

  hwc_layer->info.type = type;
  hwc_layer->info.app_id = appId;

  return Error::NONE;
}

Return<void> VrHwc::getCapabilities(getCapabilities_cb hidl_cb) {
  hidl_cb(hidl_vec<Capability>());
  return Void();
}

Return<void> VrHwc::dumpDebugInfo(dumpDebugInfo_cb hidl_cb) {
  hidl_cb(hidl_string());
  return Void();
}

Return<void> VrHwc::createClient(createClient_cb hidl_cb) {
  std::lock_guard<std::mutex> guard(mutex_);

  Error status = Error::NONE;
  sp<VrComposerClient> client;
  if (client_ == nullptr) {
    client = new VrComposerClient(*this);
    client->initialize();
  } else {
    ALOGE("Already have a client");
    status = Error::NO_RESOURCES;
  }

  client_ = client;
  hidl_cb(status, client);
  return Void();
}

void VrHwc::RegisterObserver(Observer* observer) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (observer_)
    ALOGE("Overwriting observer");
  else
    observer_ = observer;
}

void VrHwc::UnregisterObserver(Observer* observer) {
  std::lock_guard<std::mutex> guard(mutex_);
  if (observer != observer_)
    ALOGE("Trying to unregister unknown observer");
  else
    observer_ = nullptr;
}

HwcDisplay* VrHwc::FindDisplay(Display display) {
  auto iter = displays_.find(display);
  return iter == displays_.end() ? nullptr : iter->second.get();
}

ComposerView* GetComposerViewFromIComposer(
    hardware::graphics::composer::V2_1::IComposer* composer) {
  return static_cast<VrHwc*>(composer);
}

IComposer* HIDL_FETCH_IComposer(const char*) { return new VrHwc(); }

}  // namespace dvr
}  // namespace android
