/* infinote - Collaborative notetaking application
 * Copyright (C) 2007 Armin Burgmeier
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
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef __INFC_BROWSER_H__
#define __INFC_BROWSER_H__

#include <libinfinity/client/infc-browser-iter.h>
#include <libinfinity/client/infc-explore-request.h>
#include <libinfinity/client/infc-node-request.h>
#include <libinfinity/client/infc-note-plugin.h>
#include <libinfinity/common/inf-connection-manager.h>
#include <libinfinity/common/inf-xml-connection.h>

#include <glib-object.h>

G_BEGIN_DECLS

#define INFC_TYPE_BROWSER                 (infc_browser_get_type())
#define INFC_BROWSER(obj)                 (G_TYPE_CHECK_INSTANCE_CAST((obj), INFC_TYPE_BROWSER, InfcBrowser))
#define INFC_BROWSER_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST((klass), INFC_TYPE_BROWSER, InfcBrowserClass))
#define INFC_IS_BROWSER(obj)              (G_TYPE_CHECK_INSTANCE_TYPE((obj), INFC_TYPE_BROWSER))
#define INFC_IS_BROWSER_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE((klass), INFC_TYPE_BROWSER))
#define INFC_BROWSER_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS((obj), INFC_TYPE_BROWSER, InfcBrowserClass))

typedef struct _InfcBrowser InfcBrowser;
typedef struct _InfcBrowserClass InfcBrowserClass;

typedef enum _InfcBrowserNodeStatus {
  /* The node is in synchronized with the server */
  INFC_BROWSER_NODE_SYNC,
  /* The node has been deleted locally, but the server has not yet
   * acknowledged the deletion and might still reject the request. */
  INFC_BROWSER_NODE_DELETED,
  /* The node has been added locally, but the server has not yet
   * acknowledged the addition and might still reject the request. */
  INFC_BROWSER_NODE_ADDED,
  /* The node has been moved locally, but the server has not yet
   * acknowledged the move and might still reject the request. */
  INFC_BROWSER_NODE_MOVED,
  /* The node has been copied locally, but the server has not yet
   * acknowledget the copy and might still reject the request. */
  INFC_BROWSER_NODE_COPIED,
  /* Inherit status from parent node (used internally) */
  INFC_BROWSER_NODE_INHERIT
} InfcBrowserNodeStatus;

struct _InfcBrowserClass {
  GObjectClass parent_class;

  /* Signals */
  void (*node_added)(InfcBrowser* browser,
                     InfcBrowserIter* iter);

  void (*node_removed)(InfcBrowser* browser,
                       InfcBrowserIter* iter);
};

struct _InfcBrowser {
  GObject parent;
};

GType
infc_browser_get_type(void) G_GNUC_CONST;

InfcBrowser*
infc_browser_new(InfConnectionManager* connection_manager,
                 InfXmlConnection* connection);

InfConnectionManager*
infc_browser_get_connection_manager(InfcBrowser* browser);

InfXmlConnection*
infc_browser_get_connection(InfcBrowser* browser);

gboolean
infc_browser_add_plugin(InfcBrowser* browser,
                        InfcNotePlugin* plugin);

void
infc_browser_iter_get_root(InfcBrowser* browser,
                           InfcBrowserIter* iter);

gboolean
infc_browser_iter_get_next(InfcBrowser* browser,
                           InfcBrowserIter* iter);

gboolean
infc_browser_iter_get_prev(InfcBrowser* browser,
                           InfcBrowserIter* iter);

gboolean
infc_browser_iter_get_parent(InfcBrowser* browser,
                             InfcBrowserIter* iter);

gboolean
infc_browser_iter_get_explored(InfcBrowser* browser,
                               InfcBrowserIter* iter);

gboolean
infc_browser_iter_get_child(InfcBrowser* browser,
                            InfcBrowserIter* iter);

InfcExploreRequest*
infc_browser_iter_explore(InfcBrowser* browser,
                          InfcBrowserIter* iter);

InfcNodeRequest*
infc_browser_add_subdirectory(InfcBrowser* browser,
                              InfcBrowserIter* parent,
                              const gchar* name);

InfcNodeRequest*
infc_browser_add_note(InfcBrowser* browser,
                      InfcBrowserIter* parent,
                      const gchar* name,
                      InfcNotePlugin* plugin);

InfcNodeRequest*
infc_browser_remove_node(InfcBrowser* browser,
                         InfcBrowserIter* iter);

InfcNodeRequest*
infc_browser_subscribe_session(InfcBrowser* browser,
                               InfcBrowserIter* iter);

InfcSession*
infc_browser_iter_get_session(InfcBrowser* browser,
                              InfcBrowserIter* iter);

G_END_DECLS

#endif /* __INFC_BROWSER_H__ */
