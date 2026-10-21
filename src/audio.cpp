// Master volume, mute and the output peak meter of the default playback device.
#include "pixelbar.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <algorithm>

#pragma comment(lib, "ole32.lib")

namespace {

IMMDeviceEnumerator* g_enum = nullptr;
IAudioEndpointVolume* g_vol = nullptr;
IAudioMeterInformation* g_meter = nullptr;
std::wstring g_deviceId;
int64_t g_checkedAt = 0;

template <class T>
void Drop(T*& p) {
  if (p) {
    p->Release();
    p = nullptr;
  }
}

// Re-reads the default device so plugging in headphones moves the controls with it.
void Acquire() {
  if (!g_enum && FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                         __uuidof(IMMDeviceEnumerator), (void**)&g_enum)))
    return;
  IMMDevice* dev = nullptr;
  if (FAILED(g_enum->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev)) || !dev) {
    Drop(g_vol);
    Drop(g_meter);
    g_deviceId.clear();
    return;
  }
  LPWSTR id = nullptr;
  dev->GetId(&id);
  std::wstring sid = id ? id : L"";
  if (id) CoTaskMemFree(id);
  if (sid != g_deviceId || !g_vol) {
    Drop(g_vol);
    Drop(g_meter);
    dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&g_vol);
    dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&g_meter);
    g_deviceId = sid;
  }
  dev->Release();
}

}  // namespace

void AudioPoll(AudioState& st) {
  int64_t now = NowMs();
  if (!g_vol || now - g_checkedAt > 2000) {
    g_checkedAt = now;
    Acquire();
  }
  st.ok = false;
  if (!g_vol) return;
  float v = 0;
  BOOL m = FALSE;
  if (FAILED(g_vol->GetMasterVolumeLevelScalar(&v)) || FAILED(g_vol->GetMute(&m))) {
    Drop(g_vol);
    Drop(g_meter);
    g_deviceId.clear();
    return;
  }
  float peak = 0;
  if (g_meter) g_meter->GetPeakValue(&peak);
  st.ok = true;
  st.volume = v;
  st.muted = m != FALSE;
  st.peak = peak;
}

void AudioSetVolume(float v) {
  if (!g_vol) return;
  g_vol->SetMute(FALSE, nullptr);
  g_vol->SetMasterVolumeLevelScalar(std::clamp(v, 0.f, 1.f), nullptr);
}

void AudioToggleMute() {
  if (!g_vol) return;
  BOOL m = FALSE;
  g_vol->GetMute(&m);
  g_vol->SetMute(!m, nullptr);
}
