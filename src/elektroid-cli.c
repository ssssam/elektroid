/*
 *   elektroid-cli.c
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
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <signal.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include "backend.h"
#include "connector.h"
#include "utils.h"

#define GET_FS_OPS_OFFSET(member) offsetof(struct fs_operations, member)
#define GET_FS_OPS_FUNC(type,fs,offset) (*(((type *) (((gchar *) fs) + offset))))
#define CHECK_FS_OPS_FUNC(f) if (!(f)) {error_print ("Operation not implemented\n"); return EXIT_FAILURE;}

static struct backend backend;
static struct job_control control;
static gchar *connector, *fs, *op;
const struct fs_operations *fs_ops;

static const gchar *
cli_get_path (gchar * device_path)
{
  gint len = strlen (device_path);
  gchar *path = device_path;
  gint i = 0;

  while (path[0] != '/' && i < len)
    {
      path++;
      i++;
    }

  return path;
}

static gint
cli_ld ()
{
  gint i;
  struct backend_system_device device;
  GArray *devices = backend_get_system_devices ();

  for (i = 0; i < devices->len; i++)
    {
      device = g_array_index (devices, struct backend_system_device, i);
      printf ("%d: %s %s\n", i, device.id, device.name);
    }

  g_array_free (devices, TRUE);

  return EXIT_SUCCESS;
}

static gint
cli_connect (const gchar * device_path)
{
  gint err, id = (gint) atoi (device_path);
  struct backend_system_device device;
  GArray *devices = backend_get_system_devices ();

  if (!devices->len)
    {
      error_print ("Invalid device %d\n", id);
      return -ENODEV;
    }

  device = g_array_index (devices, struct backend_system_device, id);
  err = connector_init (&backend, device.id, connector, NULL);
  g_array_free (devices, TRUE);

  if (!err && fs)
    {
      fs_ops = backend_get_fs_operations (&backend, 0, fs);
      if (!fs_ops)
	{
	  error_print ("Invalid filesystem '%s'\n", fs);
	  return -EINVAL;
	}
    }

  return err;
}

static int
cli_list (int argc, gchar * argv[], int *optind)
{
  const gchar *path;
  struct item_iterator iter;
  gchar *device_path;

  if (*optind == argc)
    {
      error_print ("Remote path missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_path = argv[*optind];
      (*optind)++;
    }

  if (cli_connect (device_path))
    {
      return EXIT_FAILURE;
    }

  path = cli_get_path (device_path);

  CHECK_FS_OPS_FUNC (fs_ops->readdir);
  if (fs_ops->readdir (&backend, &iter, path))
    {
      return EXIT_FAILURE;
    }

  while (!next_item_iterator (&iter))
    {
      fs_ops->print_item (&iter, &backend);
    }

  free_item_iterator (&iter);

  return EXIT_SUCCESS;
}

static int
cli_command_path (int argc, gchar * argv[], int *optind,
		  ssize_t member_offset)
{
  const gchar *path;
  gchar *device_path;
  gint ret;
  fs_path_func f;

  if (*optind == argc)
    {
      error_print ("Remote path missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_path = argv[*optind];
      (*optind)++;
    }

  if (cli_connect (device_path))
    {
      return EXIT_FAILURE;
    }

  path = cli_get_path (device_path);

  f = GET_FS_OPS_FUNC (fs_path_func, fs_ops, member_offset);
  CHECK_FS_OPS_FUNC (f);
  ret = f (&backend, path);
  return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int
cli_command_src_dst (int argc, gchar * argv[], int *optind,
		     ssize_t member_offset)
{
  const gchar *src_path, *dst_path;
  gchar *device_src_path, *device_dst_path;
  gint src_card;
  gint dst_card;
  int ret;
  fs_src_dst_func f;

  if (*optind == argc)
    {
      error_print ("Remote path source missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_src_path = argv[*optind];
      (*optind)++;
    }

  if (*optind == argc)
    {
      error_print ("Remote path destination missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_dst_path = argv[*optind];
      (*optind)++;
    }

  src_card = atoi (device_src_path);
  dst_card = atoi (device_dst_path);
  if (src_card != dst_card)
    {
      error_print ("Source and destination device must be the same\n");
      return EXIT_FAILURE;
    }

  if (cli_connect (device_src_path))
    {
      return EXIT_FAILURE;
    }

  f = GET_FS_OPS_FUNC (fs_src_dst_func, fs_ops, member_offset);
  CHECK_FS_OPS_FUNC (f);
  src_path = cli_get_path (device_src_path);
  dst_path = cli_get_path (device_dst_path);
  ret = f (&backend, src_path, dst_path);
  return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int
cli_command_mv_rename (int argc, gchar * argv[], int *optind)
{
  const gchar *src_path, *dst_path;
  gchar *device_src_path, *device_dst_path;
  gint src_card;
  gint dst_card;
  int ret;
  fs_src_dst_func f;

  if (*optind == argc)
    {
      error_print ("Remote path source missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_src_path = argv[*optind];
      (*optind)++;
    }

  if (*optind == argc)
    {
      error_print ("Remote path destination missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_dst_path = argv[*optind];
      (*optind)++;
    }

  if (cli_connect (device_src_path))
    {
      return EXIT_FAILURE;
    }

  src_card = atoi (device_src_path);
  src_path = cli_get_path (device_src_path);

  // If move is implemented, rename must behave the same way.
  f = fs_ops->move;
  if (f)
    {
      dst_card = atoi (device_dst_path);
      if (src_card != dst_card)
	{
	  error_print ("Source and destination device must be the same\n");
	  return EXIT_FAILURE;
	}
      dst_path = cli_get_path (device_dst_path);
    }
  else
    {
      f = fs_ops->rename;
      dst_path = device_dst_path;
    }

  CHECK_FS_OPS_FUNC (f);
  ret = f (&backend, src_path, dst_path);
  return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int
cli_info (int argc, gchar * argv[], int *optind)
{
  gchar *device_path;
  const gchar *name;

  if (*optind == argc)
    {
      error_print ("Device missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_path = argv[*optind];
      (*optind)++;
    }

  if (cli_connect (device_path))
    {
      return EXIT_FAILURE;
    }

  printf ("%s; filesystems=", backend.device_name);

  for (gint fs = 1; fs <= MAX_BACKEND_FSS; fs <<= 1)
    {
      if (backend.device_desc.filesystems & fs)
	{
	  name = backend_get_fs_operations (&backend, fs, NULL)->name;
	  printf ("%s%s", fs == 1 ? "" : ",", name);
	}
    }
  printf ("\n");

  return EXIT_SUCCESS;
}

static int
cli_df (int argc, gchar * argv[], int *optind)
{
  gchar *device_path;
  gchar *size;
  gchar *diff;
  gchar *free;
  gint res, storage;
  struct backend_storage_stats statfs;

  if (*optind == argc)
    {
      error_print ("Device missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_path = argv[*optind];
      (*optind)++;
    }

  if (cli_connect (device_path))
    {
      return EXIT_FAILURE;
    }

  if (!backend.device_desc.storage || !backend.get_storage_stats)
    {
      return EXIT_FAILURE;
    }

  printf ("%-10.10s%16.16s%16.16s%16.16s%11.10s\n", "Storage", "Size",
	  "Used", "Available", "Use%");

  res = 0;
  for (storage = 1; storage < MAX_BACKEND_STORAGE; storage <<= 1)
    {
      if (backend.device_desc.storage & storage)
	{
	  res |= backend.get_storage_stats (&backend, storage, &statfs);
	  if (res)
	    {
	      continue;
	    }
	  size = get_human_size (statfs.bsize, FALSE);
	  diff = get_human_size (statfs.bsize - statfs.bfree, FALSE);
	  free = get_human_size (statfs.bfree, FALSE);
	  printf ("%-10.10s%16s%16s%16s%10.2f%%\n",
		  statfs.name, size, diff, free,
		  backend_get_storage_stats_percent (&statfs));
	  g_free (size);
	  g_free (diff);
	  g_free (free);
	}
    }

  return res ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int
cli_upgrade_os (int argc, gchar * argv[], int *optind)
{
  gint res;
  const gchar *src_path;
  const gchar *device_path;
  struct sysex_transfer sysex_transfer;

  if (*optind == argc)
    {
      error_print ("Local path missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      src_path = argv[*optind];
      (*optind)++;
    }

  if (*optind == argc)
    {
      error_print ("Remote path missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_path = argv[*optind];
      (*optind)++;
    }

  if (cli_connect (device_path))
    {
      return EXIT_FAILURE;
    }

  sysex_transfer.raw = g_byte_array_new ();
  res = load_file (src_path, sysex_transfer.raw, NULL);
  if (res)
    {
      error_print ("Error while loading '%s'.\n", src_path);
    }
  else
    {
      sysex_transfer.active = TRUE;
      sysex_transfer.timeout = BE_SYSEX_TIMEOUT_MS;
      CHECK_FS_OPS_FUNC (backend.upgrade_os);
      res = backend.upgrade_os (&backend, &sysex_transfer);
    }

  g_byte_array_free (sysex_transfer.raw, TRUE);
  return res ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int
cli_download (int argc, gchar * argv[], int *optind)
{
  const gchar *src_path, *src_dir;
  gchar *src_dirc;
  struct item_iterator iter;
  gchar *device_src_path, *download_path;
  gint res;
  GByteArray *array;

  if (*optind == argc)
    {
      error_print ("Remote path missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_src_path = argv[*optind];
      (*optind)++;
    }

  if (cli_connect (device_src_path))
    {
      return EXIT_FAILURE;
    }

  src_path = cli_get_path (device_src_path);

  control.active = TRUE;
  array = g_byte_array_new ();
  res = fs_ops->download (&backend, src_path, array, &control);
  if (res)
    {
      goto end;
    }

  src_dirc = strdup (src_path);
  src_dir = dirname (src_dirc);
  res = fs_ops->readdir (&backend, &iter, src_dir);
  g_free (src_dirc);
  if (res)
    {
      goto end;
    }

  download_path = fs_ops->get_download_path (&backend, &iter, fs_ops, ".",
					     src_path);
  if (!download_path)
    {
      res = -1;
      goto end;
    }

  res = fs_ops->save (download_path, array, &control);
  g_free (download_path);
  g_free (control.data);

end:
  g_byte_array_free (array, TRUE);
  return res ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int
cli_upload (int argc, gchar * argv[], int *optind)
{
  const gchar *dst_dir;
  gchar *src_path, *device_dst_path, *upload_path;
  gint res;
  GByteArray *array;
  gint32 index = 1;

  if (*optind == argc)
    {
      error_print ("Local path missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      src_path = argv[*optind];
      (*optind)++;
    }

  if (*optind == argc)
    {
      error_print ("Remote path missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_dst_path = argv[*optind];
      (*optind)++;
    }

  if (cli_connect (device_dst_path))
    {
      return EXIT_FAILURE;
    }

  dst_dir = cli_get_path (device_dst_path);

  upload_path = fs_ops->get_upload_path (&backend, NULL, fs_ops, dst_dir,
					 src_path, &index);

  array = g_byte_array_new ();
  control.active = TRUE;
  res = fs_ops->load (src_path, array, &control);
  if (res)
    {
      goto cleanup;
    }

  res = fs_ops->upload (&backend, upload_path, array, &control);
  g_free (control.data);

cleanup:
  g_free (upload_path);
  g_byte_array_free (array, TRUE);
  return res ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int
cli_send (int argc, gchar * argv[], int *optind)
{
  gint err;
  const gchar *device_dst_path, *src_file;
  struct sysex_transfer sysex_transfer;

  if (*optind == argc)
    {
      error_print ("Source file missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      src_file = argv[*optind];
      (*optind)++;
    }

  if (*optind == argc)
    {
      error_print ("Remote device missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_dst_path = argv[*optind];
      (*optind)++;
    }

  connector = "default";
  if (cli_connect (device_dst_path))
    {
      return EXIT_FAILURE;
    }

  sysex_transfer.active = TRUE;
  sysex_transfer.timeout = BE_DUMP_TIMEOUT;
  sysex_transfer.raw = g_byte_array_new ();

  err = load_file (src_file, sysex_transfer.raw, NULL);

  if (!err)
    {
      err = backend_tx_sysex (&backend, &sysex_transfer);
    }

  free_msg (sysex_transfer.raw);

  return err;
}

static int
cli_receive (int argc, gchar * argv[], int *optind)
{
  gint err;
  const gchar *device_src_path, *dst_file;
  struct sysex_transfer sysex_transfer;

  if (*optind == argc)
    {
      error_print ("Remote device missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      device_src_path = argv[*optind];
      (*optind)++;
    }

  if (*optind == argc)
    {
      error_print ("Destination file missing\n");
      return EXIT_FAILURE;
    }
  else
    {
      dst_file = argv[*optind];
      (*optind)++;
    }

  connector = "default";
  if (cli_connect (device_src_path))
    {
      return EXIT_FAILURE;
    }

  sysex_transfer.timeout = BE_DUMP_TIMEOUT;
  sysex_transfer.batch = TRUE;

  backend_rx_drain (&backend);
  //This doesn't need to be synchronized because the CLI is not multithreaded.
  err = backend_rx_sysex (&backend, &sysex_transfer);
  if (!err)
    {
      err = save_file (dst_file, sysex_transfer.raw, NULL);
    }

  free_msg (sysex_transfer.raw);

  return err;
}

gint
set_conn_fs_op_from_command (const gchar * cmd)
{
  gchar *aux;

  connector = strdup (cmd);
  aux = strchr (connector, '-');
  if (!aux)
    {
      g_free (connector);
      return -1;
    }
  *aux = 0;
  aux++;

  fs = strdup (aux);
  aux = strchr (fs, '-');
  if (!aux)
    {
      g_free (connector);
      g_free (fs);
      return -1;
    }

  *aux = 0;
  aux++;

  op = strdup (aux);

  return 0;
}

static void
cli_end (int sig)
{
  g_mutex_lock (&control.mutex);
  control.active = FALSE;
  g_mutex_unlock (&control.mutex);
}

int
main (int argc, gchar * argv[])
{
  gint c;
  gint res;
  gchar *command;
  gint vflg = 0, errflg = 0;
  struct sigaction action;

  action.sa_handler = cli_end;
  sigemptyset (&action.sa_mask);
  action.sa_flags = 0;
  sigaction (SIGTERM, &action, NULL);
  sigaction (SIGQUIT, &action, NULL);
  sigaction (SIGINT, &action, NULL);
  sigaction (SIGHUP, &action, NULL);

  while ((c = getopt (argc, argv, "v")) != -1)
    {
      switch (c)
	{
	case 'v':
	  vflg++;
	  break;
	case '?':
	  errflg++;
	}
    }

  if (optind == argc)
    {
      errflg = 1;
    }
  else
    {
      command = argv[optind];
      optind++;
    }

  if (vflg)
    {
      debug_level = vflg;
    }

  if (errflg > 0)
    {
      fprintf (stderr, "%s\n", PACKAGE_STRING);
      gchar *exec_name = basename (argv[0]);
      fprintf (stderr, "Usage: %s [options] command\n", exec_name);
      exit (EXIT_FAILURE);
    }

  if (!strcmp (command, "ld") || !strcmp (command, "list-devices"))
    {
      res = cli_ld ();
    }
  else if (!strcmp (command, "info") || !strcmp (command, "info-device"))
    {
      res = cli_info (argc, argv, &optind);
    }
  else if (!strcmp (command, "df") || !strcmp (command, "info-storage"))
    {
      res = cli_df (argc, argv, &optind);
    }
  else if (!strcmp (command, "send"))
    {
      res = cli_send (argc, argv, &optind);
    }
  else if (!strcmp (command, "receive"))
    {
      res = cli_receive (argc, argv, &optind);
    }
  else if (!strcmp (command, "upgrade"))
    {
      res = cli_upgrade_os (argc, argv, &optind);
    }
  else
    {
      if (set_conn_fs_op_from_command (command))
	{
	  exit (EXIT_FAILURE);
	}

      debug_print (1,
		   "Connector: \"%s\"; filesystem: \"%s\"; operation: \"%s\"\n",
		   connector, fs, op);

      if (!strcmp (op, "ls") || !strcmp (op, "list"))
	{
	  res = cli_list (argc, argv, &optind);
	}
      else if (!strcmp (op, "mkdir"))
	{
	  res = cli_command_path (argc, argv, &optind,
				  GET_FS_OPS_OFFSET (mkdir));
	}
      else if (!strcmp (op, "rm") || !strcmp (op, "rmdir"))
	{
	  res = cli_command_path (argc, argv, &optind,
				  GET_FS_OPS_OFFSET (delete));
	}
      else if (!strcmp (op, "download") || !strcmp (op, "dl"))
	{
	  res = cli_download (argc, argv, &optind);
	}
      else if (!strcmp (op, "upload") || !strcmp (op, "ul"))
	{
	  res = cli_upload (argc, argv, &optind);
	}
      else if (!strcmp (op, "cl"))
	{
	  res = cli_command_path (argc, argv, &optind,
				  GET_FS_OPS_OFFSET (clear));
	}
      else if (!strcmp (op, "cp"))
	{
	  res = cli_command_src_dst (argc, argv, &optind,
				     GET_FS_OPS_OFFSET (copy));
	}
      else if (!strcmp (op, "sw"))
	{
	  res = cli_command_src_dst (argc, argv, &optind,
				     GET_FS_OPS_OFFSET (swap));
	}
      else if (!strcmp (op, "mv"))
	{
	  res = cli_command_mv_rename (argc, argv, &optind);
	}
      else
	{
	  error_print ("Command '%s' not recognized\n", command);
	  res = EXIT_FAILURE;
	}

      if (backend_check (&backend))
	{
	  backend_destroy (&backend);
	}

      g_free (connector);
      g_free (fs);
      g_free (op);
    }


  usleep (BE_REST_TIME_US * 2);
  return res;
}
