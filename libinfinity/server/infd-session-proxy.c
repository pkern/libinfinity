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

#include <libinfinity/server/infd-session-proxy.h>

#include <libinfinity/common/inf-xml-util.h>
#include <libinfinity/common/inf-error.h>

#include <string.h>

typedef struct _InfdSessionProxySubscription InfdSessionProxySubscription;
struct _InfdSessionProxySubscription {
  InfXmlConnection* connection;
  GSList* users; /* Joined users via this connection */
};

typedef struct _InfdSessionProxyPrivate InfdSessionProxyPrivate;
struct _InfdSessionProxyPrivate {
  InfSession* session;
  InfConnectionManagerGroup* subscription_group;

  GSList* subscriptions;
  guint user_id_counter;

  /* Only relevant if we get a session synchronized. This flag tells whether
   * we should subscribe the synchronizing connection after synchronization
   * is complete, so we do not have to synchronize the session the other way
   * around if that connection wants to be subscribed. */
  gboolean subscribe_sync_conn;

  /* Local users that do not belong to a particular connection */
  GSList* local_users;
};

enum {
  PROP_0,

  PROP_SESSION,
  PROP_SUBSCRIPTION_GROUP,
  PROP_SUBSCRIBE_SYNC_CONN
};

#define INFD_SESSION_PROXY_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), INFD_TYPE_SESSION_PROXY, InfdSessionProxyPrivate))

static GObjectClass* parent_class;

/*
 * SessionProxy subscriptions.
 */

static InfdSessionProxySubscription*
infd_session_proxy_subscription_new(InfXmlConnection* connection)
{
  InfdSessionProxySubscription* subscription;
  subscription = g_slice_new(InfdSessionProxySubscription);

  subscription->connection = connection;
  subscription->users = NULL;

  g_object_ref(G_OBJECT(connection));
  return subscription;
}

static void
infd_session_proxy_subscription_free(InfdSessionProxySubscription* subscr)
{
  GSList* item;
  for(item = subscr->users; item != NULL; item = g_slist_next(item))
    g_object_set(G_OBJECT(item->data), "status", INF_USER_UNAVAILABLE, NULL);

  g_object_unref(G_OBJECT(subscr->connection));
  g_slist_free(subscr->users);
  g_slice_free(InfdSessionProxySubscription, subscr);
}

static GSList*
infd_session_proxy_find_subscription_item(InfdSessionProxy* proxy,
                                          InfXmlConnection* connection)
{
  InfdSessionProxyPrivate* priv;
  GSList* item;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  for(item = priv->subscriptions; item != NULL; item = g_slist_next(item))
    if( ((InfdSessionProxySubscription*)item->data)->connection == connection)
      return item;

  return NULL;
}

static InfdSessionProxySubscription*
infd_session_proxy_find_subscription(InfdSessionProxy* proxy,
                                     InfXmlConnection* connection)
{
  GSList* item;

  item = infd_session_proxy_find_subscription_item(proxy, connection);
  if(item == NULL) return NULL;

  return (InfdSessionProxySubscription*)item->data;
}

/* Required by infd_session_proxy_release_connection() */
static void
infd_session_proxy_connection_notify_status_cb(InfXmlConnection* connection,
                                               const gchar* property,
                                               gpointer user_data);

/* Unlinks a subscription connection from the session. */
static void
infd_session_proxy_release_subscription(InfdSessionProxy* proxy,
                                        InfdSessionProxySubscription* subscr)
{
  InfdSessionProxyPrivate* priv;
  InfXmlConnection* connection;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  connection = subscr->connection;

  g_signal_handlers_disconnect_by_func(
    G_OBJECT(connection),
    G_CALLBACK(infd_session_proxy_connection_notify_status_cb),
    proxy
  );

  inf_connection_manager_unref_connection(
    inf_session_get_connection_manager(priv->session),
    priv->subscription_group,
    connection
  );

  priv->subscriptions = g_slist_remove(priv->subscriptions, subscr);
  infd_session_proxy_subscription_free(subscr);
}

