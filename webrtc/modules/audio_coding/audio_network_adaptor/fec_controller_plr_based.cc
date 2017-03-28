/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/audio_coding/audio_network_adaptor/fec_controller_plr_based.h"

#include <limits>
#include <utility>

#include "webrtc/base/checks.h"
#include "webrtc/system_wrappers/include/field_trial.h"

namespace webrtc {

namespace {
class NullSmoothingFilter final : public SmoothingFilter {
 public:
  void AddSample(float sample) override {
    last_sample_ = rtc::Optional<float>(sample);
  }

  rtc::Optional<float> GetAverage() override { return last_sample_; }

  bool SetTimeConstantMs(int time_constant_ms) override {
    RTC_NOTREACHED();
    return false;
  }

 private:
  rtc::Optional<float> last_sample_;
};
}

FecControllerPlrBased::Config::Threshold::Threshold(
    int low_bandwidth_bps,
    float low_bandwidth_packet_loss,
    int high_bandwidth_bps,
    float high_bandwidth_packet_loss)
    : low_bandwidth_bps(low_bandwidth_bps),
      low_bandwidth_packet_loss(low_bandwidth_packet_loss),
      high_bandwidth_bps(high_bandwidth_bps),
      high_bandwidth_packet_loss(high_bandwidth_packet_loss) {}

FecControllerPlrBased::Config::Config(bool initial_fec_enabled,
                                      const Threshold& fec_enabling_threshold,
                                      const Threshold& fec_disabling_threshold,
                                      int time_constant_ms,
                                      const Clock* clock)
    : initial_fec_enabled(initial_fec_enabled),
      fec_enabling_threshold(fec_enabling_threshold),
      fec_disabling_threshold(fec_disabling_threshold),
      time_constant_ms(time_constant_ms),
      clock(clock) {}

FecControllerPlrBased::FecControllerPlrBased(
    const Config& config,
    std::unique_ptr<SmoothingFilter> smoothing_filter)
    : config_(config),
      fec_enabled_(config.initial_fec_enabled),
      packet_loss_smoother_(std::move(smoothing_filter)),
      fec_enabling_threshold_info_(config_.fec_enabling_threshold),
      fec_disabling_threshold_info_(config_.fec_disabling_threshold) {
  RTC_DCHECK_LE(fec_enabling_threshold_info_.slope, 0);
  RTC_DCHECK_LE(fec_enabling_threshold_info_.slope, 0);
  RTC_DCHECK_LE(
      GetPacketLossThreshold(config_.fec_enabling_threshold.low_bandwidth_bps,
                             config_.fec_disabling_threshold,
                             fec_disabling_threshold_info_),
      config_.fec_enabling_threshold.low_bandwidth_packet_loss);
  RTC_DCHECK_LE(
      GetPacketLossThreshold(config_.fec_enabling_threshold.high_bandwidth_bps,
                             config_.fec_disabling_threshold,
                             fec_disabling_threshold_info_),
      config_.fec_enabling_threshold.high_bandwidth_packet_loss);
}

FecControllerPlrBased::FecControllerPlrBased(const Config& config)
    : FecControllerPlrBased(
          config,
          webrtc::field_trial::FindFullName("UseTwccPlrForAna") == "Enabled"
              ? std::unique_ptr<NullSmoothingFilter>(new NullSmoothingFilter())
              : std::unique_ptr<SmoothingFilter>(
                    new SmoothingFilterImpl(config.time_constant_ms,
                                            config.clock))) {}

FecControllerPlrBased::~FecControllerPlrBased() = default;

void FecControllerPlrBased::UpdateNetworkMetrics(
    const NetworkMetrics& network_metrics) {
  if (network_metrics.uplink_bandwidth_bps)
    uplink_bandwidth_bps_ = network_metrics.uplink_bandwidth_bps;
  if (network_metrics.uplink_packet_loss_fraction) {
    packet_loss_smoother_->AddSample(
        *network_metrics.uplink_packet_loss_fraction);
  }
}

void FecControllerPlrBased::MakeDecision(
    AudioNetworkAdaptor::EncoderRuntimeConfig* config) {
  RTC_DCHECK(!config->enable_fec);
  RTC_DCHECK(!config->uplink_packet_loss_fraction);

  const auto& packet_loss = packet_loss_smoother_->GetAverage();

  fec_enabled_ = fec_enabled_ ? !FecDisablingDecision(packet_loss)
                              : FecEnablingDecision(packet_loss);

  config->enable_fec = rtc::Optional<bool>(fec_enabled_);

  config->uplink_packet_loss_fraction =
      rtc::Optional<float>(packet_loss ? *packet_loss : 0.0);
}

FecControllerPlrBased::ThresholdInfo::ThresholdInfo(
    const Config::Threshold& threshold) {
  int bandwidth_diff_bps =
      threshold.high_bandwidth_bps - threshold.low_bandwidth_bps;
  float packet_loss_diff = threshold.high_bandwidth_packet_loss -
                           threshold.low_bandwidth_packet_loss;
  slope = bandwidth_diff_bps == 0 ? 0.0 : packet_loss_diff / bandwidth_diff_bps;
  offset =
      threshold.low_bandwidth_packet_loss - slope * threshold.low_bandwidth_bps;
}

float FecControllerPlrBased::GetPacketLossThreshold(
    int bandwidth_bps,
    const Config::Threshold& threshold,
    const ThresholdInfo& threshold_info) const {
  if (bandwidth_bps < threshold.low_bandwidth_bps) {
    return std::numeric_limits<float>::max();
  } else if (bandwidth_bps >= threshold.high_bandwidth_bps) {
    return threshold.high_bandwidth_packet_loss;
  } else {
    float rc = threshold_info.offset + threshold_info.slope * bandwidth_bps;
    RTC_DCHECK_LE(rc, threshold.low_bandwidth_packet_loss);
    RTC_DCHECK_GE(rc, threshold.high_bandwidth_packet_loss);
    return rc;
  }
}

bool FecControllerPlrBased::FecEnablingDecision(
    const rtc::Optional<float>& packet_loss) const {
  if (!uplink_bandwidth_bps_)
    return false;
  if (!packet_loss)
    return false;
  return *packet_loss >= GetPacketLossThreshold(*uplink_bandwidth_bps_,
                                                config_.fec_enabling_threshold,
                                                fec_enabling_threshold_info_);
}

bool FecControllerPlrBased::FecDisablingDecision(
    const rtc::Optional<float>& packet_loss) const {
  if (!uplink_bandwidth_bps_)
    return false;
  if (!packet_loss)
    return false;
  return *packet_loss <= GetPacketLossThreshold(*uplink_bandwidth_bps_,
                                                config_.fec_disabling_threshold,
                                                fec_disabling_threshold_info_);
}

}  // namespace webrtc