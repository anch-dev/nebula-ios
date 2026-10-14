// SPDX-License-Identifier: GPL-3.0-only
// Included by native_host.cpp (namespace galaxy::host, anonymous) on
// non-Windows hosts: the audio sink with the same interface as the XAudio2
// NativeAudioSink.
//
// The game's AI DMA buffers (big-endian stereo PCM16 at 32 or 48 kHz) are
// converted, gain-adjusted, resampled to 48 kHz and queued in a single-
// producer/single-consumer ring the output device drains:
// - Apple (macOS, iOS): an AudioUnit output (RemoteIO / DefaultOutput) whose
//   render callback reads the ring, rendering silence on underrun;
// - elsewhere: no device; buffers are accepted and discarded, so headless
//   runs behave like a machine with its output muted.
// Pacing comes from the guest's audio DMA deadlines, as on Windows; a full
// ring drops the newest frames and counts them.

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#endif

class NativeAudioSink final {
public:
    NativeAudioSink() = default;
    ~NativeAudioSink() {
        shutdown();
        const AudioSinkStats health = stats();
        std::cout << std::dec << "[audio-route-health] scope=process-audio backend="
                  << backend_name() << " ready=" << (health.initialization_ready ? 1 : 0)
                  << " submitted=" << health.submitted_buffers
                  << " completed=" << health.completed_buffers
                  << " dropped=" << health.dropped_buffers
                  << " queue-empty-events=" << health.queue_empty_events << '\n';
    }
    NativeAudioSink(const NativeAudioSink&) = delete;
    NativeAudioSink& operator=(const NativeAudioSink&) = delete;

    using FailureNotifier = NativeAudioFailureNotifier::Callback;

    void set_failure_notifier(FailureNotifier notifier, void* user) noexcept {
        failure_notifier_.set(notifier, user);
    }

    bool preinitialize() {
        const std::lock_guard<std::mutex> lock(control_mutex_);
        ++initialization_attempts_;
        ++preinitialize_calls_;
        if (!initialized_) initialized_ = initialize_backend();
        return initialized_;
    }

    bool submit_pcm16_be_stereo(std::span<const std::byte> source, std::uint32_t sample_rate,
                                std::uint32_t /*source_address*/,
                                std::uint64_t /*guest_start_ticks*/) {
        if (source.empty()) return true;
        if ((source.size() & 3u) != 0u) {
            strict_failure_.store(true, std::memory_order_release);
            set_strict_failure_reason("invalid AI PCM buffer size");
            return false;
        }
        if (sample_rate != 32'000u && sample_rate != 48'000u) return false;
        {
            const std::lock_guard<std::mutex> lock(control_mutex_);
            if (!initialized_) {
                ++initialization_attempts_;
                initialized_ = initialize_backend();
            }
        }
        first_submit_observed_.store(true, std::memory_order_release);

        std::vector<std::byte> pcm_le(source.size());
        if (!native_audio_convert_wii_ai_pcm16(source, std::span<std::byte>(pcm_le))) {
            dropped_buffers_.fetch_add(1u, std::memory_order_relaxed);
            return false;
        }
        apply_output_master_gain(pcm_le);
        if (const char* const dump = std::getenv("GALAXY_AUDIO_WAV_DUMP");
            dump != nullptr && *dump != '\0') {
            (void)wav_dump_.append(std::span<const std::byte>(pcm_le), sample_rate);
            if (read_env_flag("GALAXY_AUDIO_WAV_DUMP_ONLY")) {
                submitted_buffers_.fetch_add(1u, std::memory_order_relaxed);
                completed_buffers_.fetch_add(1u, std::memory_order_relaxed);
                return true;
            }
        }
        submitted_buffers_.fetch_add(1u, std::memory_order_relaxed);
        if (!device_running_.load(std::memory_order_acquire)) {
            // No output device: the buffer is consumed silently.
            completed_buffers_.fetch_add(1u, std::memory_order_relaxed);
            return true;
        }
        const std::size_t frames = pcm_le.size() / 4u;
        std::vector<std::int16_t> samples(frames * 2u);
        std::memcpy(samples.data(), pcm_le.data(), pcm_le.size());
        if (!resample_into_ring(samples, sample_rate)) {
            dropped_buffers_.fetch_add(1u, std::memory_order_relaxed);
        }
        completed_buffers_.fetch_add(1u, std::memory_order_relaxed);
        return true;
    }