static void
infd_session_proxy_remove_subscription(InfdSessionProxy* proxy,
                                       InfdSessionProxySubscription* subscr)
{
  InfdSessionProxyPrivate* priv;
  xmlNodePtr xml;
  GSList* item;
  gchar id_buf[16];

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  for(item = subscr->users; item != NULL; item = g_slist_next(item))
  {
    sprintf(id_buf, "%u", inf_user_get_id(INF_USER(item->data)));

    xml = xmlNewNode(NULL, (const xmlChar*)"user-status-change");
    xmlNewProp(xml, (const xmlChar*)"id", (const xmlChar*)id_buf);
    xmlNewProp(xml, (const xmlChar*)"status", (const xmlChar*)"unavailable");

    inf_session_send_to_subscriptions(priv->session, subscr->connection, xml);
  }

  infd_session_proxy_release_subscription(proxy, subscr);
}

/*
 * Utility functions.
 */

/* Performs a user join on the given proxy. If connection is not null, the
 * user join is made from that connection, otherwise a local user join is
 * performed. request_seq is the seq of the user join request and used in
 * the reply. It is ignored when connection is NULL. */
static InfUser*
infd_session_proxy_perform_user_join(InfdSessionProxy* proxy,
                                     InfXmlConnection* connection,
                                     const gchar* request_seq,
                                     GArray* user_props,
                                     GError** error)
{
  InfSessionClass* session_class;
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;
  InfUser* user;
  const GParameter* name_param;
  GParameter* param;
  gboolean result;
  xmlNodePtr xml;
  guint i;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  session_class = INF_SESSION_GET_CLASS(priv->session);

  g_assert(session_class->validate_user_props != NULL);
  g_assert(session_class->user_new != NULL);

  name_param = inf_session_lookup_user_property(
    (const GParameter*)user_props->data,
    user_props->len,
    "name"
  );

  if(name_param == NULL)
  {
    g_set_error(
      error,
      inf_request_error_quark(),
      INF_REQUEST_ERROR_NO_SUCH_ATTRIBUTE,
      "Request does not contain required attribute 'name'"
    );

    return NULL;
  }

  user = inf_session_lookup_user_by_name(
    priv->session,
    g_value_get_string(&name_param->value)
  );

  if(user != NULL && inf_user_get_status(user) != INF_USER_UNAVAILABLE)
  {
    g_set_error(
      error,
      inf_user_join_error_quark(),
      INF_USER_JOIN_ERROR_NAME_IN_USE,
      "%s",
      inf_user_join_strerror(INF_USER_JOIN_ERROR_NAME_IN_USE)
    );

    return NULL;
  }

  /* User join requests must not have the id value set because the server
   * chooses an ID, or reuses an existing one in the case of a rejoin. */
  param = inf_session_get_user_property(user_props, "id");
  if(G_IS_VALUE(&param->value))
  {
    g_set_error(
      error,
      inf_user_join_error_quark(),
      INF_USER_JOIN_ERROR_ID_PROVIDED,
      "%s",
      inf_user_join_strerror(INF_USER_JOIN_ERROR_ID_PROVIDED)
    );

    return NULL;
  }

  /* The user ID counter is increased in the add-user default signal
   * handler. */
  g_value_init(&param->value, G_TYPE_UINT);

  /* Reuse user ID on rejoin. */
  if(user != NULL)
    g_value_set_uint(&param->value, inf_user_get_id(user));
  else
    g_value_set_uint(&param->value, priv->user_id_counter);

  /* Again, if a user joins, the status is always available, so it should
   * not be already provided. */
  param = inf_session_get_user_property(user_props, "status");
  if(G_IS_VALUE(&param->value))
  {
    g_set_error(
      error,
      inf_user_join_error_quark(),
      INF_USER_JOIN_ERROR_STATUS_PROVIDED,
      "%s",
      inf_user_join_strerror(INF_USER_JOIN_ERROR_STATUS_PROVIDED)
    );

    return NULL;
  }

  g_value_init(&param->value, G_TYPE_ENUM);
  g_value_set_enum(&param->value, INF_USER_AVAILABLE);

  /* flags should not be set by get_xml_user_props, nor given
   * to infd_session_proxy_add_user. */
  param = inf_session_get_user_property(user_props, "flags");
  g_assert(!G_IS_VALUE(&param->value));

  g_value_init(&param->value, G_TYPE_FLAGS);
  if(connection == NULL)
    g_value_set_flags(&param->value, INF_USER_LOCAL);
  else
    g_value_set_flags(&param->value, 0);

  if(user == NULL)
  {
    /* This validates properties */
    user = inf_session_add_user(
      priv->session,
      (const GParameter*)user_props->data,
      user_props->len,
      error
    );

    if(user == NULL)
      return NULL;

    xml = xmlNewNode(NULL, (const xmlChar*)"user-join");
  }
  else
  {
    /* Validate properties, but exclude the rejoining user from the check.
     * Otherwise, we would get conflicts because the name and the ID
     * of the request and the rejoining user are the same. */
    result = session_class->validate_user_props(
      priv->session,
      (const GParameter*)user_props->data,
      user_props->len,
      user,
      error
    );

    if(result == FALSE)
      return NULL;

    g_object_freeze_notify(G_OBJECT(user));

    /* Set properties on already existing user object. */
    for(i = 0; i < user_props->len; ++ i)
    {
      param = &g_array_index(user_props, GParameter, i);

      /* Don't set name and ID because they did not change, and we are not
       * even allowed to set ID because it is construct only. */
      if(strcmp(param->name, "name") != 0 && strcmp(param->name, "id") != 0)
        g_object_set_property(G_OBJECT(user), param->name, &param->value);
    }

    g_object_thaw_notify(G_OBJECT(user));

    xml = xmlNewNode(NULL, (const xmlChar*)"user-rejoin");
  }

  inf_session_user_to_xml(priv->session, user, xml);

  /* exclude the connection from which the request comes. The reply to it
   * is sent separately telling it that the user join was accepted. */
  inf_session_send_to_subscriptions(
    priv->session,
    connection,
    xmlCopyNode(xml, 1)
  );

  if(connection != NULL)
  {
    xmlNewProp(xml, (const xmlChar*)"seq", (const xmlChar*)request_seq);

    inf_connection_manager_send_to(
      inf_session_get_connection_manager(priv->session),
      priv->subscription_group,
      connection,
      xml
    );

    subscription = infd_session_proxy_find_subscription(proxy, connection);
    g_assert(subscription != NULL);

    subscription->users = g_slist_prepend(subscription->users, user);
  }
  else
  {
    priv->local_users = g_slist_prepend(priv->local_users, user);
    xmlFreeNode(xml);
  }

  return user;
}

