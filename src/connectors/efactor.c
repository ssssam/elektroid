/*
 *   efactor.c
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

#include "efactor.h"
#include "common.h"

#define EFACTOR_MSG_TYPE_OBJECT 0x31
#define EFACTOR_MSG_TYPE_VALUE 0x3b

#define EFACTOR_KEY_LEN 4

#define EFACTOR_FACTOR_SW_LEN 17
#define EFACTOR_H9_SW_LEN 18

#define EFACTOR_FACTOR_NAME_PREFIX "Eventide Factor"
#define EFACTOR_H9_NAME_PREFIX "Eventide H9"

#define EFACTOR_PRESET_LINE_SEPARATOR "\x0d\x0a"

#define EFACTOR_OP_PRESETS_WANT 0x48
#define EFACTOR_OP_PROGRAM_WANT 0x4e
#define EFACTOR_OP_PRESETS_DUMP 0x49

#define EFACTOR_PRESET_DUMP_OFFSET 5

#define EFACTOR_SINGLE_PRESET_MIN_LEN (sizeof(EFACTOR_REQUEST_HEADER) + 5)	//The additional 5 bytes are the 2 square brackets and the preset number and the \0 and 0xf7 at the end.
#define EFACTOR_SINGLE_PRESET_MAX_LEN 256	//This is an empirical value. The maximum found value is 233 but we just add a few bytes just in case.
#define EFACTOR_MAX_ID_TAG_LEN 16	//The longest value is "[100]" plus the \0.

#define EFACTOR_TIMEOUT_TOTAL_PRESETS 20000	//This takes more than 10s for 100 presets.

#define EFACTOR_PEDAL_NAME(data) (data->type == EFACTOR_FACTOR ? EFACTOR_FACTOR_NAME_PREFIX : EFACTOR_H9_NAME_PREFIX)

static const guint8 EVENTIDE_ID[] = { 0x1c };
static const guint8 FAMILY_ID[] = { 0, 6 };	//This might not be the same value for all the Factor and H9 pedals.
static const guint8 MODEL_ID[] = { 0x11, 0 };	//This might not be the same value for all the Factor and H9 pedals.

//Identity Request Universal Sysex message. This is the same message used in the backend.
//We replicate this here because the compiler can't know the size of an external const array.
static const guint8 MIDI_IDENTITY_REQUEST[] =
  { 0xf0, 0x7e, 0x7f, 6, 1, 0xf7 };

static const guint8 EFACTOR_REQUEST_HEADER[] = { 0xf0, 0x1c, 0x70, 0 };

enum efactor_type
{
  EFACTOR_FACTOR,
  EFACTOR_H9
};

struct efactor_data
{
  guint id;
  guint presets;
  guint min;
  enum efactor_type type;
  //Readdir data is kept in memory so it can be used in other operations.
  //In the efactor case, the only way to get a single preset is by getting the panel, which needs a preset to be loaded
  // but won't work properly if there are preset mappings. So we read all the memory -we were reading it anyway- and
  // use it to get the download data from there.
  gchar **lines;
};

struct efactor_iter_data
{
  guint next;
  guint presets;
  struct efactor_data *backend_data;
};

enum efactor_fs
{
  FS_EFACTOR_PRESET = 1
};

static GByteArray *
efactor_new_op_msg (guint8 op)
{
  GByteArray *tx_msg =
    g_byte_array_sized_new (sizeof (EFACTOR_REQUEST_HEADER));
  g_byte_array_append (tx_msg, EFACTOR_REQUEST_HEADER,
		       sizeof (EFACTOR_REQUEST_HEADER) + 2);
  tx_msg->data[4] = op;
  tx_msg->data[5] = 0xf7;
  return tx_msg;
}

static GByteArray *
efactor_new_get_msg (guint8 type, const gchar * key)
{
  GByteArray *tx_msg =
    g_byte_array_sized_new (sizeof (EFACTOR_REQUEST_HEADER));
  g_byte_array_append (tx_msg, EFACTOR_REQUEST_HEADER,
		       sizeof (EFACTOR_REQUEST_HEADER) + 6);
  tx_msg->data[4] = type;
  memcpy (&tx_msg->data[5], key, EFACTOR_KEY_LEN);
  tx_msg->data[9] = 0xf7;
  return tx_msg;
}

static gchar *
efactor_get_download_path (struct backend *backend,
			   struct item_iterator *remote_iter,
			   const struct fs_operations *ops,
			   const gchar * dst_dir, const gchar * src_path)
{
  gchar *path, *name;
  struct efactor_data *data = backend->data;
  gchar *src_path_copy = strdup (src_path);
  gint id = atoi (basename (src_path_copy));
  g_free (src_path_copy);

  name = NULL;
  while (!next_item_iterator (remote_iter))
    {
      if (remote_iter->item.id == id)
	{
	  name = get_item_name (&remote_iter->item);
	  break;
	}
    }

  if (!name)
    {
      return NULL;
    }

  path = malloc (PATH_MAX);
  snprintf (path, PATH_MAX, "%s/%s %s.syx", dst_dir,
	    EFACTOR_PEDAL_NAME (data), name);

  return path;
}

static guint
efactor_next_dentry (struct item_iterator *iter)
{
  struct efactor_iter_data *data = iter->data;
  struct efactor_data *backend_data = data->backend_data;
  gchar *preset_name;

  if (data->next == data->presets)
    {
      return -ENOENT;
    }

  iter->item.id = data->next + backend_data->min;
  preset_name = data->backend_data->lines[data->next * 7 + 6];
  snprintf (iter->item.name, LABEL_MAX, "%s", preset_name);
  iter->item.type = ELEKTROID_FILE;
  iter->item.size = -1;
  data->next++;

  return 0;
}

static gint
efactor_read_dir (struct backend *backend, struct item_iterator *iter,
		  const gchar * path)
{
  GByteArray *tx_msg;
  GByteArray *rx_msg;
  struct efactor_iter_data *iter_data;
  struct efactor_data *data = backend->data;

  if (strcmp (path, "/"))
    {
      return -ENOTDIR;
    }

  if (data->lines)
    {
      //Reading from the device switches off and on the internal relays.
      //In case we call this function again just after calling it, we give the device some time to do it.
      sleep (1);
    }

  tx_msg = efactor_new_op_msg (EFACTOR_OP_PRESETS_WANT);
  rx_msg = backend_tx_and_rx_sysex (backend, tx_msg,
				    EFACTOR_TIMEOUT_TOTAL_PRESETS);
  if (!rx_msg)
    {
      return -ETIMEDOUT;
    }

  iter_data = g_malloc (sizeof (struct efactor_iter_data));
  iter_data->next = 0;
  iter_data->presets = data->presets;
  iter_data->backend_data = backend->data;
  if (iter_data->backend_data->lines)
    {
      g_strfreev (iter_data->backend_data->lines);
    }
  data->lines =
    g_strsplit ((gchar *) & rx_msg->data[EFACTOR_PRESET_DUMP_OFFSET],
		EFACTOR_PRESET_LINE_SEPARATOR, -1);
  free_msg (rx_msg);
  iter->data = iter_data;
  iter->next = efactor_next_dentry;
  iter->free = g_free;

  return 0;
}

static gint
efactor_download (struct backend *backend, const gchar * src_path,
		  GByteArray * output, struct job_control *control)
{
  gint err = 0, id;
  gchar *basename_copy;
  gboolean active;
  gchar **lines;
  struct efactor_data *data = backend->data;

  control->parts = 1;
  control->part = 0;
  set_job_control_progress (control, 0.0);

  if (!data->lines)
    {
      struct item_iterator iter;
      err = efactor_read_dir (backend, &iter, "/");
      if (err)
	{
	  return err;
	}
      free_item_iterator (&iter);
    }

  basename_copy = strdup (src_path);
  id = atoi (basename (basename_copy)) - data->min;	//Base 0
  g_free (basename_copy);

  g_byte_array_append (output, EFACTOR_REQUEST_HEADER,
		       sizeof (EFACTOR_REQUEST_HEADER));
  g_byte_array_append (output, (guint8 *) "\x49", 1);	// EFACTOR_OP_PRESETS_DUMP
  lines = &data->lines[id * 7];
  for (gint i = 0; i < 7; i++, lines++)
    {
      g_byte_array_append (output, (guint8 *) * lines, strlen (*lines));
      g_byte_array_append (output, (guint8 *) EFACTOR_PRESET_LINE_SEPARATOR,
			   strlen (EFACTOR_PRESET_LINE_SEPARATOR));
    }
  g_byte_array_append (output, (guint8 *) "\0\xf7", 2);

  g_mutex_lock (&control->mutex);
  active = control->active;
  g_mutex_unlock (&control->mutex);

  if (active)
    {
      set_job_control_progress (control, 1.0);
    }
  else
    {
      err = -ECANCELED;
    }

  return err;
}

static gint
efactor_upload (struct backend *backend, const gchar * path,
		GByteArray * input, struct job_control *control)
{
  gint err = 0, i, id;
  gchar *name, *b;
  gboolean active;
  GByteArray *tx_msg;
  gchar id_tag[EFACTOR_MAX_ID_TAG_LEN];
  struct efactor_data *data = backend->data;

  name = strdup (path);
  id = atoi (basename (name));	//This stops at the ':'.
  g_free (name);

  control->parts = 1;
  control->part = 0;
  set_job_control_progress (control, 0.0);

  //The fourth header byte is the device number ID so it might be different than 0.
  input->data[sizeof (EFACTOR_REQUEST_HEADER) - 1] = 0;
  if (input->len > EFACTOR_SINGLE_PRESET_MAX_LEN
      || input->len <= EFACTOR_SINGLE_PRESET_MIN_LEN
      || memcmp (input->data, EFACTOR_REQUEST_HEADER,
		 sizeof (EFACTOR_REQUEST_HEADER))
      || input->data[sizeof (EFACTOR_REQUEST_HEADER)] !=
      EFACTOR_OP_PRESETS_DUMP)
    {
      error_print ("Bad preset\n");
      err = -EBADMSG;
      goto end;
    }

  tx_msg = g_byte_array_sized_new (input->len + 2);	// With this we ensure there is enough space for all the digits of the preset number.
  g_byte_array_append (tx_msg, input->data,
		       sizeof (EFACTOR_REQUEST_HEADER) + 1);
  tx_msg->data[sizeof (EFACTOR_REQUEST_HEADER) - 1] = (guint8) data->id;
  snprintf (id_tag, EFACTOR_MAX_ID_TAG_LEN, "[%d]", (id % 100) + 1);	//1 based
  g_byte_array_append (tx_msg, (guint8 *) id_tag, strlen (id_tag));

  i = sizeof (EFACTOR_REQUEST_HEADER) + 1;
  for (b = (gchar *) & input->data[i]; i < input->len; i++, b++)
    {
      if (*b == ' ')
	{
	  break;
	}
    }

  if (i == input->len)
    {
      free_msg (tx_msg);
      err = -EBADMSG;
      goto end;
    }
  g_byte_array_append (tx_msg, (guint8 *) b, input->len - i);

  err = backend_tx (backend, tx_msg);

  g_mutex_lock (&control->mutex);
  active = control->active;
  g_mutex_unlock (&control->mutex);

  if (active)
    {
      set_job_control_progress (control, 1.0);
    }
  else
    {
      err = -ECANCELED;
    }

end:
  return err;
}

static gint
efactor_rename (struct backend *backend, const gchar * src, const gchar * dst)
{
  GByteArray *preset, *rx_msg;
  guint id;
  gint err;
  struct job_control control;
  gchar **lines, **line;
  debug_print (1, "Sending rename request...\n");
  err = common_slot_get_id_name_from_path (src, &id, NULL);
  if (err)
    {
      return err;
    }

  preset = g_byte_array_new ();
  efactor_download (backend, src, preset, &control);

  lines = g_strsplit ((gchar *) & preset->data[EFACTOR_PRESET_DUMP_OFFSET],
		      EFACTOR_PRESET_LINE_SEPARATOR, -1);
  g_free (lines[6]);
  lines[6] = NULL;

  line = lines;
  g_byte_array_set_size (preset, EFACTOR_PRESET_DUMP_OFFSET);
  while (*line)
    {
      g_byte_array_append (preset, (guint8 *) (*line), strlen (*line));
      g_byte_array_append (preset, (guint8 *) EFACTOR_PRESET_LINE_SEPARATOR,
			   strlen (EFACTOR_PRESET_LINE_SEPARATOR));
      line++;
    }
  g_byte_array_append (preset, (guint8 *) dst, strlen (dst));
  g_byte_array_append (preset, (guint8 *) EFACTOR_PRESET_LINE_SEPARATOR,
		       strlen (EFACTOR_PRESET_LINE_SEPARATOR));
  g_byte_array_append (preset, (guint8 *) "\0\xf7", 2);
  g_strfreev (lines);

  rx_msg = backend_tx_and_rx_sysex (backend, preset, 100);	//There must be no response.
  if (rx_msg)
    {
      err = -EIO;
      free_msg (rx_msg);
    }
  return err;
}

static gchar *
efactor_get_slot (struct item *item, struct backend *backend)
{
  gchar *slot = malloc (LABEL_MAX);
  struct efactor_data *data = backend->data;
  if (data->type == EFACTOR_FACTOR)
    {
      snprintf (slot, LABEL_MAX, "[%d:%d]", (item->id / 2) + 1,
		(item->id % 2) + 1);
    }
  else
    {
      snprintf (slot, LABEL_MAX, "%d", item->id + 1);
    }
  return slot;
}

static void
efactor_print (struct item_iterator *iter, struct backend *backend)
{
  gchar *slot = efactor_get_slot (&iter->item, backend);
  printf ("%c %s %s\n", iter->item.type, slot, iter->item.name);
  g_free (slot);
}

static const struct fs_operations FS_EFACTOR_OPERATIONS = {
  .fs = FS_EFACTOR_PRESET,
  .options =
    FS_OPTION_SINGLE_OP | FS_OPTION_SLOT_STORAGE | FS_OPTION_SORT_BY_ID,
  .name = "preset",
  .gui_name = "Presets",
  .gui_icon = BE_FILE_ICON_SND,
  .type_ext = "syx",
  .max_name_len = 16,
  .readdir = efactor_read_dir,
  .print_item = efactor_print,
  .rename = efactor_rename,
  .download = efactor_download,
  .upload = efactor_upload,
  .get_id = get_item_index,
  .get_slot = efactor_get_slot,
  .load = load_file,
  .save = save_file,
  .get_ext = backend_get_fs_ext,
  .get_upload_path = common_slot_get_upload_path,
  .get_download_path = efactor_get_download_path
};

static const struct fs_operations *FS_EFACTOR_OPERATIONS_LIST[] = {
  &FS_EFACTOR_OPERATIONS, NULL
};

void
efactor_destroy_data (struct backend *backend)
{
  struct efactor_data *data = backend->data;
  if (data->lines)
    {
      g_strfreev (data->lines);
    }
  backend_destroy_data (backend);
}

//We are replicanting the functionality in backend because the request is standard but the response is not.
gint
efactor_handshake (struct backend *backend)
{
  gint swlen, max, min, presets, id;
  enum efactor_type type;
  struct efactor_data *data;
  GByteArray *tx_msg;
  GByteArray *rx_msg;

  g_mutex_lock (&backend->mutex);
  backend_rx_drain (backend);
  g_mutex_unlock (&backend->mutex);

  tx_msg = g_byte_array_sized_new (sizeof (MIDI_IDENTITY_REQUEST));
  g_byte_array_append (tx_msg, (guchar *) MIDI_IDENTITY_REQUEST,
		       sizeof (MIDI_IDENTITY_REQUEST));
  rx_msg = backend_tx_and_rx_sysex (backend, tx_msg,
				    BE_SYSEX_TIMEOUT_GUESS_MS);
  if (!rx_msg)
    {
      return -ENODEV;
    }

  if (rx_msg->data[4] == 2)
    {
      if (rx_msg->len > 15)
	{
	  memset (backend->midi_info.company, 0, BE_COMPANY_LEN);
	  memcpy (backend->midi_info.company, &rx_msg->data[5], 1);
	  memcpy (backend->midi_info.family, &rx_msg->data[6], BE_FAMILY_LEN);
	  memcpy (backend->midi_info.model, &rx_msg->data[8], BE_MODEL_LEN);
	  memcpy (backend->midi_info.version, &rx_msg->data[10],
		  BE_VERSION_LEN);

	  snprintf (backend->device_name, LABEL_MAX,
		    "%02x-%02x-%02x %02x-%02x %02x-%02x %d.%d.%d.%d",
		    backend->midi_info.company[0],
		    backend->midi_info.company[1],
		    backend->midi_info.company[2],
		    backend->midi_info.family[0],
		    backend->midi_info.family[1],
		    backend->midi_info.model[0],
		    backend->midi_info.model[1],
		    backend->midi_info.version[0],
		    backend->midi_info.version[1],
		    backend->midi_info.version[2],
		    backend->midi_info.version[3]);
	  snprintf (backend->device_desc.name, LABEL_MAX,
		    backend->device_name);
	  debug_print (1, "XML version:\n%s\n", &rx_msg->data[14]);
	}
      else
	{
	  debug_print (1, "Illegal MIDI identity reply length\n");
	}
    }
  else
    {
      debug_print (1, "Illegal SUB-ID2\n");
    }

  free_msg (rx_msg);

  if (memcmp (backend->midi_info.company, EVENTIDE_ID, sizeof (EVENTIDE_ID))
      || memcmp (backend->midi_info.family, FAMILY_ID, sizeof (FAMILY_ID))
      || memcmp (backend->midi_info.model, MODEL_ID, sizeof (MODEL_ID)))
    {
      return -ENODEV;
    }

  tx_msg = efactor_new_get_msg (EFACTOR_MSG_TYPE_OBJECT, "0000");	//tj_version_key
  rx_msg = backend_tx_and_rx_sysex (backend, tx_msg, -1);
  id = rx_msg->data[sizeof (EFACTOR_REQUEST_HEADER) - 1];
  debug_print (1, "Version: %s\n", &rx_msg->data[7]);
  free_msg (rx_msg);

  tx_msg = efactor_new_get_msg (EFACTOR_MSG_TYPE_VALUE, "0001");	//tj_switch_key
  rx_msg = backend_tx_and_rx_sysex (backend, tx_msg, -1);
  debug_print (1, "Switches: %s\n", &rx_msg->data[7]);
  swlen = strlen ((gchar *) & rx_msg->data[7]) - 2;	//Remove single quotes
  free_msg (rx_msg);

  tx_msg = efactor_new_get_msg (EFACTOR_MSG_TYPE_OBJECT, "0206");	//sp_num_banks_lo
  rx_msg = backend_tx_and_rx_sysex (backend, tx_msg, -1);
  debug_print (1, "Minimum value: %s\n", &rx_msg->data[7]);
  min = atoi ((gchar *) & rx_msg->data[9]);
  free_msg (rx_msg);

  tx_msg = efactor_new_get_msg (EFACTOR_MSG_TYPE_OBJECT, "020A");	//sp_num_banks
  rx_msg = backend_tx_and_rx_sysex (backend, tx_msg, -1);
  debug_print (1, "Maximum value: %s\n", &rx_msg->data[7]);
  max = atoi ((gchar *) & rx_msg->data[9]);
  free_msg (rx_msg);

  if (swlen == EFACTOR_FACTOR_SW_LEN)
    {
      debug_print (1, "Factor pedal detected\n");
      min = 2 * min;
      max = 2 * (max + 1);
      type = EFACTOR_FACTOR;
    }
  else if (swlen == EFACTOR_H9_SW_LEN)
    {
      debug_print (1, "H9 pedal detected\n");
      type = EFACTOR_H9;
    }
  else
    {
      error_print ("Illegal switches number %d\n", swlen);
      return -ENODEV;
    }

  presets = max - min;
  debug_print (1, "Total presets: %d [%d, %d]\n", presets, min, max - 1);

  data = g_malloc (sizeof (struct efactor_data));
  data->id = id;
  data->presets = presets;
  data->min = min;
  data->type = type;
  data->lines = NULL;

  backend->device_desc.filesystems = FS_EFACTOR_PRESET;
  backend->fs_ops = FS_EFACTOR_OPERATIONS_LIST;
  backend->destroy_data = efactor_destroy_data;
  backend->data = data;

  snprintf (backend->device_name, LABEL_MAX, "%s %d.%d.%d.%d",
	    EFACTOR_PEDAL_NAME (data), backend->midi_info.version[0],
	    backend->midi_info.version[1], backend->midi_info.version[2],
	    backend->midi_info.version[3]);

  return 0;
}
