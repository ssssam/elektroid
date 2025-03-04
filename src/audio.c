/*
 *   audio.c
 *   Copyright (C) 2019 David García Goñi <dagargo@gmail.com>
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

#include "../config.h"
#include "audio.h"

#define AUDIO_PA_BUFFER_LEN (AUDIO_SAMPLE_RATE / 10)
#define AUDIO_CHANNELS 2

static const pa_buffer_attr buffer_attributes = {
  .maxlength = -1,
  .tlength = AUDIO_PA_BUFFER_LEN * 2,
  .prebuf = 0,
  .minreq = -1
};

static const pa_sample_spec sample_spec = {
  .format = PA_SAMPLE_S16LE,
  .channels = AUDIO_CHANNELS,
  .rate = AUDIO_SAMPLE_RATE
};

static void
audio_success_cb (pa_stream * stream, int success, void *data)
{
  struct audio *audio = data;
  pa_threaded_mainloop_signal (audio->mainloop, 0);
}

static void
audio_wait_success (struct audio *audio, pa_operation * operation)
{
  if (!operation)
    {
      debug_print (2, "No operation. Skipping wait...\n");
      return;
    }
  while (pa_operation_get_state (operation) != PA_OPERATION_DONE)
    {
      pa_threaded_mainloop_wait (audio->mainloop);
    }
}

static void
audio_write_callback (pa_stream * stream, size_t size, void *data)
{
  struct audio *audio = data;
  guint32 req_frames;
  void *buffer;
  gint16 *dst, *src;

  if (audio->release_frames > AUDIO_PA_BUFFER_LEN)
    {
      pa_stream_cork (audio->stream, 1, NULL, NULL);
      return;
    }

  req_frames = size >> AUDIO_CHANNELS;

  pa_stream_begin_write (stream, &buffer, &size);

  g_mutex_lock (&audio->control.mutex);

  debug_print (2, "Writing %d frames to %d channels...\n", req_frames,
	       audio->channels);

  if (!audio->sample || !audio->sample->len)
    {
      g_mutex_unlock (&audio->control.mutex);
      pa_stream_cancel_write (stream);
      debug_print (2, "Cancelled\n");
      return;
    }

  memset (buffer, 0, size);

  if ((audio->pos == audio->frames && !audio->loop) ||
      audio->status == AUDIO_STATUS_PREPARING ||
      audio->status == AUDIO_STATUS_STOPPING)
    {
      if (audio->status == AUDIO_STATUS_PREPARING)
	{
	  audio->status = AUDIO_STATUS_PLAYING;
	  g_mutex_unlock (&audio->control.mutex);
	}
      else
	{
	  g_mutex_unlock (&audio->control.mutex);
	  audio->release_frames += req_frames;
	}
      pa_stream_write (stream, buffer, size, NULL, 0, PA_SEEK_RELATIVE);
      return;
    }

  dst = buffer;
  src = (gint16 *) & audio->sample->data[audio->pos << audio->channels];
  for (gint i = 0; i < req_frames; i++)
    {
      if (audio->pos == audio->frames)
	{
	  if (!audio->loop)
	    {
	      break;
	    }
	  debug_print (2, "Sample reset\n");
	  audio->pos = 0;
	  src = (gint16 *) audio->sample->data;
	}

      *dst = *src;
      dst++;
      if (audio->channels == 2)
	{
	  src++;
	}
      *dst = *src;
      src++;
      dst++;
      audio->pos++;
    }

  g_mutex_unlock (&audio->control.mutex);

  pa_stream_write (stream, buffer, size, NULL, 0, PA_SEEK_RELATIVE);
}

void
audio_stop (struct audio *audio, gboolean flush)
{
  pa_operation *operation;

  if (!audio->stream)
    {
      return;
    }

  g_mutex_lock (&audio->control.mutex);
  if (audio->status == AUDIO_STATUS_PREPARING ||
      audio->status == AUDIO_STATUS_PLAYING)
    {
      audio->status = AUDIO_STATUS_STOPPING;
      g_mutex_unlock (&audio->control.mutex);

      debug_print (1, "Stopping audio...\n");

      pa_threaded_mainloop_lock (audio->mainloop);
      if (flush)
	{
	  operation =
	    pa_stream_flush (audio->stream, audio_success_cb, audio);
	  audio_wait_success (audio, operation);
	}

      operation = pa_stream_cork (audio->stream, 1, audio_success_cb, audio);
      audio_wait_success (audio, operation);
      pa_threaded_mainloop_unlock (audio->mainloop);

      g_mutex_lock (&audio->control.mutex);
      audio->status = AUDIO_STATUS_STOPPED;
      g_mutex_unlock (&audio->control.mutex);
    }
  else
    {
      while (audio->status != AUDIO_STATUS_STOPPED)
	{
	  g_mutex_unlock (&audio->control.mutex);
	  usleep (100000);
	  g_mutex_lock (&audio->control.mutex);
	}
      g_mutex_unlock (&audio->control.mutex);
    }
}

void
audio_play (struct audio *audio)
{
  pa_operation *operation;

  if (!audio->stream)
    {
      return;
    }

  audio_stop (audio, TRUE);

  debug_print (1, "Playing audio...\n");

  g_mutex_lock (&audio->control.mutex);
  audio->pos = 0;
  audio->release_frames = 0;
  audio->status = AUDIO_STATUS_PREPARING;
  g_mutex_unlock (&audio->control.mutex);

  pa_threaded_mainloop_lock (audio->mainloop);
  operation = pa_stream_cork (audio->stream, 0, audio_success_cb, audio);
  audio_wait_success (audio, operation);
  pa_threaded_mainloop_unlock (audio->mainloop);
}

static void
audio_set_sink_volume (pa_context * context, const pa_sink_input_info * info,
		       int eol, void *data)
{
  struct audio *audio = data;

  if (info && pa_cvolume_valid (&info->volume))
    {
      gdouble v = pa_sw_volume_to_linear (pa_cvolume_avg (&info->volume));
      debug_print (1, "Setting volume to %f...\n", v);
      audio->volume_change_callback (v);
    }
}

static void
audio_notify (pa_context * context, pa_subscription_event_type_t type,
	      uint32_t index, void *data)
{
  struct audio *audio = data;

  if (audio->context != context)
    {
      return;
    }

  if ((type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) ==
      PA_SUBSCRIPTION_EVENT_SINK_INPUT)
    {
      if ((type & PA_SUBSCRIPTION_EVENT_TYPE_MASK) ==
	  PA_SUBSCRIPTION_EVENT_CHANGE)
	{
	  pa_context_get_sink_input_info (audio->context, audio->index,
					  audio_set_sink_volume, audio);
	}
    }
}

static void
audio_connect_callback (pa_stream * stream, void *data)
{
  struct audio *audio = data;

  if (pa_stream_get_state (stream) == PA_STREAM_READY)
    {
      pa_stream_set_write_callback (stream, audio_write_callback, audio);
      audio->index = pa_stream_get_index (audio->stream);
      debug_print (2, "Sink index: %d\n", audio->index);
      pa_context_get_sink_input_info (audio->context, audio->index,
				      audio_set_sink_volume, audio);
    }
}

static void
audio_context_callback (pa_context * context, void *data)
{
  struct audio *audio = data;
  pa_operation *operation;
  pa_proplist *props = pa_proplist_new ();
  pa_stream_flags_t stream_flags =
    PA_STREAM_START_CORKED | PA_STREAM_INTERPOLATE_TIMING |
    PA_STREAM_NOT_MONOTONIC | PA_STREAM_AUTO_TIMING_UPDATE |
    PA_STREAM_ADJUST_LATENCY;


  if (pa_context_get_state (context) == PA_CONTEXT_READY)
    {
      pa_proplist_set (props,
		       PA_PROP_APPLICATION_ICON_NAME,
		       PACKAGE, sizeof (PACKAGE));
      audio->stream = pa_stream_new_with_proplist (context, PACKAGE,
						   &sample_spec, NULL, props);
      pa_proplist_free (props);
      pa_stream_set_state_callback (audio->stream, audio_connect_callback,
				    audio);
      pa_stream_connect_playback (audio->stream, NULL, &buffer_attributes,
				  stream_flags, NULL, NULL);
      pa_context_set_subscribe_callback (audio->context, audio_notify, audio);
      operation = pa_context_subscribe (audio->context,
					PA_SUBSCRIPTION_MASK_SINK_INPUT, NULL,
					NULL);
      if (operation != NULL)
	{
	  pa_operation_unref (operation);
	}
    }
}

void
audio_init (struct audio *audio, void (*volume_change_callback) (gdouble),
	    job_control_callback load_progress_callback)
{
  debug_print (1, "Initializing audio...\n");
  audio->sample = g_byte_array_new ();
  audio->frames = 0;
  audio->loop = FALSE;
  audio->stream = NULL;
  audio->index = PA_INVALID_INDEX;
  audio->volume_change_callback = volume_change_callback;
  audio->control.callback = load_progress_callback;
  audio->path[0] = 0;
  audio->control.data = g_malloc (sizeof (struct sample_info));
  audio->status = AUDIO_STATUS_STOPPED;
}

gint
audio_run (struct audio *audio)
{
  pa_mainloop_api *api;
  audio->mainloop = pa_threaded_mainloop_new ();
  if (!audio->mainloop)
    {
      return -1;
    }

  api = pa_threaded_mainloop_get_api (audio->mainloop);
  audio->context = pa_context_new (api, PACKAGE);

  if (pa_context_connect (audio->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
    {
      pa_context_unref (audio->context);
      pa_threaded_mainloop_free (audio->mainloop);
      audio->mainloop = NULL;
      return -1;
    }
  else
    {
      pa_context_set_state_callback (audio->context, audio_context_callback,
				     audio);
      pa_threaded_mainloop_start (audio->mainloop);
      pa_threaded_mainloop_wait (audio->mainloop);
    }

  return 0;
}

void
audio_destroy (struct audio *audio)
{
  debug_print (1, "Destroying audio...\n");

  audio_stop (audio, TRUE);
  audio_reset_sample (audio);

  g_mutex_lock (&audio->control.mutex);

  if (audio->mainloop)
    {
      pa_threaded_mainloop_stop (audio->mainloop);
      pa_context_disconnect (audio->context);
      pa_context_unref (audio->context);
      if (audio->stream)
	{
	  pa_stream_unref (audio->stream);
	  audio->stream = NULL;
	}
      pa_threaded_mainloop_free (audio->mainloop);
      audio->mainloop = NULL;
    }

  g_free (audio->control.data);
  g_byte_array_free (audio->sample, TRUE);
  audio->sample = NULL;

  g_mutex_unlock (&audio->control.mutex);
}

gboolean
audio_check (struct audio *audio)
{
  return audio->mainloop ? TRUE : FALSE;
}

void
audio_reset_sample (struct audio *audio)
{
  g_mutex_lock (&audio->control.mutex);
  debug_print (1, "Resetting sample...\n");
  g_byte_array_set_size (audio->sample, 0);
  audio->frames = 0;
  audio->pos = 0;
  audio->path[0] = 0;
  audio->release_frames = AUDIO_PA_BUFFER_LEN;
  audio->src = AUDIO_SRC_NONE;
  audio->status = AUDIO_STATUS_STOPPED;
  memset (audio->control.data, 0, sizeof (struct sample_info));
  g_mutex_unlock (&audio->control.mutex);
}

void
audio_set_volume (struct audio *audio, gdouble volume)
{
  pa_operation *operation;
  pa_volume_t v;

  if (audio->index != PA_INVALID_INDEX)
    {
      debug_print (1, "Setting volume to %f...\n", volume);
      v = pa_sw_volume_from_linear (volume);
      pa_cvolume_set (&audio->volume, AUDIO_CHANNELS, v);

      operation = pa_context_set_sink_input_volume (audio->context,
						    audio->index,
						    &audio->volume, NULL,
						    NULL);
      if (operation != NULL)
	{
	  pa_operation_unref (operation);
	}
    }
}