/* Subscribes the given connection to the session without synchronizing it. */
static void
infd_session_proxy_subscribe_connection(InfdSessionProxy* proxy,
                                        InfXmlConnection* connection)
{
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* Note that if this is called from (the public)
   * infd_session_proxy_subscribe_to, then the InfSession has already
   * refed the connection in inf_session_synchronize_to(). However, since we
   * want to keep it after the synchronization finishes we have to add
   * another reference here. */
  inf_connection_manager_ref_connection(
    inf_session_get_connection_manager(priv->session),
    priv->subscription_group,
    connection
  );

  g_signal_connect(
    G_OBJECT(connection),
    "notify::status",
    G_CALLBACK(infd_session_proxy_connection_notify_status_cb),
    proxy
  );

  subscription = infd_session_proxy_subscription_new(connection);
  priv->subscriptions = g_slist_prepend(priv->subscriptions, subscription);
}

/*
 * Signal handlers.
 */

static void
infd_session_proxy_connection_notify_status_cb(InfXmlConnection* connection,
                                               const gchar* property,
                                               gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxySubscription* subscription;
  InfXmlConnectionStatus status;

  proxy = INFD_SESSION_PROXY(user_data);

  g_object_get(G_OBJECT(connection), "status", &status, NULL);

  if(status == INF_XML_CONNECTION_CLOSED ||
     status == INF_XML_CONNECTION_CLOSING)
  {
    subscription = infd_session_proxy_find_subscription(proxy, connection);
    g_assert(subscription != NULL);

    infd_session_proxy_remove_subscription(proxy, subscription);
  }
}

static void
infd_session_proxy_session_add_user_cb(InfSession* session,
                                       InfUser* user,
                                       gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* Make sure that we generate a non-existing user ID for the next user. */
  if(priv->user_id_counter <= inf_user_get_id(user))
    priv->user_id_counter = inf_user_get_id(user) + 1;
}

