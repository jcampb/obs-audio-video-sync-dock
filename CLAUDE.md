# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an OBS Studio plugin that measures audio-video latency by detecting QR code patterns in video frames and audio markers in audio streams. The plugin creates a dock in OBS Studio that displays synchronization measurements.

## Build Commands

### Linux
```bash
# Configure with CMake
cmake -S . -B build \
  -D CMAKE_BUILD_TYPE=RelWithDebInfo \
  -D CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON

# Build
cd build
make -j4

# Create package
make package
```

### macOS
```bash
# Configure with CMake
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$PWD/release/" \
  -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"

# Build
cmake --build build --config RelWithDebInfo

# Install
cmake --install build --config RelWithDebInfo --prefix=release
```

### Windows
```powershell
# Configure with CMake
cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_SYSTEM_VERSION=10.0.18363.657

# Build
cmake --build build --config RelWithDebInfo -j 4

# Install
cmake --install build --config RelWithDebInfo --prefix release

# Package
ci/windows/package-windows.cmd <obs-version>
```

## Architecture

### Plugin Entry Point
- `src/plugin-main.c` - Module initialization and registration
  - `obs_module_load()`: Registers the output and monitor sources
  - `obs_module_post_load()`: Creates and adds the dock to OBS UI (must happen post-load to access frontend API)

### Core Components

**Sync Test Output** (`src/sync-test-output.cpp`, `src/sync-test-output.hpp`)
- Implements the `sync-test-output` OBS output type that captures both video and audio streams
- **Video Processing**:
  - Uses quirc library (in `deps/quirc/`) to decode QR codes from video frames
  - Downsamples video frames for QR detection (up to 640x480)
  - Tracks circular patterns at QR code corners to detect timing markers
  - Supports multiple pixel formats (I420, NV12, I444, I422, I010, P010, P216, P416, RGBA, BGRA, BGRX)
- **Audio Processing**:
  - Demodulates audio signal using complex oscillator to detect markers
  - Uses preamble pattern (0xF0) for synchronization
  - Implements CRC4 check for data integrity
  - Maintains circular buffer with cumulative sums for efficient windowed operations
- **Sync Detection**:
  - Maintains list of video/audio index pairs to match timestamps
  - Emits signals when sync patterns are found: `video_marker_found`, `audio_marker_found`, `sync_found`

**Sync Test Dock** (`src/sync-test-dock.cpp`, `src/sync-test-dock.hpp`)
- Qt-based UI widget that displays latency measurements
- Connects to signals from sync-test-output
- Shows latency, frequency, video/audio indices, and missed markers percentage
- Positive latency = audio lagged, negative = video lagged

**Sync Test Monitor** (`src/sync-test-monitor.c`)
- Optional OBS source that renders QR code corner positions as green rectangles
- Can be disabled in config (not listed in sources) via `ListMonitor` config option
- Automatically finds and connects to active sync-test-output

**Dock Compatibility** (`src/dock-compat.cpp`, `src/dock-compat.hpp`)
- Provides `obs_frontend_add_dock_by_id_compat()` for OBS versions ≤ 29.1.3
- Handles API version differences in dock registration

### Key Data Structures

**QR Data Decoding** (`src/sync-test-output.hpp`)
- `st_qr_data`: Parses QR code payload containing index, frequency (f), cycle count (c), and timing (q_ms)

**Peak Finding** (`src/peak-finder.hpp`)
- `peak_finder`: Detects peaks in audio correlation for marker timing

### Threading and Synchronization
- Video and audio callbacks run on OBS render threads
- Mutex (`std::mutex`) protects shared state between video/audio processing threads
- Signal handlers use Qt's meta-object system for thread-safe UI updates

## Dependencies

- **OBS Studio**: libobs, obs-frontend-api
- **Qt**: Core, Widgets, Gui (version determined by OBS build)
- **quirc**: QR code detection library (bundled in `deps/quirc/`)

## Platform-Specific Notes

