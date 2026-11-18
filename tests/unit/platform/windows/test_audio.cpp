/**
 * @file tests/unit/platform/windows/test_audio.cpp
 * @brief Tests for Windows audio sink selection and endpoint-change handling.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <algorithm>
  #include <array>
  #include <cstdlib>
  #include <cstring>
  #include <functional>
  #include <optional>
  #include <string>
  #include <tuple>
  #include <type_traits>
  #include <utility>
  #include <vector>

  // platform includes
  #include <mmdeviceapi.h>
  #include <mmreg.h>
  #include <propsys.h>

  // local includes
  #include "src/config.h"
  #include "src/platform/common.h"

namespace platf::audio::tests {
  std::vector<WAVEFORMATEXTENSIBLE> virtual_sink_waveformats(WORD channel_count);
  int initialize_audio_control(const std::function<std::remove_pointer_t<decltype(&CoCreateInstance)>> &create_instance);
  std::optional<sink_t> configured_sink_info();
  int set_external_sink(const std::string &sink);
  bool sink_device_available(const std::string &sink, IMMDeviceEnumerator *device_enum);
  bool microphone_available(const std::string &assigned_sink, const std::string &configured_sink, IMMDeviceEnumerator *device_enum);
  bool capture_follows_default_device(IMMDeviceEnumerator *device_enum, IMMDevice *capture_device);
  capture_e simulate_default_device_change(bool follows_default_device, bool install_callback, bool render_device_changed, int &callback_count);
  std::optional<WAVEFORMATEXTENSIBLE> endpoint_waveformat(const WAVEFORMATEX *format, std::size_t size);
  bool same_waveformat_layout(const WAVEFORMATEXTENSIBLE &a, const WAVEFORMATEXTENSIBLE &b);
  std::vector<std::pair<WAVEFORMATEXTENSIBLE, WAVEFORMATEXTENSIBLE>> virtual_mic_format_pairs(const WAVEFORMATEXTENSIBLE &render, const WAVEFORMATEXTENSIBLE &capture);
  std::string pretty_waveformat(const WAVEFORMATEXTENSIBLE &waveformat);
}  // namespace platf::audio::tests

namespace {
  class fake_property_store_t final: public IPropertyStore {
  public:
    explicit fake_property_store_t(std::wstring friendly_name):
        friendly_name {std::move(friendly_name)} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE GetCount(DWORD *property_count) override {
      *property_count = friendly_name.empty() ? 0 : 1;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAt(DWORD, PROPERTYKEY *) override {
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY, PROPVARIANT *value) override {
      if (friendly_name.empty()) {
        return E_NOTIMPL;
      }

      const auto byte_count = (friendly_name.size() + 1) * sizeof(wchar_t);
      value->pwszVal = static_cast<wchar_t *>(CoTaskMemAlloc(byte_count));
      if (!value->pwszVal) {
        return E_OUTOFMEMORY;
      }

      std::memcpy(value->pwszVal, friendly_name.c_str(), byte_count);
      value->vt = VT_LPWSTR;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override {
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Commit() override {
      return E_NOTIMPL;
    }

    std::wstring friendly_name;
  };

  class fake_device_t final: public IMMDevice {
  public:
    fake_device_t(std::wstring id, std::wstring friendly_name):
        id {std::move(id)},
        properties {std::move(friendly_name)} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE Activate(REFIID, DWORD, PROPVARIANT *, void **) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore **property_store) override {
      properties.AddRef();
      *property_store = &properties;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetId(LPWSTR *device_id) override {
      const auto byte_count = (id.size() + 1) * sizeof(wchar_t);
      *device_id = static_cast<wchar_t *>(CoTaskMemAlloc(byte_count));
      if (!*device_id) {
        return E_OUTOFMEMORY;
      }

      std::memcpy(*device_id, id.c_str(), byte_count);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetState(DWORD *device_state) override {
      if (FAILED(state_status)) {
        return state_status;
      }

      *device_state = state;
      return S_OK;
    }

    std::wstring id;
    fake_property_store_t properties;
    HRESULT state_status = S_OK;
    DWORD state = DEVICE_STATE_ACTIVE;
  };

  class fake_device_collection_t final: public IMMDeviceCollection {
  public:
    explicit fake_device_collection_t(fake_device_t &device):
        device {device} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE GetCount(UINT *device_count) override {
      *device_count = 1;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice **item) override {
      if (index != 0) {
        return E_INVALIDARG;
      }

      device.AddRef();
      *item = &device;
      return S_OK;
    }

    fake_device_t &device;
  };

  class fake_device_enumerator_t final: public IMMDeviceEnumerator {
  public:
    explicit fake_device_enumerator_t(std::wstring id, std::wstring friendly_name = {}):
        device {std::move(id), std::move(friendly_name)},
        collection {device} {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void **object) override {  // NOSONAR(cpp:S5008): required by the Windows COM interface
      *object = nullptr;
      return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
      return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
      return 1;
    }

    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection **devices) override {
      if (FAILED(enumeration_status)) {
        return enumeration_status;
      }

      collection.AddRef();
      *devices = &collection;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow, ERole, IMMDevice **resolved_device) override {
      ++get_default_device_calls;
      device.AddRef();
      *resolved_device = &device;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR device_id, IMMDevice **resolved_device) override {
      ++get_device_calls;
      last_requested_id = device_id;
      if (FAILED(get_device_status)) {
        return get_device_status;
      }
      if (last_requested_id != device.id) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
      }

      device.AddRef();
      *resolved_device = &device;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient *) override {
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient *) override {
      return S_OK;
    }

    fake_device_t device;
    fake_device_collection_t collection;
    HRESULT enumeration_status = S_OK;
    HRESULT get_device_status = S_OK;
    int get_device_calls = 0;
    int get_default_device_calls = 0;
    std::wstring last_requested_id;
  };
}  // namespace

TEST(WindowsAudioTest, PreservesVirtualSpeakerFormatPreferenceAndChannelMasks) {
  constexpr DWORD stereo_mask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  constexpr DWORD surround_back_mask = stereo_mask | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
  constexpr DWORD surround_side_mask = stereo_mask | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
  constexpr DWORD surround71_mask = surround_back_mask | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
  const std::array stereo_samples {
    std::tuple {32, 24, WAVE_FORMAT_PCM},
    std::tuple {24, 24, WAVE_FORMAT_PCM},
    std::tuple {16, 16, WAVE_FORMAT_PCM},
    std::tuple {32, 32, WAVE_FORMAT_IEEE_FLOAT},
    std::tuple {32, 32, WAVE_FORMAT_PCM},
  };
  const std::array surround_samples {
    stereo_samples[3],
    stereo_samples[4],
    stereo_samples[0],
    stereo_samples[1],
    stereo_samples[2],
  };

  for (WORD channels : {2, 6, 8}) {
    const auto formats = platf::audio::tests::virtual_sink_waveformats(channels);
    const auto &samples = channels == 2 ? stereo_samples : surround_samples;
    ASSERT_EQ(formats.size(), channels == 6 ? 10 : 5);
    for (std::size_t i = 0; i < formats.size(); ++i) {
      const auto &[storage_bits, valid_bits, subformat] = samples[channels == 6 ? i / 2 : i];
      const auto &format = formats[i];
      EXPECT_EQ(format.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
      EXPECT_EQ(format.Format.nChannels, channels);
      EXPECT_EQ(format.Format.nSamplesPerSec, 48000);
      EXPECT_EQ(format.Format.wBitsPerSample, storage_bits);
      EXPECT_EQ(format.Samples.wValidBitsPerSample, valid_bits);
      EXPECT_EQ(format.SubFormat.Data1, subformat);
      EXPECT_EQ(format.Format.nBlockAlign, channels * storage_bits / 8);
      EXPECT_EQ(format.Format.nAvgBytesPerSec, 48000 * format.Format.nBlockAlign);
      EXPECT_EQ(format.dwChannelMask, channels == 2 ? stereo_mask : channels == 8 ? surround71_mask :
                                                                  i % 2 == 0      ? surround_back_mask :
                                                                                    surround_side_mask);
    }
  }
  EXPECT_TRUE(platf::audio::tests::virtual_sink_waveformats(4).empty());
}

TEST(WindowsAudioTest, AssignedSinkTakesPriorityOverConfiguredSink) {
  fake_device_enumerator_t enumerator {L"assigned-id"};

  EXPECT_FALSE(platf::audio::tests::microphone_available("assigned-id", "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"assigned-id");
}

TEST(WindowsAudioTest, ConfiguredSinkIsUsedWhenNoSinkWasAssigned) {
  fake_device_enumerator_t enumerator {L"configured-id"};

  EXPECT_FALSE(platf::audio::tests::microphone_available({}, "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"configured-id");
}

TEST(WindowsAudioTest, DefaultDeviceIsUsedWhenNoSinkWasRequested) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  platf::audio::tests::microphone_available({}, {}, &enumerator);
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

/**
 * @brief Preserve global audio configuration while exercising capture-only mode.
 */