static void
infd_session_proxy_session_synchronization_complete_cb(InfSession* session,
                                                       InfXmlConnection* conn,
                                                       gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfSessionStatus status;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  g_object_get(session, "status", &status, NULL);

  if(status == INF_SESSION_SYNCHRONIZING)
  {
    if(priv->subscribe_sync_conn == TRUE)
    {
      /* Do not use subscribe_to here because this would synchronize the
       * session to conn. However, we just got it synchronized the
       * other way around and therefore no further synchronization is
       * required. */
      infd_session_proxy_subscribe_connection(proxy, conn);
    }
  }
}

static void
infd_session_proxy_session_synchronization_failed_cb(InfSession* session,
                                                     InfXmlConnection* conn,
                                                     const GError* error,
                                                     gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfSessionStatus status;
  InfdSessionProxySubscription* subscription;

  proxy = INFD_SESSION_PROXY(user_data);

  g_object_get(session, "status", &status, NULL);

  /* We do not need handle the status == INF_SESSION_PROXY_SYNCHRONIZING case
   * since there cannot be any subscriptions while we are synchronizing. */

  if(status == INF_SESSION_RUNNING)
  {
    subscription = infd_session_proxy_find_subscription(proxy, conn);
    if(subscription != NULL)
    {
      /* Note that it should not matter whether we call
       * infd_session_proxy_release_subscription or
       * infd_session_proxy_remove_subscription
       * because there cannot be any users joined via the connection anyway,
       * because it was not yet synchronized. */
      infd_session_proxy_release_subscription(proxy, subscription);
    }
  }
}

static void
infd_session_proxy_session_close_cb(InfSession* session,
                                    gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;
  InfSessionSyncStatus status;
  xmlNodePtr xml;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  while(priv->subscriptions != NULL)
  {
    subscription = (InfdSessionProxySubscription*)priv->subscriptions->data;

    status = inf_session_get_synchronization_status(
      priv->session,
      subscription->connection
    );

    /* If synchronization is still in progress, the default handler of
     * InfSession will cancel the synchronization in which case we do
     * not need to send an extra session-close message. */

    /* We send session_close when we are in AWAITING_ACK status. In
     * AWAITING_ACK status we cannot cancel the synchronization anymore
     * because everything has already been sent out. Therefore the client
     * will eventuelly get in RUNNING state when he receives this message,
     * and process it correctly. */
    if(status != INF_SESSION_SYNC_IN_PROGRESS)
    {
      xml = xmlNewNode(NULL, (const xmlChar*)"session-close");

      inf_connection_manager_send_to(
        inf_session_get_connection_manager(priv->session),
        priv->subscription_group,
        subscription->connection,
        xml
      );
    }

    /* Do not call remove_subscription because this would try to send
     * messages about leaving users, but we are sending session_proxy-close
     * to all subscriptions anyway. */
    infd_session_proxy_release_subscription(proxy, subscription);
  }

  inf_connection_manager_unref_group(
    inf_session_get_connection_manager(priv->session),
    priv->subscription_group
  );

  priv->subscription_group = NULL;

  g_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->session),
    G_CALLBACK(infd_session_proxy_session_close_cb),
    proxy
  );
  
  g_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->session),
    G_CALLBACK(infd_session_proxy_session_add_user_cb),
    proxy
  );
  
  g_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->session),
    G_CALLBACK(infd_session_proxy_session_synchronization_complete_cb),
    proxy
  );
  
  g_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->session),
    G_CALLBACK(infd_session_proxy_session_synchronization_failed_cb),
    proxy
  );

  g_object_unref(priv->session);
  priv->session = NULL;
}

/*
 * GObject overrides.
 */

static void
infd_session_proxy_init(GTypeInstance* instance,
                        gpointer g_class)
{
  InfdSessionProxy* session_proxy;
  InfdSessionProxyPrivate* priv;

  session_proxy = INFD_SESSION_PROXY(instance);
  priv = INFD_SESSION_PROXY_PRIVATE(session_proxy);

  priv->subscriptions = NULL;
  priv->subscription_group = NULL;
  priv->user_id_counter = 1;
  priv->subscribe_sync_conn = FALSE;
  priv->local_users = NULL;
}

