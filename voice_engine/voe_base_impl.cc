/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "voice_engine/voe_base_impl.h"

#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "common_audio/signal_processing/include/signal_processing_library.h"
#include "modules/audio_coding/include/audio_coding_module.h"
#include "modules/audio_device/audio_device_impl.h"
#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_base/format_macros.h"
#include "rtc_base/location.h"
#include "rtc_base/logging.h"
#include "voice_engine/channel.h"
#include "voice_engine/include/voe_errors.h"
#include "voice_engine/transmit_mixer.h"
#include "voice_engine/voice_engine_impl.h"

namespace webrtc {

VoEBase* VoEBase::GetInterface(VoiceEngine* voiceEngine) {
  if (nullptr == voiceEngine) {
    return nullptr;
  }
  VoiceEngineImpl* s = static_cast<VoiceEngineImpl*>(voiceEngine);
  s->AddRef();
  return s;
}

VoEBaseImpl::VoEBaseImpl(voe::SharedData* shared)
    : voiceEngineObserverPtr_(nullptr),
      shared_(shared) {}

VoEBaseImpl::~VoEBaseImpl() {
  TerminateInternal();
}

void VoEBaseImpl::OnErrorIsReported(const ErrorCode error) {
  rtc::CritScope cs(&callbackCritSect_);
  int errCode = 0;
  if (error == AudioDeviceObserver::kRecordingError) {
    errCode = VE_RUNTIME_REC_ERROR;
    LOG_F(LS_ERROR) << "VE_RUNTIME_REC_ERROR";
  } else if (error == AudioDeviceObserver::kPlayoutError) {
    errCode = VE_RUNTIME_PLAY_ERROR;
    LOG_F(LS_ERROR) << "VE_RUNTIME_PLAY_ERROR";
  }
  if (voiceEngineObserverPtr_) {
    // Deliver callback (-1 <=> no channel dependency)
    voiceEngineObserverPtr_->CallbackOnError(-1, errCode);
  }
}

void VoEBaseImpl::OnWarningIsReported(const WarningCode warning) {
  rtc::CritScope cs(&callbackCritSect_);
  int warningCode = 0;
  if (warning == AudioDeviceObserver::kRecordingWarning) {
    warningCode = VE_RUNTIME_REC_WARNING;
    LOG_F(LS_WARNING) << "VE_RUNTIME_REC_WARNING";
  } else if (warning == AudioDeviceObserver::kPlayoutWarning) {
    warningCode = VE_RUNTIME_PLAY_WARNING;
    LOG_F(LS_WARNING) << "VE_RUNTIME_PLAY_WARNING";
  }
  if (voiceEngineObserverPtr_) {
    // Deliver callback (-1 <=> no channel dependency)
    voiceEngineObserverPtr_->CallbackOnError(-1, warningCode);
  }
}

int32_t VoEBaseImpl::RecordedDataIsAvailable(
    const void* audio_data,
    const size_t number_of_frames,
    const size_t bytes_per_sample,
    const size_t number_of_channels,
    const uint32_t sample_rate,
    const uint32_t audio_delay_milliseconds,
    const int32_t clock_drift,
    const uint32_t volume,
    const bool key_pressed,
    uint32_t& new_mic_volume) {
  RTC_DCHECK_EQ(2 * number_of_channels, bytes_per_sample);
  RTC_DCHECK(shared_->transmit_mixer() != nullptr);
  RTC_DCHECK(shared_->audio_device() != nullptr);

  uint32_t max_volume = 0;
  uint16_t voe_mic_level = 0;
  // Check for zero to skip this calculation; the consumer may use this to
  // indicate no volume is available.
  if (volume != 0) {
    // Scale from ADM to VoE level range
    if (shared_->audio_device()->MaxMicrophoneVolume(&max_volume) == 0) {
      if (max_volume) {
        voe_mic_level = static_cast<uint16_t>(
            (volume * kMaxVolumeLevel + static_cast<int>(max_volume / 2)) /
            max_volume);
      }
    }
    // We learned that on certain systems (e.g Linux) the voe_mic_level
    // can be greater than the maxVolumeLevel therefore
    // we are going to cap the voe_mic_level to the maxVolumeLevel
    // and change the maxVolume to volume if it turns out that
    // the voe_mic_level is indeed greater than the maxVolumeLevel.
    if (voe_mic_level > kMaxVolumeLevel) {
      voe_mic_level = kMaxVolumeLevel;
      max_volume = volume;
    }
  }

  // Perform channel-independent operations
  // (APM, mix with file, record to file, mute, etc.)
  shared_->transmit_mixer()->PrepareDemux(
      audio_data, number_of_frames, number_of_channels, sample_rate,
      static_cast<uint16_t>(audio_delay_milliseconds), clock_drift,
      voe_mic_level, key_pressed);

  // Copy the audio frame to each sending channel and perform
  // channel-dependent operations (file mixing, mute, etc.), encode and
  // packetize+transmit the RTP packet.
  shared_->transmit_mixer()->ProcessAndEncodeAudio();

  // Scale from VoE to ADM level range.
  uint32_t new_voe_mic_level = shared_->transmit_mixer()->CaptureLevel();
  if (new_voe_mic_level != voe_mic_level) {
    // Return the new volume if AGC has changed the volume.
    return static_cast<int>((new_voe_mic_level * max_volume +
                             static_cast<int>(kMaxVolumeLevel / 2)) /
                            kMaxVolumeLevel);
  }

  return 0;
}

int32_t VoEBaseImpl::NeedMorePlayData(const size_t nSamples,
                                      const size_t nBytesPerSample,
                                      const size_t nChannels,
                                      const uint32_t samplesPerSec,
                                      void* audioSamples,
                                      size_t& nSamplesOut,
                                      int64_t* elapsed_time_ms,
                                      int64_t* ntp_time_ms) {
  RTC_NOTREACHED();
  return 0;
}

void VoEBaseImpl::PushCaptureData(int voe_channel, const void* audio_data,
                                  int bits_per_sample, int sample_rate,
                                  size_t number_of_channels,
                                  size_t number_of_frames) {
  voe::ChannelOwner ch = shared_->channel_manager().GetChannel(voe_channel);
  voe::Channel* channel = ch.channel();
  if (!channel)
    return;
  if (channel->Sending()) {
    // Send the audio to each channel directly without using the APM in the
    // transmit mixer.
    channel->ProcessAndEncodeAudio(static_cast<const int16_t*>(audio_data),
                                   sample_rate, number_of_frames,
                                   number_of_channels);
  }
}

void VoEBaseImpl::PullRenderData(int bits_per_sample,
                                 int sample_rate,
                                 size_t number_of_channels,
                                 size_t number_of_frames,
                                 void* audio_data, int64_t* elapsed_time_ms,
                                 int64_t* ntp_time_ms) {
  RTC_NOTREACHED();
}

int VoEBaseImpl::RegisterVoiceEngineObserver(VoiceEngineObserver& observer) {
  rtc::CritScope cs(&callbackCritSect_);
  if (voiceEngineObserverPtr_) {
    shared_->SetLastError(
        VE_INVALID_OPERATION, kTraceError,
        "RegisterVoiceEngineObserver() observer already enabled");
    return -1;
  }

  // Register the observer in all active channels
  for (voe::ChannelManager::Iterator it(&shared_->channel_manager());
       it.IsValid(); it.Increment()) {
    it.GetChannel()->RegisterVoiceEngineObserver(observer);
  }

  shared_->transmit_mixer()->RegisterVoiceEngineObserver(observer);
  voiceEngineObserverPtr_ = &observer;
  return 0;
}

int VoEBaseImpl::DeRegisterVoiceEngineObserver() {
  rtc::CritScope cs(&callbackCritSect_);
  if (!voiceEngineObserverPtr_) {
    shared_->SetLastError(
        VE_INVALID_OPERATION, kTraceError,
        "DeRegisterVoiceEngineObserver() observer already disabled");
    return 0;
  }
  voiceEngineObserverPtr_ = nullptr;

  // Deregister the observer in all active channels
  for (voe::ChannelManager::Iterator it(&shared_->channel_manager());
       it.IsValid(); it.Increment()) {
    it.GetChannel()->DeRegisterVoiceEngineObserver();
  }

  return 0;
}

int VoEBaseImpl::Init(
    AudioDeviceModule* external_adm,
    AudioProcessing* audio_processing,
    const rtc::scoped_refptr<AudioDecoderFactory>& decoder_factory) {
  RTC_DCHECK(audio_processing);
  rtc::CritScope cs(shared_->crit_sec());
  WebRtcSpl_Init();
  if (shared_->statistics().Initialized()) {
    return 0;
  }
  if (shared_->process_thread()) {
    shared_->process_thread()->Start();
  }

  // Create an internal ADM if the user has not added an external
  // ADM implementation as input to Init().
  if (external_adm == nullptr) {
#if !defined(WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE)
    return -1;
#else
    // Create the internal ADM implementation.
    shared_->set_audio_device(AudioDeviceModule::Create(
        VoEId(shared_->instance_id(), -1),
        AudioDeviceModule::kPlatformDefaultAudio));
    if (shared_->audio_device() == nullptr) {
      shared_->SetLastError(VE_NO_MEMORY, kTraceCritical,
                            "Init() failed to create the ADM");
      return -1;
    }
#endif  // WEBRTC_INCLUDE_INTERNAL_AUDIO_DEVICE
  } else {
    // Use the already existing external ADM implementation.
    shared_->set_audio_device(external_adm);
    LOG_F(LS_INFO)
        << "An external ADM implementation will be used in VoiceEngine";
  }

  // Register the ADM to the process thread, which will drive the error
  // callback mechanism
  if (shared_->process_thread()) {
    shared_->process_thread()->RegisterModule(shared_->audio_device(),
                                              RTC_FROM_HERE);
  }

  bool available = false;

  // --------------------
  // Reinitialize the ADM

  // Register the AudioObserver implementation
  if (shared_->audio_device()->RegisterEventObserver(this) != 0) {
    shared_->SetLastError(
        VE_AUDIO_DEVICE_MODULE_ERROR, kTraceWarning,
        "Init() failed to register event observer for the ADM");
  }

  // Register the AudioTransport implementation
  if (shared_->audio_device()->RegisterAudioCallback(this) != 0) {
    shared_->SetLastError(
        VE_AUDIO_DEVICE_MODULE_ERROR, kTraceWarning,
        "Init() failed to register audio callback for the ADM");
  }

  // ADM initialization
  if (shared_->audio_device()->Init() != 0) {
    shared_->SetLastError(VE_AUDIO_DEVICE_MODULE_ERROR, kTraceError,
                          "Init() failed to initialize the ADM");
    return -1;
  }

  // Initialize the default speaker
  if (shared_->audio_device()->SetPlayoutDevice(
          WEBRTC_VOICE_ENGINE_DEFAULT_DEVICE) != 0) {
    shared_->SetLastError(VE_AUDIO_DEVICE_MODULE_ERROR, kTraceInfo,
                          "Init() failed to set the default output device");
  }
  if (shared_->audio_device()->InitSpeaker() != 0) {
    shared_->SetLastError(VE_CANNOT_ACCESS_SPEAKER_VOL, kTraceInfo,
                          "Init() failed to initialize the speaker");
  }

  // Initialize the default microphone
  if (shared_->audio_device()->SetRecordingDevice(
          WEBRTC_VOICE_ENGINE_DEFAULT_DEVICE) != 0) {
    shared_->SetLastError(VE_SOUNDCARD_ERROR, kTraceInfo,
                          "Init() failed to set the default input device");
  }
  if (shared_->audio_device()->InitMicrophone() != 0) {
    shared_->SetLastError(VE_CANNOT_ACCESS_MIC_VOL, kTraceInfo,
                          "Init() failed to initialize the microphone");
  }

  // Set number of channels
  if (shared_->audio_device()->StereoPlayoutIsAvailable(&available) != 0) {
    shared_->SetLastError(VE_SOUNDCARD_ERROR, kTraceWarning,
                          "Init() failed to query stereo playout mode");
  }
  if (shared_->audio_device()->SetStereoPlayout(available) != 0) {
    shared_->SetLastError(VE_SOUNDCARD_ERROR, kTraceWarning,
                          "Init() failed to set mono/stereo playout mode");
  }

  // TODO(andrew): These functions don't tell us whether stereo recording
  // is truly available. We simply set the AudioProcessing input to stereo
  // here, because we have to wait until receiving the first frame to
  // determine the actual number of channels anyway.
  //
  // These functions may be changed; tracked here:
  // http://code.google.com/p/webrtc/issues/detail?id=204
  shared_->audio_device()->StereoRecordingIsAvailable(&available);
  if (shared_->audio_device()->SetStereoRecording(available) != 0) {
    shared_->SetLastError(VE_SOUNDCARD_ERROR, kTraceWarning,
                          "Init() failed to set mono/stereo recording mode");
  }

  shared_->set_audio_processing(audio_processing);

  // Set the error state for any failures in this block.
  shared_->SetLastError(VE_APM_ERROR);
  // Configure AudioProcessing components.
  // TODO(peah): Move this initialization to webrtcvoiceengine.cc.
  if (audio_processing->high_pass_filter()->Enable(true) != 0) {
    LOG_F(LS_ERROR) << "Failed to enable high pass filter.";
    return -1;
  }
  if (audio_processing->echo_cancellation()->enable_drift_compensation(false) !=
      0) {
    LOG_F(LS_ERROR) << "Failed to disable drift compensation.";
    return -1;
  }
  if (audio_processing->noise_suppression()->set_level(kDefaultNsMode) != 0) {
    LOG_F(LS_ERROR) << "Failed to set noise suppression level: "
        << kDefaultNsMode;
    return -1;
  }
  GainControl* agc = audio_processing->gain_control();
  if (agc->set_analog_level_limits(kMinVolumeLevel, kMaxVolumeLevel) != 0) {
    LOG_F(LS_ERROR) << "Failed to set analog level limits with minimum: "
        << kMinVolumeLevel << " and maximum: " << kMaxVolumeLevel;
    return -1;
  }
  if (agc->set_mode(kDefaultAgcMode) != 0) {
    LOG_F(LS_ERROR) << "Failed to set mode: " << kDefaultAgcMode;
    return -1;
  }
  if (agc->Enable(kDefaultAgcState) != 0) {
    LOG_F(LS_ERROR) << "Failed to set agc state: " << kDefaultAgcState;
    return -1;
  }
  shared_->SetLastError(0);  // Clear error state.

#ifdef WEBRTC_VOICE_ENGINE_AGC
  bool agc_enabled =
      agc->mode() == GainControl::kAdaptiveAnalog && agc->is_enabled();
  if (shared_->audio_device()->SetAGC(agc_enabled) != 0) {
    LOG_F(LS_ERROR) << "Failed to set agc to enabled: " << agc_enabled;
    shared_->SetLastError(VE_AUDIO_DEVICE_MODULE_ERROR);
    // TODO(ajm): No error return here due to
    // https://code.google.com/p/webrtc/issues/detail?id=1464
  }
#endif

  if (decoder_factory)
    decoder_factory_ = decoder_factory;
  else
    decoder_factory_ = CreateBuiltinAudioDecoderFactory();

  return shared_->statistics().SetInitialized();
}

int VoEBaseImpl::Terminate() {
  rtc::CritScope cs(shared_->crit_sec());
  return TerminateInternal();
}

int VoEBaseImpl::CreateChannel() {
  return CreateChannel(ChannelConfig());
}

int VoEBaseImpl::CreateChannel(const ChannelConfig& config) {
  rtc::CritScope cs(shared_->crit_sec());
  if (!shared_->statistics().Initialized()) {
    shared_->SetLastError(VE_NOT_INITED, kTraceError);
    return -1;
  }

  ChannelConfig config_copy(config);
  config_copy.acm_config.decoder_factory = decoder_factory_;
  voe::ChannelOwner channel_owner =
      shared_->channel_manager().CreateChannel(config_copy);
  return InitializeChannel(&channel_owner);
}

int VoEBaseImpl::InitializeChannel(voe::ChannelOwner* channel_owner) {
  if (channel_owner->channel()->SetEngineInformation(
          shared_->statistics(),
          *shared_->process_thread(), *shared_->audio_device(),
          voiceEngineObserverPtr_, &callbackCritSect_,
          shared_->encoder_queue()) != 0) {
    shared_->SetLastError(
        VE_CHANNEL_NOT_CREATED, kTraceError,
        "CreateChannel() failed to associate engine and channel."
        " Destroying channel.");
    shared_->channel_manager().DestroyChannel(
        channel_owner->channel()->ChannelId());
    return -1;
  } else if (channel_owner->channel()->Init() != 0) {
    shared_->SetLastError(
        VE_CHANNEL_NOT_CREATED, kTraceError,
        "CreateChannel() failed to initialize channel. Destroying"
        " channel.");
    shared_->channel_manager().DestroyChannel(
        channel_owner->channel()->ChannelId());
    return -1;
  }
  return channel_owner->channel()->ChannelId();
}

int VoEBaseImpl::DeleteChannel(int channel) {
  rtc::CritScope cs(shared_->crit_sec());
  if (!shared_->statistics().Initialized()) {
    shared_->SetLastError(VE_NOT_INITED, kTraceError);
    return -1;
  }

  {
    voe::ChannelOwner ch = shared_->channel_manager().GetChannel(channel);
    voe::Channel* channelPtr = ch.channel();
    if (channelPtr == nullptr) {
      shared_->SetLastError(VE_CHANNEL_NOT_VALID, kTraceError,
                            "DeleteChannel() failed to locate channel");
      return -1;
    }
  }

  shared_->channel_manager().DestroyChannel(channel);
  if (StopSend() != 0) {
    return -1;
  }
  if (StopPlayout() != 0) {
    return -1;
  }
  return 0;
}

int VoEBaseImpl::StartPlayout(int channel) {
  rtc::CritScope cs(shared_->crit_sec());
  if (!shared_->statistics().Initialized()) {
    shared_->SetLastError(VE_NOT_INITED, kTraceError);
    return -1;
  }
  voe::ChannelOwner ch = shared_->channel_manager().GetChannel(channel);
  voe::Channel* channelPtr = ch.channel();
  if (channelPtr == nullptr) {
    shared_->SetLastError(VE_CHANNEL_NOT_VALID, kTraceError,
                          "StartPlayout() failed to locate channel");
    return -1;
  }
  if (channelPtr->Playing()) {
    return 0;
  }
  if (StartPlayout() != 0) {
    shared_->SetLastError(VE_AUDIO_DEVICE_MODULE_ERROR, kTraceError,
                          "StartPlayout() failed to start playout");
    return -1;
  }
  return channelPtr->StartPlayout();
}

int VoEBaseImpl::StopPlayout(int channel) {
  rtc::CritScope cs(shared_->crit_sec());
  if (!shared_->statistics().Initialized()) {
    shared_->SetLastError(VE_NOT_INITED, kTraceError);
    return -1;
  }
  voe::ChannelOwner ch = shared_->channel_manager().GetChannel(channel);
  voe::Channel* channelPtr = ch.channel();
  if (channelPtr == nullptr) {
    shared_->SetLastError(VE_CHANNEL_NOT_VALID, kTraceError,
                          "StopPlayout() failed to locate channel");
    return -1;
  }
  if (channelPtr->StopPlayout() != 0) {
    LOG_F(LS_WARNING) << "StopPlayout() failed to stop playout for channel "
                      << channel;
  }
  return StopPlayout();
}

int VoEBaseImpl::StartSend(int channel) {
  rtc::CritScope cs(shared_->crit_sec());
  if (!shared_->statistics().Initialized()) {
    shared_->SetLastError(VE_NOT_INITED, kTraceError);
    return -1;
  }
  voe::ChannelOwner ch = shared_->channel_manager().GetChannel(channel);
  voe::Channel* channelPtr = ch.channel();
  if (channelPtr == nullptr) {
    shared_->SetLastError(VE_CHANNEL_NOT_VALID, kTraceError,
                          "StartSend() failed to locate channel");
    return -1;
  }
  if (channelPtr->Sending()) {
    return 0;
  }
  if (StartSend() != 0) {
    shared_->SetLastError(VE_AUDIO_DEVICE_MODULE_ERROR, kTraceError,
                          "StartSend() failed to start recording");
    return -1;
  }
  return channelPtr->StartSend();
}

int VoEBaseImpl::StopSend(int channel) {
  rtc::CritScope cs(shared_->crit_sec());
  if (!shared_->statistics().Initialized()) {
    shared_->SetLastError(VE_NOT_INITED, kTraceError);
    return -1;
  }
  voe::ChannelOwner ch = shared_->channel_manager().GetChannel(channel);
  voe::Channel* channelPtr = ch.channel();
  if (channelPtr == nullptr) {
    shared_->SetLastError(VE_CHANNEL_NOT_VALID, kTraceError,
                          "StopSend() failed to locate channel");
    return -1;
  }
  channelPtr->StopSend();
  return StopSend();
}

int32_t VoEBaseImpl::StartPlayout() {
  if (!shared_->audio_device()->Playing()) {
    if (shared_->audio_device()->InitPlayout() != 0) {
      LOG_F(LS_ERROR) << "Failed to initialize playout";
      return -1;
    }
    if (shared_->audio_device()->StartPlayout() != 0) {
      LOG_F(LS_ERROR) << "Failed to start playout";
      return -1;
    }
  }
  return 0;
}

int32_t VoEBaseImpl::StopPlayout() {
  // Stop audio-device playing if no channel is playing out
  if (shared_->NumOfPlayingChannels() == 0) {
    if (shared_->audio_device()->StopPlayout() != 0) {
      shared_->SetLastError(VE_CANNOT_STOP_PLAYOUT, kTraceError,
                            "StopPlayout() failed to stop playout");
      return -1;
    }
  }
  return 0;
}

int32_t VoEBaseImpl::StartSend() {
  if (!shared_->audio_device()->RecordingIsInitialized() &&
      !shared_->audio_device()->Recording()) {
    if (shared_->audio_device()->InitRecording() != 0) {
      LOG_F(LS_ERROR) << "Failed to initialize recording";
      return -1;
    }
  }
  if (!shared_->audio_device()->Recording()) {
    if (shared_->audio_device()->StartRecording() != 0) {
      LOG_F(LS_ERROR) << "Failed to start recording";
      return -1;
    }
  }
  return 0;
}

int32_t VoEBaseImpl::StopSend() {
  if (shared_->NumOfSendingChannels() == 0) {
    // Stop audio-device recording if no channel is recording
    if (shared_->audio_device()->StopRecording() != 0) {
      shared_->SetLastError(VE_CANNOT_STOP_RECORDING, kTraceError,
                            "StopSend() failed to stop recording");
      return -1;
    }
    shared_->transmit_mixer()->StopSend();
  }

  return 0;
}

int32_t VoEBaseImpl::TerminateInternal() {
  // Delete any remaining channel objects
  shared_->channel_manager().DestroyAllChannels();

  if (shared_->process_thread()) {
    if (shared_->audio_device()) {
      shared_->process_thread()->DeRegisterModule(shared_->audio_device());
    }
    shared_->process_thread()->Stop();
  }

  if (shared_->audio_device()) {
    if (shared_->audio_device()->StopPlayout() != 0) {
      shared_->SetLastError(VE_SOUNDCARD_ERROR, kTraceWarning,
                            "TerminateInternal() failed to stop playout");
    }
    if (shared_->audio_device()->StopRecording() != 0) {
      shared_->SetLastError(VE_SOUNDCARD_ERROR, kTraceWarning,
                            "TerminateInternal() failed to stop recording");
    }
    if (shared_->audio_device()->RegisterEventObserver(nullptr) != 0) {
      shared_->SetLastError(
          VE_AUDIO_DEVICE_MODULE_ERROR, kTraceWarning,
          "TerminateInternal() failed to de-register event observer "
          "for the ADM");
    }
    if (shared_->audio_device()->RegisterAudioCallback(nullptr) != 0) {
      shared_->SetLastError(
          VE_AUDIO_DEVICE_MODULE_ERROR, kTraceWarning,
          "TerminateInternal() failed to de-register audio callback "
          "for the ADM");
    }
    if (shared_->audio_device()->Terminate() != 0) {
      shared_->SetLastError(VE_AUDIO_DEVICE_MODULE_ERROR, kTraceError,
                            "TerminateInternal() failed to terminate the ADM");
    }
    shared_->set_audio_device(nullptr);
  }

  shared_->set_audio_processing(nullptr);

  return shared_->statistics().SetUnInitialized();
}
}  // namespace webrtc