class ExternalAudioTest: public testing::Test {
protected:
  void SetUp() override {
    previous = config::audio;
    config::audio.external_audio = true;
    config::audio.sink = "configured-id";
    config::audio.virtual_sink = "ignored-virtual-sink";
    policy_requests = 0;
    enumerator_requests = 0;
    enumerator_status = S_OK;
  }

  void TearDown() override {
    config::audio = previous;
  }

  /**
   * @brief Get the number of policy activation requests.
   * @return Requests observed by the test factory.
   */
  static int policy_request_count() {
    return policy_requests;
  }

  /**
   * @brief Get the number of endpoint enumeration activation requests.
   * @return Requests observed by the test factory.
   */
  static int enumerator_request_count() {
    return enumerator_requests;
  }

  /**
   * @brief Simulate failure to initialize endpoint enumeration.
   */
  static void fail_enumerator_initialization() {
    enumerator_status = E_FAIL;
  }

  /**
   * @brief Supply endpoint enumeration while simulating an unavailable policy interface.
   * @param class_id Requested COM class.
   * @param outer Unused aggregation pointer.
   * @param context Unused activation context.
   * @param interface_id Unused interface identifier.
   * @param object Receives the enumerator on success.
   * @return Simulated COM activation status.
   */
  static HRESULT WINAPI create_capture_interface(REFCLSID class_id, [[maybe_unused]] LPUNKNOWN outer, [[maybe_unused]] DWORD context, [[maybe_unused]] REFIID interface_id, LPVOID *object) {
    *object = nullptr;
    if (class_id != CLSID_MMDeviceEnumerator) {
      ++policy_requests;
      return REGDB_E_CLASSNOTREG;
    }
    ++enumerator_requests;
    if (FAILED(enumerator_status)) {
      return enumerator_status;
    }
    static fake_device_enumerator_t enumerator {L"configured-id"};
    enumerator.AddRef();
    *object = static_cast<IMMDeviceEnumerator *>(&enumerator);
    return S_OK;
  }

private:
  config::audio_t previous;  ///< Configuration restored after each test.
  inline static int policy_requests = 0;  ///< Requests for the unavailable policy interface.
  inline static int enumerator_requests = 0;  ///< Requests for endpoint enumeration.
  inline static HRESULT enumerator_status = S_OK;  ///< Simulated enumeration initialization result.
};