static GObject*
infd_session_proxy_constructor(GType type,
                               guint n_construct_properties,
                               GObjectConstructParam* construct_properties)
{
  GObject* object;
  InfdSessionProxy* session_proxy;
  InfdSessionProxyPrivate* priv;

  object = G_OBJECT_CLASS(parent_class)->constructor(
    type,
    n_construct_properties,
    construct_properties
  );

  session_proxy = INFD_SESSION_PROXY(object);
  priv = INFD_SESSION_PROXY_PRIVATE(session_proxy);

  g_assert(priv->subscription_group != NULL);
  g_assert(priv->session != NULL);

  /* TODO: We could perhaps optimize by only setting the subscription
   * group when there are subscribed connections. */
  inf_session_set_subscription_group(priv->session, priv->subscription_group);

  return object;
}

static void
infd_session_proxy_dispose(GObject* object)
{
  InfdSessionProxy* session_proxy;
  InfdSessionProxyPrivate* priv;
  InfConnectionManager* manager;

  session_proxy = INFD_SESSION_PROXY(object);
  priv = INFD_SESSION_PROXY_PRIVATE(session_proxy);

  g_object_ref(G_OBJECT(manager));

  g_slist_free(priv->local_users);
  priv->local_users = NULL;

  /* Note this emits the close signal, removing all subscriptions and
   * the subscription group */
  g_object_unref(G_OBJECT(priv->session));
  priv->session = NULL;

  g_assert(priv->subscription_group == NULL);
  g_assert(priv->subscriptions == NULL);

  g_object_unref(G_OBJECT(manager));
}

