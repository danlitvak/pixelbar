// Now-playing and transport controls through Windows' media session API (SMTC), which
// covers Spotify, browsers and most players. Runs on its own MTA thread.
#include "pixelbar.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#pragma comment(lib, "windowsapp.lib")

using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage::Streams;
namespace wf = winrt::Windows::Foundation;

namespace {

std::mutex g_mu;
std::condition_variable g_cv;
MediaState g_state;
std::deque<std::pair<MediaCmd, int64_t>> g_cmds;
HWND g_hwnd = nullptr;
int g_artSize = 32;

int64_t ToMs(wf::TimeSpan t) { return std::chrono::duration_cast<std::chrono::milliseconds>(t).count(); }

int64_t ToUnixMs(wf::DateTime t) {
  int64_t ticks = (int64_t)winrt::clock::to_file_time(t).value;
  return ticks <= 0 ? 0 : ticks / 10000 - 11644473600000LL;
}

std::string AppName(const std::wstring& id) {
  std::wstring l = id;
  for (auto& ch : l) ch = (wchar_t)towlower(ch);
  static const struct {
    const wchar_t* needle;
    const char* name;
  } kKnown[] = {
      {L"spotify", "Spotify"},       {L"msedge", "Edge"},          {L"chrome", "Chrome"},
      {L"firefox", "Firefox"},       {L"308046b0af4a39cb", "Firefox"}, {L"zunemusic", "Media Player"},
      {L"applemusic", "Apple Music"}, {L"tidal", "Tidal"},          {L"vlc", "VLC"},
      {L"foobar", "foobar2000"},     {L"brave", "Brave"},          {L"opera", "Opera"},
      {L"discord", "Discord"},       {L"deezer", "Deezer"},        {L"amazon", "Amazon Music"},
      {L"soundcloud", "SoundCloud"}, {L"youtube", "YouTube"},
  };
  for (auto& k : kKnown)
    if (l.find(k.needle) != std::wstring::npos) return k.name;
  size_t cut = id.find_last_of(L"\\/!");
  std::wstring t = cut == std::wstring::npos ? id : id.substr(cut + 1);
  if (t.size() > 4 && _wcsicmp(t.c_str() + t.size() - 4, L".exe") == 0) t.resize(t.size() - 4);
  return Narrow(t);
}

// Album art as 1-bit Atkinson dither: white pixels on the black desktop.
std::vector<uint8_t> Dither(const uint8_t* bgra, int n) {
  std::vector<float> lum((size_t)n * n);
  for (int i = 0; i < n * n; i++)
    lum[i] = 0.114f * bgra[i * 4] + 0.587f * bgra[i * 4 + 1] + 0.299f * bgra[i * 4 + 2];
  std::vector<float> sorted = lum;
  std::sort(sorted.begin(), sorted.end());
  float lo = sorted[sorted.size() * 3 / 100], hi = sorted[sorted.size() * 97 / 100];
  if (hi - lo < 24) {
    float mid = (hi + lo) / 2;
    lo = mid - 12;
    hi = mid + 12;
  }
  for (auto& v : lum) v = std::clamp((v - lo) / (hi - lo), 0.f, 1.f) * 255.f;
  std::vector<uint8_t> out((size_t)n * n);
  auto spread = [&](int x, int y, float e) {
    if (x >= 0 && x < n && y < n) lum[(size_t)y * n + x] += e;
  };
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++) {
      float old = lum[(size_t)y * n + x];
      float now = old >= 128 ? 255.f : 0.f;
      out[(size_t)y * n + x] = now > 0;
      float err = (old - now) / 8;
      spread(x + 1, y, err);
      spread(x + 2, y, err);
      spread(x - 1, y + 1, err);
      spread(x, y + 1, err);
      spread(x + 1, y + 1, err);
      spread(x, y + 2, err);
    }
  return out;
}

std::vector<uint8_t> FetchArt(const GlobalSystemMediaTransportControlsSessionMediaProperties& props) {
  auto ref = props.Thumbnail();
  if (!ref) return {};
  auto stream = ref.OpenReadAsync().get();
  auto dec = BitmapDecoder::CreateAsync(stream).get();
  uint32_t w = dec.PixelWidth(), h = dec.PixelHeight();
  if (!w || !h) return {};
  // Scale the short side to the art size and crop the centre square.
  double k = (double)g_artSize / std::min(w, h);
  uint32_t sw = std::max<uint32_t>(g_artSize, (uint32_t)std::lround(w * k));
  uint32_t sh = std::max<uint32_t>(g_artSize, (uint32_t)std::lround(h * k));
  BitmapTransform tf;
  tf.ScaledWidth(sw);
  tf.ScaledHeight(sh);
  tf.InterpolationMode(BitmapInterpolationMode::Fant);
  tf.Bounds(BitmapBounds{(sw - g_artSize) / 2, (sh - g_artSize) / 2, (uint32_t)g_artSize, (uint32_t)g_artSize});
  auto pd = dec.GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, tf,
                                  ExifOrientationMode::RespectExifOrientation, ColorManagementMode::DoNotColorManage)
                .get();
  auto px = pd.DetachPixelData();
  if (px.size() < (size_t)g_artSize * g_artSize * 4) return {};
  return Dither(px.data(), g_artSize);
}