    unsigned queued_buffers() const {
        // Queued output in 5 ms units, the size of a typical AI DMA block.
        const std::size_t frames = ring_frames();
        return static_cast<unsigned>(frames / (kDeviceRate / 200u));
    }

    void record_anomaly(NativeAudioAnomalyRecord) noexcept {}

    std::uint64_t dropped_buffers() const {
        return dropped_buffers_.load(std::memory_order_relaxed);
    }

    bool output_available() const noexcept { return device_running_.load(std::memory_order_acquire); }

    void poll_health() {}

    AudioSinkStats stats() const {
        AudioSinkStats stats{};
        stats.initialization_attempts = initialization_attempts_;
        stats.preinitialize_calls = preinitialize_calls_;
        stats.initialization_ready = initialized_;
        stats.first_submit_observed = first_submit_observed_.load(std::memory_order_acquire);
        stats.submitted_buffers = submitted_buffers_.load(std::memory_order_relaxed);
        stats.completed_buffers = completed_buffers_.load(std::memory_order_relaxed);
        stats.dropped_buffers = dropped_buffers_.load(std::memory_order_relaxed);
        stats.queue_empty_events = underruns_.load(std::memory_order_relaxed);
        return stats;
    }

    void reset_measurement_window() {}

    bool strict_failure() const { return strict_failure_.load(std::memory_order_acquire); }