static void
infd_session_proxy_set_property(GObject* object,
                                guint prop_id,
                                const GValue* value,
                                GParamSpec* pspec)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(object);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  switch(prop_id)
  {
  case PROP_SESSION:
    g_assert(priv->session == NULL); /* construct only */
    priv->session = INF_SESSION(g_value_dup_object(value));

    g_signal_connect(
      G_OBJECT(priv->session),
      "close",
      G_CALLBACK(infd_session_proxy_session_close_cb),
      proxy
    );

    g_signal_connect(
      G_OBJECT(priv->session),
      "add-user",
      G_CALLBACK(infd_session_proxy_session_add_user_cb),
      proxy
    );

    g_signal_connect(
      G_OBJECT(priv->session),
      "synchronization-complete",
      G_CALLBACK(infd_session_proxy_session_synchronization_complete_cb),
      proxy
    );

    g_signal_connect(
      G_OBJECT(priv->session),
      "synchronization-failed",
      G_CALLBACK(infd_session_proxy_session_synchronization_failed_cb),
      proxy
    );

    break;
  case PROP_SUBSCRIPTION_GROUP:
    g_assert(priv->subscription_group == NULL); /* construct only */
    priv->subscription_group =
      (InfConnectionManagerGroup*)g_value_dup_boxed(value);
    break;
  case PROP_SUBSCRIBE_SYNC_CONN:
    priv->subscribe_sync_conn = g_value_get_boolean(value);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
infd_session_proxy_get_property(GObject* object,
                                guint prop_id,
                                GValue* value,
                                GParamSpec* pspec)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(object);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  switch(prop_id)
  {
  case PROP_SESSION:
    g_value_set_object(value, G_OBJECT(priv->session));
    break;
  case PROP_SUBSCRIPTION_GROUP:
    g_value_set_boxed(value, priv->subscription_group);
    break;
  case PROP_SUBSCRIBE_SYNC_CONN:
    g_value_set_boolean(value, priv->subscribe_sync_conn);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

/*
 * Message handling.
 */

static gboolean
infd_session_proxy_handle_user_join(InfdSessionProxy* proxy,
                                    InfXmlConnection* connection,
                                    xmlNodePtr xml,
                                    GError** error)
{
  InfdSessionProxyPrivate* priv;
  InfSessionClass* session_class;
  GArray* array;
  InfUser* user;
  xmlChar* seq_attr;
  guint i;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  session_class = INF_SESSION_CLASS(priv->session);

  array = session_class->get_xml_user_props(
    priv->session,
    connection,
    xml
  );

  seq_attr = xmlGetProp(xml, (const xmlChar*)"seq");
  user = infd_session_proxy_perform_user_join(
    proxy,
    connection,
    (const gchar*)seq_attr,
    array,
    error
  );

  xmlFree(seq_attr);

  for(i = 0; i < array->len; ++ i)
    g_value_unset(&g_array_index(array, GParameter, i).value);

  g_array_free(array, TRUE);

  if(user == NULL)
    return FALSE;

  return TRUE;
}

static gboolean
infd_session_proxy_handle_user_leave(InfdSessionProxy* proxy,
                                     InfXmlConnection* connection,
                                     xmlNodePtr xml,
                                     GError** error)
{
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;
  InfUser* user;
  xmlChar* id_attr;
  xmlChar* seq_attr;
  guint id;

  xmlNodePtr reply_xml;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  subscription = infd_session_proxy_find_subscription(proxy, connection);
  g_assert(subscription != NULL);

  id_attr = xmlGetProp(xml, (const xmlChar*)"id");
  if(id_attr == NULL)
  {
    g_set_error(
      error,
      inf_user_leave_error_quark(),
      INF_USER_LEAVE_ERROR_ID_NOT_PRESENT,
      "%s",
      inf_user_leave_strerror(INF_USER_LEAVE_ERROR_ID_NOT_PRESENT)
    );

    return FALSE;
  }

  id = strtoul((const gchar*)id_attr, NULL, 0);
  xmlFree(id_attr);

  user = inf_session_lookup_user_by_id(priv->session, id);
  if(user == NULL)
  {
    g_set_error(
      error,
      inf_user_leave_error_quark(),
      INF_USER_LEAVE_ERROR_NO_SUCH_USER,
      "%s",
      inf_user_leave_strerror(INF_USER_LEAVE_ERROR_NO_SUCH_USER)
    );

    return FALSE;
  }

  if(g_slist_find(subscription->users, user) == NULL)
  {
    g_set_error(
      error,
      inf_user_leave_error_quark(),
      INF_USER_LEAVE_ERROR_NOT_JOINED,
      "%s",
      inf_user_leave_strerror(INF_USER_LEAVE_ERROR_NOT_JOINED)
    );

    return FALSE;
  }

  reply_xml = xmlNewNode(NULL, (const xmlChar*)"user-leave");
  inf_xml_util_set_attribute_uint(reply_xml, "id", id);

  /* TODO: Only send seq back to the user that made the request */
  seq_attr = xmlGetProp(xml, (const xmlChar*)"seq");
  if(seq_attr != NULL)
  {
    xmlNewProp(reply_xml, (const xmlChar*)"seq", seq_attr);
    xmlFree(seq_attr);
  }

  inf_session_send_to_subscriptions(priv->session, NULL, reply_xml);
  subscription->users = g_slist_remove(subscription->users, user);

  g_object_set(G_OBJECT(user), "status", INF_USER_UNAVAILABLE, NULL);

  return TRUE;
}

static gboolean
infd_session_proxy_handle_session_unsubscribe(InfdSessionProxy* proxy,
                                              InfXmlConnection* connection,
                                              const xmlNodePtr xml,
                                              GError** error)
{
  InfdSessionProxySubscription* subscription;

  subscription = infd_session_proxy_find_subscription(proxy, connection);
  g_assert(subscription != NULL);

  infd_session_proxy_remove_subscription(proxy, subscription);
  return TRUE;
}

/*
 * InfNetObject implementation
 */

static void
infd_session_proxy_net_object_sent(InfNetObject* net_object,
                                   InfXmlConnection* connection,
                                   xmlNodePtr node)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(net_object);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* TODO: Don't forward for messages the proxy issued */

  g_assert(priv->session != NULL);
  inf_net_object_sent(INF_NET_OBJECT(priv->session), connection, node);
}

static void
infd_session_proxy_net_object_enqueued(InfNetObject* net_object,
                                       InfXmlConnection* connection,
                                       xmlNodePtr node)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(net_object);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* TODO: Don't forward for messages the proxy issued */

  g_assert(priv->session != NULL);
  inf_net_object_enqueued(INF_NET_OBJECT(priv->session), connection, node);
}