### Linux
- Requires specific compiler warnings disabled for quirc library (`-Wno-unused-parameter`, `-Wno-sign-compare`)
- Uses `-Wl,-z,defs` and `-Wl,--unresolved-symbols=report-all` for strict linking

### macOS
- Requires `-stdlib=libc++` and `-fvisibility=default`
- Plugin naming: no `lib` prefix
- Supports universal binaries (x86_64 + arm64)

### Windows
- Requires `/MP` (multicore builds) and `/d2FH4-` flags
- Defines `_USE_MATH_DEFINES` for M_PI
- Links with `w32-pthreads`

## Configuration

- Config section: `AudioVideoSyncDock` in OBS app config
- `ListMonitor`: Boolean to control whether monitor source appears in source list

## Testing Pattern Files

The plugin works with specific sync pattern video files (24, 23.98, 25, 50, 59.94, 60 FPS) that encode timing information in QR codes and audio. Users point a camera at a display playing these patterns to measure capture latency.

---

## WebSocket API Implementation Plan

### Goal

Expose the plugin's latency measurement data via the obs-websocket Vendor API for:
- Reading current latency measurements programmatically
- Starting/stopping measurements remotely
- Real-time latency update events
- Automated sync correction workflows

### Key Implementation Steps

#### 1. Vendor Registration in `src/plugin-main.c`

Add to `obs_module_post_load()`:
```c
#include "obs-websocket-api.h"

static obs_websocket_vendor vendor = NULL;

void obs_module_post_load(void)
{
    vendor = obs_websocket_register_vendor("audio_video_sync_dock");
    if (!vendor) {
        blog(LOG_WARNING, "[audio-video-sync-dock] Failed to register WebSocket vendor");
        return;
    }

    obs_websocket_vendor_register_request(vendor, "get_sync_state", get_sync_state_cb, NULL);
    obs_websocket_vendor_register_request(vendor, "start_measurement", start_measurement_cb, NULL);
    obs_websocket_vendor_register_request(vendor, "stop_measurement", stop_measurement_cb, NULL);

    // Add dock registration here...
}
```

#### 2. Download Required Header

Download `obs-websocket-api.h` from:
https://github.com/obsproject/obs-websocket/blob/master/lib/obs-websocket-api.h

Place in `src/` directory.

#### 3. State Access Pattern

The `sync_test_output` struct (in `src/sync-test-output.cpp`) contains the measurement state. Key challenge: accessing this from C callbacks in `plugin-main.c`.

**Suggested approach:**
- Store a weak reference to the output in a global variable when dock creates it
- Or: Iterate through outputs to find one with `OUTPUT_ID` type
- Protect access with the existing `std::mutex` in the struct

**State to expose:**
- Current latency (calculated in `on_sync_found()` in `sync-test-dock.cpp`)
- Video/audio indices (from `video_marker_found_s` and `audio_marker_found_s` structs)
- Frequency (from `qr_data.f`)
- Measurement running state (check if `sync_test` output exists and is started)

#### 4. Request Handler Implementation

**`get_sync_state`:**
```c
void get_sync_state_cb(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
    UNUSED_PARAMETER(request_data);
    UNUSED_PARAMETER(priv_data);

    // Find the sync-test-output and access its state
    // Note: May need to add getter functions in sync-test-output.cpp

    obs_data_set_double(response_data, "latency_ms", /* latency value */);
    obs_data_set_int(response_data, "video_index", /* video index */);
    obs_data_set_int(response_data, "audio_index", /* audio index */);
    obs_data_set_double(response_data, "frequency", /* frequency */);
    obs_data_set_bool(response_data, "is_measuring", /* running state */);
}
```

**`start_measurement` / `stop_measurement`:**
- Need to trigger the same logic as the Start/Stop button in `SyncTestDock::on_start_stop()`
- Consider refactoring that logic into a shared function callable from both UI and WebSocket

#### 5. Optional: Real-Time Events

