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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Functions for managing dock instance (called from plugin-main) */
void sync_set_dock_instance(void *dock);

/* Functions for updating global sync state (called from the dock) */
void sync_state_update_latency(double latency_ms, int index);
void sync_state_update_video(int index, double frequency);
void sync_state_update_audio(int index);
void sync_state_set_measuring(bool measuring);

#ifdef __cplusplus
}
#endif
