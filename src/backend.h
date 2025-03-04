/*
 *   backend.h
 *   Copyright (C) 2022 David García Goñi <dagargo@gmail.com>
 *
 *   This file is part of Elektroid.
 *
 *   Elektroid is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Elektroid is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Elektroid. If not, see <http://www.gnu.org/licenses/>.
 */

#include <alsa/asoundlib.h>
#include "utils.h"

#ifndef BACKEND_H
#define BACKEND_H

#define BE_FILE_ICON_WAVE "elektroid-wave-symbolic"
#define BE_FILE_ICON_SEQ "elektroid-sequence-symbolic"
#define BE_FILE_ICON_PRJ "elektroid-project-symbolic"
#define BE_FILE_ICON_SND "elektroid-sound-symbolic"

#define BE_DUMP_TIMEOUT 5000	//With and E-Mu ESI-2000 it takes this more than 3 seconds to receive to receive some packets after the process has started.
#define BE_REST_TIME_US 50000
#define BE_SYSEX_TIMEOUT_MS 5000
#define BE_SYSEX_TIMEOUT_GUESS_MS 500	//When the request is not implemented, 5 s is too much.
#define BE_SAMPLE_ID_NAME_SEPARATOR ":"

#define BE_COMPANY_LEN 3
#define BE_FAMILY_LEN 2
#define BE_MODEL_LEN 2
#define BE_VERSION_LEN 4

#define BE_SYSTEM_ID "SYSTEM_ID"

struct backend_storage_stats
{
  const gchar *name;
  guint64 bsize;
  guint64 bfree;
};

typedef void (*t_destroy_data) (struct backend *);

typedef gint (*t_get_storage_stats) (struct backend *, gint,
				     struct backend_storage_stats *);

struct backend_midi_info
{
  gchar company[BE_COMPANY_LEN];
  gchar family[BE_FAMILY_LEN];
  gchar model[BE_MODEL_LEN];
  gchar version[BE_VERSION_LEN];
};

enum backend_type
{
  BE_TYPE_NONE,
  BE_TYPE_SYSTEM,
  BE_TYPE_MIDI
};

struct backend
{
  enum backend_type type;
  struct device_desc device_desc;
  struct backend_midi_info midi_info;
  snd_rawmidi_t *inputp;
  snd_rawmidi_t *outputp;
  GMutex mutex;
  //Internal buffer
  guint8 *buffer;
  ssize_t rx_len;
  //Linux
  gint npfds;
  struct pollfd *pfds;
  gchar device_name[LABEL_MAX];
  //Message cache
  GHashTable *cache;
  //These must be filled by the concrete backend.
  const struct fs_operations **fs_ops;
  t_destroy_data destroy_data;
  t_sysex_transfer upgrade_os;
  t_get_storage_stats get_storage_stats;
  void *data;
};

struct backend_system_device
{
  gchar name[LABEL_MAX];
  gchar id[LABEL_MAX];
};

gint backend_init (struct backend *, const gchar *);

void backend_destroy (struct backend *);

ssize_t backend_tx_raw (struct backend *, const guint8 *, guint);

gint backend_tx_sysex_no_update (struct backend *, struct sysex_transfer *);

gint backend_tx_sysex (struct backend *, struct sysex_transfer *);

gint backend_rx_sysex (struct backend *, struct sysex_transfer *);

gint backend_tx (struct backend *, GByteArray *);

gint backend_tx_and_rx_sysex_transfer (struct backend *,
				       struct sysex_transfer *, gboolean);

GByteArray *backend_tx_and_rx_sysex (struct backend *, GByteArray *, gint);

void backend_rx_drain (struct backend *);

gboolean backend_check (struct backend *);

void backend_enable_cache (struct backend *);

void backend_disable_cache (struct backend *);

GArray *backend_get_system_devices ();

const struct fs_operations *backend_get_fs_operations (struct backend *, gint,
						       const char *);

const gchar *backend_get_fs_name (struct backend *, guint);

gchar *backend_get_fs_ext (const struct device_desc *,
			   const struct fs_operations *);

gdouble backend_get_storage_stats_percent (struct backend_storage_stats *);

void backend_destroy_data (struct backend *);

gint backend_program_change (struct backend *, guint8, guint8);

#endif