bool SameForUi(const MediaState& a, const MediaState& b) {
  return a.present == b.present && a.playing == b.playing && a.title == b.title && a.artist == b.artist &&
         a.album == b.album && a.app == b.app && a.posMs == b.posMs && a.durMs == b.durMs && a.canPrev == b.canPrev &&
         a.canNext == b.canNext && a.canPlayPause == b.canPlayPause && a.canSeek == b.canSeek &&
         a.artVersion == b.artVersion;
}

void Run() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
  std::wstring trackKey;
  std::vector<uint8_t> art;
  uint64_t artVersion = 0;
  int artTries = 0;
  int64_t artNextAt = 0, lastPos = -1, lastPosAt = 0;

  for (;;) {
    std::pair<MediaCmd, int64_t> cmd{};
    bool haveCmd = false;
    {
      std::unique_lock lk(g_mu);
      g_cv.wait_for(lk, std::chrono::milliseconds(500), [] { return !g_cmds.empty(); });
      if (!g_cmds.empty()) {
        cmd = g_cmds.front();
        g_cmds.pop_front();
        haveCmd = true;
      }
    }
    try {
      if (!mgr) mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
      auto s = mgr.GetCurrentSession();
      MediaState st;
      if (s) {
        if (haveCmd) {
          switch (cmd.first) {
            case MediaCmd::PlayPause: s.TryTogglePlayPauseAsync().get(); break;
            case MediaCmd::Next: s.TrySkipNextAsync().get(); break;
            case MediaCmd::Prev: s.TrySkipPreviousAsync().get(); break;
            case MediaCmd::Seek: {
              auto start = s.GetTimelineProperties().StartTime();
              s.TryChangePlaybackPositionAsync(start.count() + cmd.second * 10000).get();
              break;
            }
          }
        }
        auto info = s.GetPlaybackInfo();
        auto ctl = info.Controls();
        st.present = true;
        st.playing = info.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        st.canPlayPause = ctl.IsPlayPauseToggleEnabled() || ctl.IsPlayEnabled() || ctl.IsPauseEnabled();
        st.canNext = ctl.IsNextEnabled();
        st.canPrev = ctl.IsPreviousEnabled();
        st.canSeek = ctl.IsPlaybackPositionEnabled();

        auto tl = s.GetTimelineProperties();
        st.durMs = std::max<int64_t>(0, ToMs(tl.EndTime() - tl.StartTime()));
        st.posMs = std::max<int64_t>(0, ToMs(tl.Position() - tl.StartTime()));
        int64_t now = NowMs();
        st.posStampMs = ToUnixMs(tl.LastUpdatedTime());
        // Some players never stamp their timeline; measure from when the position last moved.
        if (st.posStampMs < now - 12LL * 3600 * 1000 || st.posStampMs > now + 60000) {
          if (st.posMs != lastPos) {
            lastPos = st.posMs;
            lastPosAt = now;
          }
          st.posStampMs = lastPosAt;
        }

        auto props = s.TryGetMediaPropertiesAsync().get();
        st.title = winrt::to_string(props.Title());
        st.artist = winrt::to_string(props.Artist());
        if (st.artist.empty()) st.artist = winrt::to_string(props.AlbumArtist());
        st.album = winrt::to_string(props.AlbumTitle());
        std::wstring app(s.SourceAppUserModelId());
        st.app = AppName(app);

        std::wstring key = app + L"|" + std::wstring(props.Title()) + L"|" + std::wstring(props.Artist());
        if (key != trackKey) {
          trackKey = key;
          art.clear();
          artVersion++;
          artTries = 0;
          artNextAt = 0;
        }
        // Players often publish the cover a beat after the title, sometimes the old one first.
        if (artTries < 3 && now >= artNextAt) {
          artTries++;
          artNextAt = now + (artTries == 1 ? 1500 : 4000);
          std::vector<uint8_t> fresh;
          try {
            fresh = FetchArt(props);
          } catch (...) {
          }
          if (!fresh.empty() && fresh != art) {
            art = std::move(fresh);
            artVersion++;
          }
        }
        st.art = art;
        st.artSize = art.empty() ? 0 : g_artSize;
        st.artVersion = artVersion;
      } else {
        trackKey.clear();
        art.clear();
      }

      bool changed;
      {
        std::lock_guard lk(g_mu);
        changed = !SameForUi(st, g_state);
        g_state = std::move(st);
      }
      if (changed && g_hwnd) PostMessageW(g_hwnd, WM_PB_REFRESH, 0, 0);
    } catch (...) {
      mgr = nullptr;
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }
}

}  // namespace

void MediaStart(HWND notify, int artSize) {
  g_hwnd = notify;
  g_artSize = artSize;
  std::thread(Run).detach();
}

MediaState MediaSnapshot() {
  std::lock_guard lk(g_mu);
  return g_state;
}

void MediaCommand(MediaCmd c, int64_t arg) {
  {
    std::lock_guard lk(g_mu);
    g_cmds.emplace_back(c, arg);
  }
  g_cv.notify_one();
}
