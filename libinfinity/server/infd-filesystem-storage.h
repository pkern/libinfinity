/* libinfinity - a GObject-based infinote implementation
 * Copyright (C) 2007-2015 Armin Burgmeier <armin@arbur.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#ifndef __INFD_FILESYSTEM_STORAGE_H__
#define __INFD_FILESYSTEM_STORAGE_H__

#include <glib-object.h>

#include <libxml/tree.h>

#include <stdio.h>

G_BEGIN_DECLS

#define INFD_TYPE_FILESYSTEM_STORAGE                 (infd_filesystem_storage_get_type())
#define INFD_FILESYSTEM_STORAGE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST((obj), INFD_TYPE_FILESYSTEM_STORAGE, InfdFilesystemStorage))
#define INFD_FILESYSTEM_STORAGE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST((klass), INFD_TYPE_FILESYSTEM_STORAGE, InfdFilesystemStorageClass))
#define INFD_IS_FILESYSTEM_STORAGE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE((obj), INFD_TYPE_FILESYSTEM_STORAGE))
#define INFD_IS_FILESYSTEM_STORAGE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE((klass), INFD_TYPE_FILESYSTEM_STORAGE))
#define INFD_FILESYSTEM_STORAGE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS((obj), INFD_TYPE_FILESYSTEM_STORAGE, InfdFilesystemStorageClass))

typedef struct _InfdFilesystemStorage InfdFilesystemStorage;
typedef struct _InfdFilesystemStorageClass InfdFilesystemStorageClass;

typedef enum _InfdFilesystemStorageError {
  /* The path contains invalid characters */
  INFD_FILESYSTEM_STORAGE_ERROR_INVALID_PATH,
  /* Failed to remove files from disk */
  INFD_FILESYSTEM_STORAGE_ERROR_REMOVE_FILES,
  /* File has invaild format */
  INFD_FILESYSTEM_STORAGE_ERROR_INVALID_FORMAT,

  INFD_FILESYSTEM_STORAGE_ERROR_FAILED
} InfdFilesystemStorageError;

struct _InfdFilesystemStorageClass {
  GObjectClass parent_class;
};

struct _InfdFilesystemStorage {
  GObject parent;
};

GType
infd_filesystem_storage_get_type(void) G_GNUC_CONST;

InfdFilesystemStorage*
infd_filesystem_storage_new(const gchar* root_directory);

gchar*
infd_filesystem_storage_get_path(InfdFilesystemStorage* storage,
                                 const gchar* identifier,
                                 const gchar* path,
                                 GError** error);

FILE*
infd_filesystem_storage_open(InfdFilesystemStorage* storage,
                             const gchar* identifier,
                             const gchar* path,
                             const gchar* mode,
                             gchar** full_path,
                             GError** error);

xmlDocPtr
infd_filesystem_storage_read_xml_file(InfdFilesystemStorage* storage,
                                      const gchar* identifier,
                                      const gchar* path,
                                      const gchar* toplevel_tag,
                                      GError** error);

gboolean
infd_filesystem_storage_write_xml_file(InfdFilesystemStorage* storage,
                                       const gchar* identifier,
                                       const gchar* path,
                                       xmlDocPtr doc,
                                       GError** error);

int
infd_filesystem_storage_stream_close(FILE* file);

gsize
infd_filesystem_storage_stream_read(FILE* file,
                                    gpointer buffer,
                                    gsize len);

gsize
infd_filesystem_storage_stream_write(FILE* file,
                                     gconstpointer buffer,
                                     gsize len);

G_END_DECLS

#endif /* __INFD_FILESYSTEM_STORAGE_H__ */

/* vim:set et sw=2 ts=2: */
