/*
 * Copyright (C) 2010, Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "third_party/blink/renderer/modules/webaudio/audio_node_input.h"

#include <algorithm>
#include <memory>

#include "base/memory/ptr_util.h"
#include "third_party/blink/renderer/modules/webaudio/audio_node_output.h"

namespace blink {

inline AudioNodeInput::AudioNodeInput(AudioHandler& handler)
    : AudioSummingJunction(handler.Context()->GetDeferredTaskHandler()),
      handler_(handler) {
  // Set to mono by default.
  internal_summing_bus_ =
      AudioBus::Create(1, audio_utilities::kRenderQuantumFrames);
}

std::unique_ptr<AudioNodeInput> AudioNodeInput::Create(AudioHandler& handler) {
  return base::WrapUnique(new AudioNodeInput(handler));
}

void AudioNodeInput::Connect(AudioNodeOutput& output) {
  GetDeferredTaskHandler().AssertGraphOwner();

  // Check if we're already connected to this output.
  if (outputs_.Contains(&output))
    return;

  output.AddInput(*this);
  outputs_.insert(&output);
  ChangedOutputs();
}

void AudioNodeInput::Disconnect(AudioNodeOutput& output) {
  GetDeferredTaskHandler().AssertGraphOwner();

  // First try to disconnect from "active" connections.
  if (outputs_.Contains(&output)) {
    outputs_.erase(&output);
    ChangedOutputs();
    output.RemoveInput(*this);
    // Note: it's important to return immediately after removeInput() calls
    // since the node may be deleted.
    return;
  }

  // Otherwise, try to disconnect from disabled connections.
  if (disabled_outputs_.Contains(&output)) {
    disabled_outputs_.erase(&output);
    output.RemoveInput(*this);
    // Note: it's important to return immediately after all removeInput() calls
    // since the node may be deleted.
    return;
  }

  NOTREACHED();
}

void AudioNodeInput::Disable(AudioNodeOutput& output) {
  GetDeferredTaskHandler().AssertGraphOwner();
  DCHECK(outputs_.Contains(&output));

  disabled_outputs_.insert(&output);
  outputs_.erase(&output);
  ChangedOutputs();

  // Propagate disabled state to outputs.
  Handler().DisableOutputsIfNecessary();
}

void AudioNodeInput::Enable(AudioNodeOutput& output) {
  GetDeferredTaskHandler().AssertGraphOwner();

  // Move output from disabled list to active list.
  outputs_.insert(&output);
  if (disabled_outputs_.size() > 0) {
    DCHECK(disabled_outputs_.Contains(&output));
    disabled_outputs_.erase(&output);
  }
  ChangedOutputs();

  // Propagate enabled state to outputs.
  Handler().EnableOutputsIfNecessary();
}

void AudioNodeInput::DidUpdate() {
  Handler().CheckNumberOfChannelsForInput(this);
}

void AudioNodeInput::UpdateInternalBus() {
  DCHECK(GetDeferredTaskHandler().IsAudioThread());
  GetDeferredTaskHandler().AssertGraphOwner();

  unsigned number_of_input_channels = NumberOfChannels();

  if (number_of_input_channels == internal_summing_bus_->NumberOfChannels())
    return;

  internal_summing_bus_ = AudioBus::Create(
      number_of_input_channels, audio_utilities::kRenderQuantumFrames);
}

unsigned AudioNodeInput::NumberOfChannels() const {
  AudioHandler::ChannelCountMode mode = Handler().InternalChannelCountMode();
  if (mode == AudioHandler::kExplicit)
    return Handler().ChannelCount();

  // Find the number of channels of the connection with the largest number of
  // channels.
  unsigned max_channels = 1;  // one channel is the minimum allowed

  for (AudioNodeOutput* output : outputs_) {
    // Use output()->numberOfChannels() instead of
    // output->bus()->numberOfChannels(), because the calling of
    // AudioNodeOutput::bus() is not safe here.
    max_channels = std::max(max_channels, output->NumberOfChannels());
  }

  if (mode == AudioHandler::kClampedMax)
    max_channels =
        std::min(max_channels, static_cast<unsigned>(Handler().ChannelCount()));

  return max_channels;
}

AudioBus* AudioNodeInput::Bus() {
  DCHECK(GetDeferredTaskHandler().IsAudioThread());

  // Handle single connection specially to allow for in-place processing.
  if (NumberOfRenderingConnections() == 1 &&
      Handler().InternalChannelCountMode() == AudioHandler::kMax)
    return RenderingOutput(0)->Bus();

  // Multiple connections case or complex ChannelCountMode (or no connections).
  return InternalSummingBus();
}

AudioBus* AudioNodeInput::InternalSummingBus() {
  DCHECK(GetDeferredTaskHandler().IsAudioThread());

  return internal_summing_bus_.get();
}

void AudioNodeInput::SumAllConnections(AudioBus* summing_bus,
                                       size_t frames_to_process) {
  DCHECK(GetDeferredTaskHandler().IsAudioThread());

  // We shouldn't be calling this method if there's only one connection, since
  // it's less efficient.
  //    DCHECK(numberOfRenderingConnections() > 1 ||
  //        handler().internalChannelCountMode() != AudioHandler::Max);

  DCHECK(summing_bus);
  if (!summing_bus)
    return;

  summing_bus->Zero();

  AudioBus::ChannelInterpretation interpretation =
      Handler().InternalChannelInterpretation();

  for (unsigned i = 0; i < NumberOfRenderingConnections(); ++i) {
    AudioNodeOutput* output = RenderingOutput(i);
    DCHECK(output);

    // Render audio from this output.
    AudioBus* connection_bus = output->Pull(nullptr, frames_to_process);

    // Sum, with unity-gain.
    summing_bus->SumFrom(*connection_bus, interpretation);
  }
}

AudioBus* AudioNodeInput::Pull(AudioBus* in_place_bus,
                               size_t frames_to_process) {
  DCHECK(GetDeferredTaskHandler().IsAudioThread());

  // Handle single connection case.
  if (NumberOfRenderingConnections() == 1 &&
      Handler().InternalChannelCountMode() == AudioHandler::kMax) {
    // The output will optimize processing using inPlaceBus if it's able.
    AudioNodeOutput* output = this->RenderingOutput(0);
    return output->Pull(in_place_bus, frames_to_process);
  }

  AudioBus* internal_summing_bus = this->InternalSummingBus();

  if (!NumberOfRenderingConnections()) {
    // At least, generate silence if we're not connected to anything.
    // FIXME: if we wanted to get fancy, we could propagate a 'silent hint' here
    // to optimize the downstream graph processing.
    internal_summing_bus->Zero();
    return internal_summing_bus;
  }

  // Handle multiple connections case.
  SumAllConnections(internal_summing_bus, frames_to_process);

  return internal_summing_bus;
}

}  // namespace blink