    // Test hooks shared with the XAudio2 sink. Ownership: a sink created,
    // fed and destroyed on a thread other than the simulation thread.
    bool test_recovery_ownership() {
        if (!preinitialize()) return false;
        std::array<std::byte, 640> silence{};
        return submit_pcm16_be_stereo(silence, 32'000u, 0u, 0u) && !strict_failure();
    }

    // Backpressure: far more audio than the ring holds stays bounded; with a
    // device the overflow is dropped and counted, without one every buffer
    // is consumed.
    bool test_recovery_backpressure(unsigned test_case) {
        if (!preinitialize()) return false;
        const std::uint32_t rate = test_case % 2u == 0u ? 32'000u : 48'000u;
        std::vector<std::byte> block(rate / 100u * 4u);  // 10 ms
        for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<std::byte>(i * 7u);
        const unsigned blocks = 200u;  // 2 s, several times the ring
        for (unsigned i = 0; i < blocks; ++i) {
            if (!submit_pcm16_be_stereo(block, rate, 0u, 0u)) return false;
        }
        const AudioSinkStats health = stats();
        if (health.submitted_buffers != blocks || health.completed_buffers != blocks) return false;
        if (ring_frames() > kRingFrames) return false;
        return output_available() ? true : health.dropped_buffers == 0u;
    }

    std::string strict_failure_reason() const {
        const std::lock_guard<std::mutex> lock(reason_mutex_);
        return strict_failure_reason_;
    }

private:
    static constexpr std::uint32_t kDeviceRate = 48'000u;
    static constexpr std::size_t kRingFrames = 16'384u;  // ~340 ms at 48 kHz

    const char* backend_name() const noexcept {
#if defined(__APPLE__)
        return "audiounit";
#else
        return "none";
#endif
    }

    void set_strict_failure_reason(const char* reason) {
        const std::lock_guard<std::mutex> lock(reason_mutex_);
        if (strict_failure_reason_.empty()) strict_failure_reason_ = reason;
    }

    // Same ramped master gain as the Windows sink.
    void apply_output_master_gain(std::vector<std::byte>& pcm_le) noexcept {
        constexpr std::uint32_t kUnity = galaxy::audio::kNativeAudioUnityGainQ16;
        const std::uint32_t target =
            galaxy::audio::native_audio_gain_to_q16(galaxy::get_runtime_settings().audio.master);
        const std::uint32_t start = output_master_started_ ? output_master_q16_ : target;
        output_master_started_ = true;
        output_master_q16_ = target;
        const std::size_t frames = pcm_le.size() / 4u;
        if ((start == kUnity && target == kUnity) || frames == 0u) return;
        const auto scale = [](std::byte* sample, std::int64_t gain) noexcept {
            const auto bits = static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(sample[0]) |
                                                         (std::to_integer<std::uint16_t>(sample[1]) << 8u));
            const std::int64_t scaled = std::clamp<std::int64_t>(
                (static_cast<std::int64_t>(static_cast<std::int16_t>(bits)) * gain) >> 16u, -32768, 32767);
            const auto out = static_cast<std::uint16_t>(static_cast<std::int16_t>(scaled));
            sample[0] = static_cast<std::byte>(out & 0xFFu);
            sample[1] = static_cast<std::byte>(out >> 8u);
        };
        const std::int64_t delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(start);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::int64_t gain = static_cast<std::int64_t>(start) +
                delta * static_cast<std::int64_t>(frame + 1u) / static_cast<std::int64_t>(frames);
            scale(pcm_le.data() + frame * 4u, gain);
            scale(pcm_le.data() + frame * 4u + 2u, gain);
        }
    }

    // --- ring (producer: submit thread; consumer: device thread) ---------

    std::size_t ring_frames() const noexcept {
        return write_index_.load(std::memory_order_acquire) - read_index_.load(std::memory_order_acquire);
    }

    bool push_frame(std::int16_t left, std::int16_t right) noexcept {
        const std::size_t write = write_index_.load(std::memory_order_relaxed);
        if (write - read_index_.load(std::memory_order_acquire) >= kRingFrames) return false;
        ring_[(write % kRingFrames) * 2u] = left;
        ring_[(write % kRingFrames) * 2u + 1u] = right;
        write_index_.store(write + 1u, std::memory_order_release);
        return true;
    }

    // Linear-interpolation resampling to the device rate, continuous across
    // buffers. Returns false when the ring overflowed.
    bool resample_into_ring(const std::vector<std::int16_t>& samples, std::uint32_t rate) {
        const std::size_t frames = samples.size() / 2u;
        if (rate != resample_rate_) {
            resample_rate_ = rate;
            resample_phase_ = 0.0;
            have_previous_ = false;
        }
        bool fit = true;
        if (rate == kDeviceRate) {
            for (std::size_t i = 0; i < frames; ++i) {
                fit &= push_frame(samples[i * 2u], samples[i * 2u + 1u]);
            }
            return fit;
        }
        const double step = static_cast<double>(rate) / kDeviceRate;
        // Position `phase` runs over [previous frame, this buffer's frames].
        const auto frame_at = [&](std::size_t index, unsigned channel) -> double {
            if (index == 0u) {
                return have_previous_ ? previous_[channel] : samples[channel];
            }
            return samples[(index - 1u) * 2u + channel];
        };
        while (resample_phase_ < static_cast<double>(frames)) {
            const auto base = static_cast<std::size_t>(resample_phase_);
            const double fraction = resample_phase_ - static_cast<double>(base);
            std::int16_t out[2];
            for (unsigned channel = 0; channel < 2u; ++channel) {
                const double a = frame_at(base, channel);
                const double b = frame_at(base + 1u, channel);
                out[channel] = static_cast<std::int16_t>(std::lround(a + (b - a) * fraction));
            }
            fit &= push_frame(out[0], out[1]);
            resample_phase_ += step;
        }
        resample_phase_ -= static_cast<double>(frames);
        previous_[0] = samples[(frames - 1u) * 2u];
        previous_[1] = samples[(frames - 1u) * 2u + 1u];
        have_previous_ = true;
        return fit;
    }

    // Device thread: copies `frames` stereo frames, silence where the ring
    // runs dry.
    void pull(std::int16_t* out, std::size_t frames) noexcept {
        const std::size_t read = read_index_.load(std::memory_order_relaxed);
        const std::size_t available =
            std::min(frames, write_index_.load(std::memory_order_acquire) - read);
        for (std::size_t i = 0; i < available; ++i) {
            out[i * 2u] = ring_[((read + i) % kRingFrames) * 2u];
            out[i * 2u + 1u] = ring_[((read + i) % kRingFrames) * 2u + 1u];
        }
        if (available < frames) {
            std::memset(out + available * 2u, 0, (frames - available) * 2u * sizeof(std::int16_t));
            if (first_submit_observed_.load(std::memory_order_relaxed)) {
                underruns_.fetch_add(1u, std::memory_order_relaxed);
            }
        }
        read_index_.store(read + available, std::memory_order_release);
    }

    // --- device ----------------------------------------------------------