static void
infd_session_proxy_net_object_received(InfNetObject* net_object,
                                       InfXmlConnection* connection,
                                       xmlNodePtr node)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfSessionSyncStatus status;
  GError* error;
  gboolean result;
  xmlNodePtr reply_xml;
  xmlChar* seq_attr;

  proxy = INFD_SESSION_PROXY(net_object);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* TODO: Don't forward for messages the proxy issued */

  g_assert(priv->session != NULL);
  status = inf_session_get_synchronization_status(priv->session, connection);
  error = NULL;

  if(status != INF_SESSION_SYNC_NONE)
  {
    inf_net_object_received(INF_NET_OBJECT(priv->session), connection, node);
    result = TRUE;
  }
  else
  {
    if(strcmp((const char*)node->name, "join-user") == 0)
    {
      result = infd_session_proxy_handle_user_join(
        proxy,
        connection,
        node,
        &error
      );
    }
    else if(strcmp((const char*)node->name, "leave-user") == 0)
    {
      result = infd_session_proxy_handle_user_leave(
        proxy,
        connection,
        node,
        &error
      );
    }
    else if(strcmp((const char*)node->name, "session-unsubscribe") == 0)
    {
      result = infd_session_proxy_handle_session_unsubscribe(
        proxy,
        connection,
        node,
        &error
      );
    }
    else
    {
      inf_net_object_received(
        INF_NET_OBJECT(priv->session),
        connection,
        node
      );

      result = TRUE;
    }
  }

  if(result == FALSE && error != NULL)
  {
    /* Only send request-failed when it was a proxy-related request */
    reply_xml = xmlNewNode(NULL, (const xmlChar*)"request-failed");
    inf_xml_util_set_attribute_uint(reply_xml, "code", error->code);

    xmlNewProp(
      reply_xml,
      (const xmlChar*)"domain",
      (const xmlChar*)g_quark_to_string(error->domain)
    );

    seq_attr = xmlGetProp(node, (const xmlChar*)"seq");
    if(seq_attr != NULL)
    {
      xmlNewProp(reply_xml, (const xmlChar*)"seq", seq_attr);
      xmlFree(seq_attr);
    }

    inf_connection_manager_send_to(
      inf_session_get_connection_manager(priv->session),
      priv->subscription_group,
      connection,
      reply_xml
    );

    g_error_free(error);
  }  
}

/*
 * GType registration.
 */