TEST_F(ExternalAudioTest, InitializesWithoutPolicyInterface) {
  EXPECT_EQ(platf::audio::tests::initialize_audio_control(&create_capture_interface), 0);
  EXPECT_EQ(policy_request_count(), 0);
  EXPECT_EQ(enumerator_request_count(), 1);
}

TEST_F(ExternalAudioTest, InitializationRequiresEndpointEnumerator) {
  fail_enumerator_initialization();
  EXPECT_NE(platf::audio::tests::initialize_audio_control(&create_capture_interface), 0);
  EXPECT_EQ(policy_request_count(), 0);
  EXPECT_EQ(enumerator_request_count(), 1);
}

TEST_F(ExternalAudioTest, DisabledModeStillRequiresPolicyInterface) {
  config::audio.external_audio = false;
  EXPECT_NE(platf::audio::tests::initialize_audio_control(&create_capture_interface), 0);
  EXPECT_EQ(policy_request_count(), 1);
  EXPECT_EQ(enumerator_request_count(), 0);
}

TEST_F(ExternalAudioTest, RequiresExplicitSink) {
  config::audio.sink.clear();
  EXPECT_EXIT(std::exit(platf::audio::tests::configured_sink_info() ? 1 : 0), testing::ExitedWithCode(0), "");
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available({}, {}, &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST_F(ExternalAudioTest, SkipsDefaultAndVirtualDeviceDiscovery) {
  EXPECT_EXIT(
    {
      const auto sinks = platf::audio::tests::configured_sink_info();
      std::exit(sinks && sinks->host == "configured-id" && !sinks->null ? 0 : 1);
    },
    testing::ExitedWithCode(0),
    ""
  );
}

TEST_F(ExternalAudioTest, SinkChangesDoNotAccessPolicyOrFormatInterfaces) {
  EXPECT_EXIT(std::exit(platf::audio::tests::set_external_sink("virtual-Stereoendpoint-id")), testing::ExitedWithCode(0), "");
  EXPECT_EXIT(std::exit(platf::audio::tests::set_external_sink("other-endpoint")), testing::ExitedWithCode(0), "");
}

TEST_F(ExternalAudioTest, ConfiguredSinkWinsOverAssignedSink) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available("virtual-Stereoother-id", "configured-id", &enumerator));
  EXPECT_EQ(enumerator.last_requested_id, L"configured-id");
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(ExternalAudioTest, MissingSinkDoesNotFallBackToDefault) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available({}, "missing-id", &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(ExternalAudioTest, DisconnectedSinkDoesNotFallBackToDefault) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  enumerator.device.state = DEVICE_STATE_UNPLUGGED;
  EXPECT_FALSE(platf::audio::tests::microphone_available({}, "configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(ExternalAudioTest, DisabledModePreservesAssignedSinkPriority) {
  config::audio.external_audio = false;
  fake_device_enumerator_t enumerator {L"assigned-id"};
  EXPECT_FALSE(platf::audio::tests::microphone_available("assigned-id", "configured-id", &enumerator));
  EXPECT_EQ(enumerator.last_requested_id, L"assigned-id");
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST_F(ExternalAudioTest, DisabledModePreservesDefaultFallback) {
  config::audio.external_audio = false;
  fake_device_enumerator_t enumerator {L"endpoint-id"};
  EXPECT_TRUE(platf::audio::tests::capture_follows_default_device(&enumerator, nullptr));
  EXPECT_EQ(enumerator.get_default_device_calls, 1);
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST_F(ExternalAudioTest, ReactivatedSinkCanBeResolvedAgain) {
  fake_device_enumerator_t enumerator {L"configured-id"};
  enumerator.device.state = DEVICE_STATE_UNPLUGGED;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("configured-id", &enumerator));
  enumerator.device.state = DEVICE_STATE_ACTIVE;
  EXPECT_TRUE(platf::audio::tests::sink_device_available("configured-id", &enumerator));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST(WindowsAudioTest, DefaultCaptureSelectsDefaultEndpoint) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_TRUE(platf::audio::tests::capture_follows_default_device(&enumerator, nullptr));
  EXPECT_EQ(enumerator.get_default_device_calls, 1);
}

TEST(WindowsAudioTest, PinnedCaptureKeepsExplicitEndpoint) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_FALSE(platf::audio::tests::capture_follows_default_device(&enumerator, &enumerator.device));
  EXPECT_EQ(enumerator.get_default_device_calls, 0);
}

TEST(WindowsAudioTest, MicrophoneRejectsUnresolvedSink) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_FALSE(platf::audio::tests::microphone_available("unknown", {}, &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 0);
}

TEST(WindowsAudioTest, ResolvesVirtualSinkDescriptorToActiveEndpoint) {
  fake_device_enumerator_t enumerator {L"endpoint-id"};

  EXPECT_TRUE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &enumerator));
  EXPECT_EQ(enumerator.get_device_calls, 1);
  EXPECT_EQ(enumerator.last_requested_id, L"endpoint-id");
}

TEST(WindowsAudioTest, ResolvesDeviceIdentifiersAndFriendlyNames) {
  fake_device_enumerator_t id_enumerator {L"endpoint-id"};
  EXPECT_TRUE(platf::audio::tests::sink_device_available("endpoint-id", &id_enumerator));
  EXPECT_EQ(id_enumerator.get_device_calls, 1);
  EXPECT_EQ(id_enumerator.last_requested_id, L"endpoint-id");

  fake_device_enumerator_t name_enumerator {L"endpoint-id", L"Friendly Endpoint"};
  EXPECT_TRUE(platf::audio::tests::sink_device_available("Friendly Endpoint", &name_enumerator));
  EXPECT_EQ(name_enumerator.get_device_calls, 1);
  EXPECT_EQ(name_enumerator.last_requested_id, L"endpoint-id");
}

TEST(WindowsAudioTest, RejectsUnknownOrUnenumerableSinks) {
  fake_device_enumerator_t unknown_enumerator {L"endpoint-id"};
  EXPECT_FALSE(platf::audio::tests::sink_device_available("unknown", &unknown_enumerator));
  EXPECT_EQ(unknown_enumerator.get_device_calls, 0);

  fake_device_enumerator_t failed_enumerator {L"endpoint-id"};
  failed_enumerator.enumeration_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("endpoint-id", &failed_enumerator));
  EXPECT_EQ(failed_enumerator.get_device_calls, 0);
}

