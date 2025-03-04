/*
 *   connector.c
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

#include <glib/gi18n.h>
#include "backend.h"
#include "local.h"
#include "connector.h"
#include "connectors/elektron.h"
#include "connectors/microbrute.h"
#include "connectors/cz.h"
#include "connectors/sds.h"
#include "connectors/efactor.h"

static gint
default_handshake (struct backend *backend)
{
  backend->type = BE_TYPE_MIDI;
  backend->device_desc.filesystems = 0;
  backend->fs_ops = NULL;
  backend->destroy_data = backend_destroy_data;
  if (strlen (backend->device_name))
    {
      gchar *device_name = strdup (backend->device_name);
      snprintf (backend->device_name, LABEL_MAX, "%s %s", _("MIDI device"),
		device_name);
      g_free (device_name);
    }
  else
    {
      snprintf (backend->device_name, LABEL_MAX, "%s", _("MIDI device"));
    }
  return 0;
}

struct connector
{
  gint (*handshake) (struct backend * backend);
  const gchar *name;
};

static const struct connector CONNECTOR_ELEKTRON = {
  .handshake = elektron_handshake,
  .name = "elektron"
};

static const struct connector CONNECTOR_MICROBRUTE = {
  .handshake = microbrute_handshake,
  .name = "microbrute"
};

static const struct connector CONNECTOR_CZ = {
  .handshake = cz_handshake,
  .name = "cz"
};

static const struct connector CONNECTOR_SDS = {
  .handshake = sds_handshake,
  .name = "sds"
};

static const struct connector CONNECTOR_EFACTOR = {
  .handshake = efactor_handshake,
  .name = "efactor"
};

static const struct connector CONNECTOR_DEFAULT = {
  .handshake = default_handshake,
  .name = "default"
};

// To speed up detection, connectors that purely rely on the standard device inquiry should go first.

static const struct connector *CONNECTORS[] = {
  &CONNECTOR_MICROBRUTE, &CONNECTOR_ELEKTRON, &CONNECTOR_CZ, &CONNECTOR_SDS,
  &CONNECTOR_EFACTOR, &CONNECTOR_DEFAULT, NULL
};

// A handshake function might return these values:
// 0, the device matches the connector.
// -ENODEV, the device does not match the connector but we can continue with the next connector.
// Other negative errors are allowed but we will not continue with the remaining connectors.

gint
connector_init (struct backend *backend, const gchar * id,
		const gchar * conn_name,
		struct sysex_transfer *sysex_transfer)
{
  gboolean active = TRUE;
  const struct connector **connector;
  int err = backend_init (backend, id);
  if (err)
    {
      return err;
    }

  backend_rx_drain (backend);	//This is needed in case of timeout.

  err = -ENODEV;
  connector = CONNECTORS;
  while (*connector)
    {
      if (sysex_transfer)
	{
	  g_mutex_lock (&sysex_transfer->mutex);
	  active = sysex_transfer->active;
	  g_mutex_unlock (&sysex_transfer->mutex);
	}

      if (!active)
	{
	  err = -ECANCELED;
	  goto end;
	}

      if (conn_name)
	{
	  if (!strcmp (conn_name, (*connector)->name))
	    {
	      debug_print (1, "Testing %s connector...\n",
			   (*connector)->name);
	      err = (*connector)->handshake (backend);
	      if (err && err != -ENODEV)
		{
		  return err;
		}

	      if (!err)
		{
		  debug_print (1, "Using %s connector...\n",
			       (*connector)->name);
		  return 0;
		}
	    }
	}
      else
	{
	  debug_print (1, "Testing %s connector...\n", (*connector)->name);
	  err = (*connector)->handshake (backend);
	  if (err && err != -ENODEV)
	    {
	      return err;
	    }

	  if (!err)
	    {
	      debug_print (1, "Using %s connector...\n", (*connector)->name);
	      return 0;
	    }
	}
      connector++;

      backend_rx_drain (backend);
    }

  error_print ("No device recognized\n");

end:
  backend_destroy (backend);
  return err;
}