Add event emission in `SyncTestDock::on_sync_found()`:
```cpp
extern obs_websocket_vendor vendor; // Defined in plugin-main.c

void SyncTestDock::on_sync_found(uint64_t video_ts, uint64_t audio_ts, int index)
{
    // ... existing code ...

    if (vendor) {
        obs_data_t *event_data = obs_data_create();
        obs_data_set_double(event_data, "latency_ms", ts * 1e-6);
        obs_data_set_int(event_data, "index", index);
        obs_websocket_vendor_emit_event(vendor, "latency_updated", event_data);
        obs_data_release(event_data);
    }
}
```

### Proposed API Specification

#### Requests (via CallVendorRequest)

**Vendor Name:** `audio_video_sync_dock`

| Request Type | Request Fields | Response Fields |
|--------------|----------------|-----------------|
| `get_sync_state` | none | `latency_ms` (double), `video_index` (int), `audio_index` (int), `frequency` (double), `is_measuring` (bool) |
| `start_measurement` | none | `success` (bool) |
| `stop_measurement` | none | `success` (bool) |

#### Events (via VendorEvent subscription)

| Event Type | Data Fields |
|------------|-------------|
| `latency_updated` | `latency_ms` (double), `index` (int), `frequency` (double) |

### Python Client Example

```python
import obsws_python as obs

client = obs.ReqClient(host='localhost', port=4455, password='your_password')

# Get current sync state
response = client.call_vendor_request(
    vendor_name="audio_video_sync_dock",
    request_type="get_sync_state"
)

latency = response.response_data["latency_ms"]
print(f"Current A/V latency: {latency}ms")

# Auto-apply correction to audio source
if abs(latency) > 10:
    offset_ns = int(latency * 1_000_000)  # Convert ms to nanoseconds
    client.set_input_audio_sync_offset("Mic/Aux", offset_ns)
    print(f"Applied {latency}ms offset correction")
```

### Local Development Build (macOS Apple Silicon)

```bash
# Clone and navigate
cd ~/code
git clone --recursive https://github.com/jcampb/obs-audio-video-sync-dock.git
cd obs-audio-video-sync-dock

# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)

# Install to OBS plugins directory
cp -r obs-audio-video-sync-dock.plugin ~/Library/Application\ Support/obs-studio/plugins/

# Clear macOS quarantine if needed
xattr -cr ~/Library/Application\ Support/obs-studio/plugins/obs-audio-video-sync-dock.plugin
```

### Testing WebSocket API

1. Start OBS Studio with the plugin installed
2. Enable WebSocket server: Tools → obs-websocket Settings → Enable WebSocket server
3. Test with Python:
```python
import obsws_python as obs
client = obs.ReqClient(host='localhost', port=4455, password='your_password')
result = client.call_vendor_request("audio_video_sync_dock", "get_sync_state")
print(result.response_data)
```

### Implementation References

- **obs-websocket Vendor API Example**: https://github.com/obsproject/obs-websocket/blob/master/lib/example/simplest-plugin.c
- **obs-loudness-dock**: https://github.com/norihiro/obs-loudness-dock (by same author, has WebSocket integration)
- **obs-websocket-api.h**: https://github.com/obsproject/obs-websocket/blob/master/lib/obs-websocket-api.h

### Implementation Checklist

- [ ] Download and integrate `obs-websocket-api.h`
- [ ] Register vendor in `obs_module_post_load()`
- [ ] Implement state access pattern (global weak ref or output enumeration)
- [ ] Add getter functions to `sync-test-output.cpp` if needed for state access
- [ ] Implement `get_sync_state` request handler
- [ ] Implement `start_measurement` request handler
- [ ] Implement `stop_measurement` request handler
- [ ] Test all request handlers with Python client
- [ ] (Optional) Add `latency_updated` event emission
- [ ] (Optional) Subscribe to events in Python client
- [ ] Verify no memory leaks or race conditions
- [ ] Document API in README