static void
infd_session_proxy_class_init(gpointer g_class,
                              gpointer class_data)
{
  GObjectClass* object_class;
  object_class = G_OBJECT_CLASS(g_class);

  parent_class = G_OBJECT_CLASS(g_type_class_peek_parent(g_class));
  g_type_class_add_private(g_class, sizeof(InfdSessionProxyPrivate));

  object_class->constructor = infd_session_proxy_constructor;
  object_class->dispose = infd_session_proxy_dispose;
  object_class->set_property = infd_session_proxy_set_property;
  object_class->get_property = infd_session_proxy_get_property;

  g_object_class_install_property(
    object_class,
    PROP_SESSION,
    g_param_spec_object(
      "session",
      "Session",
      "The underlaying session",
      INF_TYPE_SESSION,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_SUBSCRIPTION_GROUP,
    g_param_spec_boxed(
      "subscription-group",
      "Subscription group",
      "The connection manager group of subscribed connections",
      INF_TYPE_CONNECTION_MANAGER_GROUP,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_SUBSCRIBE_SYNC_CONN,
    g_param_spec_boolean(
      "subscribe-sync-connection",
      "Subscribe synchronizing connection",
      "Whether to subscribe the initial synchronizing connection after "
      "successful synchronization",
      FALSE,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );
}

static void
infd_session_proxy_net_object_init(gpointer g_iface,
                                   gpointer iface_data)
{
  InfNetObjectIface* iface;
  iface = (InfNetObjectIface*)g_iface;

  iface->sent = infd_session_proxy_net_object_sent;
  iface->enqueued = infd_session_proxy_net_object_enqueued;
  iface->received = infd_session_proxy_net_object_received;
}

GType
infd_session_proxy_get_type(void)
{
  static GType session_proxy_type = 0;

  if(!session_proxy_type)
  {
    static const GTypeInfo session_proxy_type_info = {
      sizeof(InfdSessionProxyClass),    /* class_size */
      NULL,                             /* base_init */
      NULL,                             /* base_finalize */
      infd_session_proxy_class_init,    /* class_init */
      NULL,                             /* class_finalize */
      NULL,                             /* class_data */
      sizeof(InfdSessionProxy),         /* instance_size */
      0,                                /* n_preallocs */
      infd_session_proxy_init,          /* instance_init */
      NULL                              /* value_table */
    };

    static const GInterfaceInfo net_object_info = {
      infd_session_proxy_net_object_init,
      NULL,
      NULL
    };

    session_proxy_type = g_type_register_static(
      G_TYPE_OBJECT,
      "InfdSessionProxy",
      &session_proxy_type_info,
      0
    );

    g_type_add_interface_static(
      session_proxy_type,
      INF_TYPE_NET_OBJECT,
      &net_object_info
    );
  }

  return session_proxy_type;
}

/*
 * Public API.
 */

/** inf_session_proxy_get_session:
 *
 * @proxy: A #InfdSessionProxy.
 *
 * Returns the session proxied by @proxy. Returns %NULL if the session was
 * closed.
 *
 * Return Value: A #InfSession, or %NULL.
 **/
InfSession*
infd_session_proxy_get_session(InfdSessionProxy* proxy)
{
  g_return_val_if_fail(INFD_IS_SESSION_PROXY(proxy), NULL);
  return INFD_SESSION_PROXY_PRIVATE(proxy)->session;
}

/** infd_session_proxy_add_user:
 *
 * @proxy: A #InfdSessionProxy.
 * @params: Construction properties for the #InfUser (or derived) object.
 * @n_params: Number of parameters.
 * @error: Location to store error information.
 *
 * Adds a local user to @proxy's session. @params must not contain the
 * 'id' property because it will be choosen by the proxy. Also, if the 'name'
 * property is already in use by an existing, but unavailable user, this user 
 * will be re-used.
 *
 * Return Value: The #InfUser that has been added, or %NULL in case of an
 * error.
 **/
InfUser*
infd_session_proxy_add_user(InfdSessionProxy* proxy,
                            const GParameter* params,
                            guint n_params,
                            GError** error)
{
  InfUser* user;
  GArray* array;

  g_return_val_if_fail(INFD_IS_SESSION_PROXY(proxy), NULL);

  /* TODO: Make sure values added by infd_session_proxy_perform_user_join are
   * released, for example by inserting copies into the array, and freeing
   * the values after the call. */
  array = g_array_sized_new(FALSE, FALSE, sizeof(GParameter), n_params + 2);
  g_array_append_vals(array, params, n_params);

  user = infd_session_proxy_perform_user_join(
    proxy,
    NULL,
    NULL,
    array,
    error
  );

  g_array_free(array, TRUE);

  return user;
}

/** infd_session_proxy_subscribe_to:
 *
 * @proxy: A #InfdSessionProxy whose session is in state %INF_SESSION_RUNNING.
 * @connection: A #InfConnection that is not yet subscribed.
 *
 * Subscribes @connection to @proxy's session. The first thing that will be
 * done is a synchronization (see inf_session_synchronize_to()). Then, all
 * changes to the session are propagated to @connection.
 *
 * A subscription can only be initialted if @proxy's session is in state
 * %INF_SESSION_RUNNING.
 **/
void
infd_session_proxy_subscribe_to(InfdSessionProxy* proxy,
                                InfXmlConnection* connection)
{
  InfdSessionProxyPrivate* priv;

  g_return_if_fail(INFD_IS_SESSION_PROXY(proxy));
  g_return_if_fail(INF_IS_XML_CONNECTION(connection));

  g_return_if_fail(
    infd_session_proxy_find_subscription(proxy, connection) == NULL
  );

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  g_return_if_fail(priv->session != NULL);

  /* Directly synchronize within the subscription group so that we do not
   * need a group change after synchronization, and the connection already
   * receives requests from other group member to process after
   * synchronization. */
  inf_session_synchronize_to(
    priv->session,
    priv->subscription_group,
    connection
  );

  infd_session_proxy_subscribe_connection(proxy, connection);
}

/* vim:set et sw=2 ts=2: */