TEST(WindowsAudioTest, RejectsUnavailableResolvedEndpoints) {
  fake_device_enumerator_t missing_enumerator {L"endpoint-id"};
  missing_enumerator.get_device_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &missing_enumerator));

  fake_device_enumerator_t state_failure_enumerator {L"endpoint-id"};
  state_failure_enumerator.device.state_status = E_FAIL;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &state_failure_enumerator));

  fake_device_enumerator_t inactive_enumerator {L"endpoint-id"};
  inactive_enumerator.device.state = DEVICE_STATE_DISABLED;
  EXPECT_FALSE(platf::audio::tests::sink_device_available("virtual-Stereoendpoint-id", &inactive_enumerator));
}

TEST(WindowsAudioTest, DefaultFollowingCaptureReinitializesAfterRenderEndpointChange) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(true, true, true, callback_count),
    platf::capture_e::reinit
  );
  EXPECT_EQ(callback_count, 1);
}

TEST(WindowsAudioTest, PinnedCaptureContinuesAfterRenderEndpointChange) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(false, true, true, callback_count),
    platf::capture_e::timeout
  );
  EXPECT_EQ(callback_count, 1);
}

TEST(WindowsAudioTest, CaptureEndpointChangeDoesNotTriggerRenderCallback) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(false, true, false, callback_count),
    platf::capture_e::timeout
  );
  EXPECT_EQ(callback_count, 0);
}