#if defined(__APPLE__)
    static OSStatus render(void* context, AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                           UInt32, UInt32 frames, AudioBufferList* data) {
        auto* const self = static_cast<NativeAudioSink*>(context);
        if (data == nullptr || data->mNumberBuffers == 0u || data->mBuffers[0].mData == nullptr) {
            return noErr;
        }
        const std::size_t capacity = data->mBuffers[0].mDataByteSize / 4u;
        self->pull(static_cast<std::int16_t*>(data->mBuffers[0].mData),
                   std::min<std::size_t>(frames, capacity));
        return noErr;
    }

    bool initialize_backend() {
        AudioComponentDescription description{};
        description.componentType = kAudioUnitType_Output;
#if TARGET_OS_IPHONE
        description.componentSubType = kAudioUnitSubType_RemoteIO;
#else
        description.componentSubType = kAudioUnitSubType_DefaultOutput;
#endif
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (component == nullptr || AudioComponentInstanceNew(component, &unit_) != noErr) {
            std::cerr << "[audio] no AudioUnit output available\n";
            unit_ = nullptr;
            return true;  // initialized, without a device: buffers are discarded
        }
        AudioStreamBasicDescription format{};
        format.mSampleRate = kDeviceRate;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        format.mFramesPerPacket = 1u;
        format.mChannelsPerFrame = 2u;
        format.mBitsPerChannel = 16u;
        format.mBytesPerFrame = 4u;
        format.mBytesPerPacket = 4u;
        AURenderCallbackStruct callback{};
        callback.inputProc = &NativeAudioSink::render;
        callback.inputProcRefCon = this;
        const bool configured =
            AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                 &format, sizeof(format)) == noErr &&
            AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                                 kAudioUnitScope_Input, 0, &callback, sizeof(callback)) == noErr &&
            AudioUnitInitialize(unit_) == noErr && AudioOutputUnitStart(unit_) == noErr;
        if (!configured) {
            std::cerr << "[audio] AudioUnit output could not start\n";
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            return true;
        }
        device_running_.store(true, std::memory_order_release);
        return true;
    }

    void shutdown() noexcept {
        if (unit_ != nullptr) {
            device_running_.store(false, std::memory_order_release);
            AudioOutputUnitStop(unit_);
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
        (void)wav_dump_.finish();
    }

    AudioUnit unit_ = nullptr;
#else
    bool initialize_backend() { return true; }
    void shutdown() noexcept { (void)wav_dump_.finish(); }
#endif

    NativeAudioFailureNotifier failure_notifier_{};
    AudioWavDump wav_dump_{};
    mutable std::mutex control_mutex_;
    mutable std::mutex reason_mutex_;
    std::string strict_failure_reason_;
    std::atomic<bool> strict_failure_{false};
    bool initialized_ = false;
    std::uint64_t initialization_attempts_ = 0u;
    std::uint64_t preinitialize_calls_ = 0u;
    std::atomic<bool> first_submit_observed_{false};
    std::atomic<bool> device_running_{false};
    std::atomic<std::uint64_t> submitted_buffers_{0u};
    std::atomic<std::uint64_t> completed_buffers_{0u};
    std::atomic<std::uint64_t> dropped_buffers_{0u};
    std::atomic<std::uint64_t> underruns_{0u};
    std::uint32_t output_master_q16_ = galaxy::audio::kNativeAudioUnityGainQ16;
    bool output_master_started_ = false;
    std::array<std::int16_t, kRingFrames * 2u> ring_{};
    std::atomic<std::size_t> write_index_{0u};
    std::atomic<std::size_t> read_index_{0u};
    std::uint32_t resample_rate_ = 0u;
    double resample_phase_ = 0.0;
    double previous_[2]{};
    bool have_previous_ = false;
};
