/*
OBS Audio Video Sync Dock
Copyright (C) 2023 Norihiro Kamae <norihiro@nagater.net>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include "quirc.h"
#include "obs-websocket-api.h"

#include "plugin-macros.generated.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define CONFIG_SECTION_NAME "AudioVideoSyncDock"

static obs_websocket_vendor vendor = NULL;
static void *g_dock_instance = NULL; // SyncTestDock pointer

/* Global state for WebSocket API access */
struct sync_state_t {
	pthread_mutex_t mutex;
	double latency_ms;
	int video_index;
	int audio_index;
	double frequency;
	bool is_measuring;
	bool has_data;
};

static struct sync_state_t g_sync_state = {
	.latency_ms = 0.0,
	.video_index = -1,
	.audio_index = -1,
	.frequency = 0.0,
	.is_measuring = false,
	.has_data = false,
};

void *create_sync_test_dock();
void register_sync_test_output();
void register_sync_test_monitor(bool list);

/* C-callable wrappers for C++ dock methods */
extern void sync_dock_start_measurement(void *dock);
extern void sync_dock_stop_measurement(void *dock);
extern bool sync_dock_is_measuring(void *dock);

void sync_set_dock_instance(void *dock)
{
	g_dock_instance = dock;
}

/* Functions for updating global state from the dock */
void sync_state_update_latency(double latency_ms, int index)
{
	pthread_mutex_lock(&g_sync_state.mutex);
	g_sync_state.latency_ms = latency_ms;
	g_sync_state.has_data = true;
	pthread_mutex_unlock(&g_sync_state.mutex);
}

void sync_state_update_video(int index, double frequency)
{
	pthread_mutex_lock(&g_sync_state.mutex);
	g_sync_state.video_index = index;
	g_sync_state.frequency = frequency;
	pthread_mutex_unlock(&g_sync_state.mutex);
}

void sync_state_update_audio(int index)
{
	pthread_mutex_lock(&g_sync_state.mutex);
	g_sync_state.audio_index = index;
	pthread_mutex_unlock(&g_sync_state.mutex);
}

void sync_state_set_measuring(bool measuring)
{
	pthread_mutex_lock(&g_sync_state.mutex);
	g_sync_state.is_measuring = measuring;
	if (!measuring) {
		g_sync_state.has_data = false;
		g_sync_state.latency_ms = 0.0;
		g_sync_state.video_index = -1;
		g_sync_state.audio_index = -1;
		g_sync_state.frequency = 0.0;
	}
	pthread_mutex_unlock(&g_sync_state.mutex);
}

#if LIBOBS_API_VER <= MAKE_SEMANTIC_VERSION(29, 1, 3)
bool obs_frontend_add_dock_by_id_compat(const char *id, const char *title, void *widget);
#define obs_frontend_add_dock_by_id obs_frontend_add_dock_by_id_compat
#endif

/* WebSocket API request handlers */
static void get_sync_state_cb(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	pthread_mutex_lock(&g_sync_state.mutex);
	obs_data_set_double(response_data, "latency_ms", g_sync_state.latency_ms);
	obs_data_set_int(response_data, "video_index", g_sync_state.video_index);
	obs_data_set_int(response_data, "audio_index", g_sync_state.audio_index);
	obs_data_set_double(response_data, "frequency", g_sync_state.frequency);
	obs_data_set_bool(response_data, "is_measuring", g_sync_state.is_measuring);
	obs_data_set_bool(response_data, "has_data", g_sync_state.has_data);
	pthread_mutex_unlock(&g_sync_state.mutex);
}

static void start_measurement_cb(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	if (!g_dock_instance) {
		blog(LOG_ERROR, "[audio-video-sync-dock] Cannot start: dock not initialized");
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Dock not initialized");
		return;
	}

	if (sync_dock_is_measuring(g_dock_instance)) {
		blog(LOG_WARNING, "[audio-video-sync-dock] Measurement already in progress");
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Already measuring");
		return;
	}

	sync_dock_start_measurement(g_dock_instance);
	obs_data_set_bool(response_data, "success", true);
	blog(LOG_INFO, "[audio-video-sync-dock] Measurement started via WebSocket");
}

static void stop_measurement_cb(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	if (!g_dock_instance) {
		blog(LOG_ERROR, "[audio-video-sync-dock] Cannot stop: dock not initialized");
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Dock not initialized");
		return;
	}

	if (!sync_dock_is_measuring(g_dock_instance)) {
		blog(LOG_WARNING, "[audio-video-sync-dock] No measurement in progress");
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Not measuring");
		return;
	}

	sync_dock_stop_measurement(g_dock_instance);
	obs_data_set_bool(response_data, "success", true);
	blog(LOG_INFO, "[audio-video-sync-dock] Measurement stopped via WebSocket");
}

const char *obs_module_name(void)
{
	return obs_module_text("Module.Name");
}

bool obs_module_load(void)
{
#if LIBOBS_API_VER < MAKE_SEMANTIC_VERSION(31, 0, 0)
	config_t *cfg = obs_frontend_get_global_config();
#else
	config_t *cfg = obs_frontend_get_app_config();
#endif
	bool list_source = cfg && config_get_bool(cfg, CONFIG_SECTION_NAME, "ListMonitor");

	// Initialize global state mutex
	pthread_mutex_init(&g_sync_state.mutex, NULL);

	register_sync_test_output();
	register_sync_test_monitor(list_source);
	blog(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	blog(LOG_INFO, "quirc (version %s)", quirc_version());
	return true;
}

void obs_module_unload(void)
{
	pthread_mutex_destroy(&g_sync_state.mutex);
}

void obs_module_post_load(void)
{
	// Register WebSocket vendor
	vendor = obs_websocket_register_vendor("audio_video_sync_dock");
	if (!vendor) {
		blog(LOG_WARNING, "[audio-video-sync-dock] Failed to register WebSocket vendor (obs-websocket not available)");
	} else {
		// Register request handlers
		if (!obs_websocket_vendor_register_request(vendor, "get_sync_state", get_sync_state_cb, NULL))
			blog(LOG_WARNING, "[audio-video-sync-dock] Failed to register get_sync_state request");

		if (!obs_websocket_vendor_register_request(vendor, "start_measurement", start_measurement_cb, NULL))
			blog(LOG_WARNING, "[audio-video-sync-dock] Failed to register start_measurement request");

		if (!obs_websocket_vendor_register_request(vendor, "stop_measurement", stop_measurement_cb, NULL))
			blog(LOG_WARNING, "[audio-video-sync-dock] Failed to register stop_measurement request");

		blog(LOG_INFO, "[audio-video-sync-dock] WebSocket vendor API registered successfully");
	}

	// Create and register the dock
	void *dock = create_sync_test_dock();
	sync_set_dock_instance(dock);
	obs_frontend_add_dock_by_id(ID_PREFIX ".main", obs_module_text("SyncTestDock.Title"), dock);
}