TEST(WindowsAudioTest, DefaultChangeWithoutCallbackStillReinitializes) {
  int callback_count = 0;

  EXPECT_EQ(
    platf::audio::tests::simulate_default_device_change(true, false, true, callback_count),
    platf::capture_e::reinit
  );
  EXPECT_EQ(callback_count, 0);
}

namespace {
  /**
   * @brief Build an endpoint format the way Windows reports a plain one.
   *
   * @param tag Format tag.
   * @param channels Channel count.
   * @param rate Sample rate in Hz.
   * @param bits Bits per sample.
   * @return Extensible endpoint format.
   */
  WAVEFORMATEXTENSIBLE endpoint_format(WORD tag, WORD channels, DWORD rate, WORD bits) {
    WAVEFORMATEX format {};
    format.wFormatTag = tag;
    format.nChannels = channels;
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = bits;
    format.nBlockAlign = channels * bits / 8;
    format.nAvgBytesPerSec = rate * format.nBlockAlign;
    return platf::audio::tests::endpoint_waveformat(&format, sizeof(format)).value();
  }
}  // namespace

TEST(WindowsAudioTest, ConvertsEndpointFormatsToExtensible) {
  const auto mono = endpoint_format(WAVE_FORMAT_IEEE_FLOAT, 1, 44100, 32);
  EXPECT_EQ(mono.Format.wFormatTag, WAVE_FORMAT_EXTENSIBLE);
  EXPECT_EQ(mono.Format.cbSize, sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
  EXPECT_EQ(mono.Samples.wValidBitsPerSample, 32);
  EXPECT_EQ(mono.SubFormat.Data1, WAVE_FORMAT_IEEE_FLOAT);
  EXPECT_EQ(mono.dwChannelMask, SPEAKER_FRONT_CENTER);

  const auto stereo = endpoint_format(WAVE_FORMAT_PCM, 2, 48000, 16);
  EXPECT_EQ(stereo.SubFormat.Data1, WAVE_FORMAT_PCM);
  EXPECT_EQ(stereo.dwChannelMask, static_cast<DWORD>(SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT));
  EXPECT_EQ(endpoint_format(WAVE_FORMAT_PCM, 6, 48000, 16).dwChannelMask, 0u);

  // Extensible data is copied unchanged
  auto copied = platf::audio::tests::endpoint_waveformat(&stereo.Format, sizeof(stereo));
  ASSERT_TRUE(copied.has_value());
  EXPECT_EQ(std::memcmp(&*copied, &stereo, sizeof(stereo)), 0);

  EXPECT_FALSE(platf::audio::tests::endpoint_waveformat(nullptr, sizeof(WAVEFORMATEX)).has_value());
  EXPECT_FALSE(platf::audio::tests::endpoint_waveformat(&mono.Format, sizeof(WAVEFORMATEX) - 1).has_value());
  EXPECT_FALSE(platf::audio::tests::endpoint_waveformat(&stereo.Format, sizeof(WAVEFORMATEX)).has_value());
}

TEST(WindowsAudioTest, ComparesEndpointSampleLayouts) {
  const auto base = endpoint_format(WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 32);
  EXPECT_TRUE(platf::audio::tests::same_waveformat_layout(base, base));
  EXPECT_FALSE(platf::audio::tests::same_waveformat_layout(base, endpoint_format(WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 32)));
  EXPECT_FALSE(platf::audio::tests::same_waveformat_layout(base, endpoint_format(WAVE_FORMAT_IEEE_FLOAT, 2, 44100, 32)));
  EXPECT_FALSE(platf::audio::tests::same_waveformat_layout(base, endpoint_format(WAVE_FORMAT_PCM, 2, 48000, 24)));
  EXPECT_FALSE(platf::audio::tests::same_waveformat_layout(base, endpoint_format(WAVE_FORMAT_PCM, 2, 48000, 32)));

  auto padded = endpoint_format(WAVE_FORMAT_PCM, 2, 48000, 32);
  padded.Samples.wValidBitsPerSample = 24;
  EXPECT_FALSE(platf::audio::tests::same_waveformat_layout(endpoint_format(WAVE_FORMAT_PCM, 2, 48000, 32), padded));
}

TEST(WindowsAudioTest, MatchesSteamMicrophoneFormatsAtFortyEightKilohertzMono) {
  // Steam Streaming Microphone's defaults: stereo 48 kHz playback, mono 44.1 kHz recording
  const auto render = endpoint_format(WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 32);
  const auto capture = endpoint_format(WAVE_FORMAT_IEEE_FLOAT, 1, 44100, 32);
  const auto pairs = platf::audio::tests::virtual_mic_format_pairs(render, capture);

  const std::array<std::tuple<WORD, DWORD, WORD, DWORD>, 6> expected {
    std::tuple {1, 48000, 1, 48000},
    std::tuple {1, 44100, 1, 44100},
    std::tuple {2, 48000, 2, 48000},
    std::tuple {2, 44100, 2, 44100},
    std::tuple {2, 48000, 1, 48000},
    std::tuple {2, 44100, 1, 44100},
  };
  ASSERT_EQ(pairs.size(), expected.size());
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    const auto &[render_format, capture_format] = pairs[i];
    const auto &[render_channels, render_rate, capture_channels, capture_rate] = expected[i];
    EXPECT_EQ(render_format.Format.nChannels, render_channels);
    EXPECT_EQ(render_format.Format.nSamplesPerSec, render_rate);
    EXPECT_EQ(capture_format.Format.nChannels, capture_channels);
    EXPECT_EQ(capture_format.Format.nSamplesPerSec, capture_rate);
    for (const auto &format : {render_format, capture_format}) {
      EXPECT_EQ(format.Format.nBlockAlign, format.Format.nChannels * 4);
      EXPECT_EQ(format.Format.nAvgBytesPerSec, format.Format.nSamplesPerSec * format.Format.nBlockAlign);
      EXPECT_EQ(format.SubFormat.Data1, WAVE_FORMAT_IEEE_FLOAT);
      EXPECT_EQ(format.dwChannelMask, format.Format.nChannels == 1 ? static_cast<DWORD>(SPEAKER_FRONT_CENTER) : static_cast<DWORD>(SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT));
    }
  }
}

TEST(WindowsAudioTest, MatchesVirtualMicrophoneSampleTypesFromEitherSide) {
  const auto render = endpoint_format(WAVE_FORMAT_PCM, 2, 48000, 24);
  const auto capture = endpoint_format(WAVE_FORMAT_PCM, 2, 44100, 16);
  const auto pairs = platf::audio::tests::virtual_mic_format_pairs(render, capture);

  ASSERT_FALSE(pairs.empty());
  EXPECT_TRUE(platf::audio::tests::same_waveformat_layout(pairs.front().first, pairs.front().second));
  EXPECT_EQ(pairs.front().first.Format.wBitsPerSample, 24);
  EXPECT_EQ(pairs.front().first.Format.nSamplesPerSec, 48000u);

  // The recording side's sample type is tried when the playback side's isn't accepted
  EXPECT_TRUE(std::ranges::any_of(pairs, [](const auto &pair) {
    return pair.first.Format.wBitsPerSample == 16 && platf::audio::tests::same_waveformat_layout(pair.first, pair.second);
  }));

  // Last resort: each side keeps its own sample type, and only the rate is matched
  const auto &[render_format, capture_format] = pairs[pairs.size() - 2];
  EXPECT_EQ(render_format.Format.wBitsPerSample, 24);
  EXPECT_EQ(capture_format.Format.wBitsPerSample, 16);
  EXPECT_EQ(render_format.Format.nSamplesPerSec, 48000u);
  EXPECT_EQ(capture_format.Format.nSamplesPerSec, 48000u);
}

TEST(WindowsAudioTest, KeepsSurroundLayoutsOutOfMatchedMicrophoneFormats) {
  auto render = endpoint_format(WAVE_FORMAT_IEEE_FLOAT, 8, 48000, 32);
  render.dwChannelMask = 0x63f;
  const auto capture = endpoint_format(WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 32);
  const auto pairs = platf::audio::tests::virtual_mic_format_pairs(render, capture);

  ASSERT_FALSE(pairs.empty());
  for (std::size_t i = 0; i + 1 < pairs.size(); ++i) {
    EXPECT_LE(pairs[i].first.Format.nChannels, 2);
  }

  // Matching only the rate keeps the playback side's own layout
  EXPECT_EQ(pairs.back().first.Format.nChannels, 8);
  EXPECT_EQ(pairs.back().first.dwChannelMask, 0x63fu);
  EXPECT_EQ(pairs.back().second.Format.nChannels, 2);
}

TEST(WindowsAudioTest, DescribesMonoFormats) {
  EXPECT_EQ(platf::audio::tests::pretty_waveformat(endpoint_format(WAVE_FORMAT_IEEE_FLOAT, 1, 48000, 32)), "F32 48000 1.0");
}
#endif
