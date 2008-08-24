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

/* TODO: Add some of the replay logic as public libinfinity API */

#include <libinftext/inf-text-session.h>
#include <libinftext/inf-text-default-buffer.h>
#include <libinftext/inf-text-buffer.h>

#include <libinfinity/common/inf-connection-manager.h>
#include <libinfinity/common/inf-central-method.h>
#include <libinfinity/common/inf-simulated-connection.h>
#include <libinfinity/common/inf-standalone-io.h>
#include <libinfinity/common/inf-io.h>

#include <libxml/xmlreader.h>

#include <string.h>

#define XML_READER_TYPE_ELEMENT 1
#define XML_READER_TYPE_SIGNIFICANT_WHITESPACE 14
#define XML_READER_TYPE_END_ELEMENT 15

static const InfConnectionManagerMethodDesc SIMULATED_CENTRAL_METHOD = {
  "simulated",
  "central",
  inf_central_method_open,
  inf_central_method_join,
  inf_central_method_finalize,
  inf_central_method_receive_msg,
  inf_central_method_receive_ctrl,
  inf_central_method_add_connection,
  inf_central_method_remove_connection,
  inf_central_method_send_to_net
};

static const InfConnectionManagerMethodDesc* const METHODS[] = {
  &SIMULATED_CENTRAL_METHOD,
  NULL
};

static GQuark inf_test_text_replay_error_quark;

typedef enum _InfTestTextReplayError {
  INF_TEST_TEXT_REPLAY_UNEXPECTED_EOF,
  INF_TEST_TEXT_REPLAY_UNEXPECTED_NODE
} InfTestTextReplayError;

typedef struct _InfTestTextReplayObject InfTestTextReplayObject;
struct _InfTestTextReplayObject {
  GObject parent_instance;
};

typedef struct _InfTestTextReplayObjectClass InfTestTextReplayObjectClass;
struct _InfTestTextReplayObjectClass {
  GObjectClass parent_class;
};

#define INF_TEST_TYPE_TEXT_REPLAY_OBJECT \
  (inf_test_text_replay_object_get_type())

static void
inf_test_text_replay_object_init(InfTestTextReplayObject* self)
{
}

static void
inf_test_text_replay_object_class_init(InfTestTextReplayObjectClass* klass)
{
}

static void
inf_test_text_replay_object_net_object_init(InfNetObjectIface* iface)
{
}

/* Dummy type implementing InfNetObject for the sending side */
G_DEFINE_TYPE_WITH_CODE(
  InfTestTextReplayObject,
  inf_test_text_replay_object,
  G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(
    INF_TYPE_NET_OBJECT,
    inf_test_text_replay_object_net_object_init
  )
)

static xmlNodePtr
inf_test_text_replay_read_current(xmlTextReaderPtr reader,
                                  GError** error)
{
  xmlError* xml_error;
  xmlNodePtr cur;

  cur = xmlTextReaderExpand(reader);
  if(cur == NULL)
  {
    xml_error = xmlGetLastError();

    g_set_error(
      error,
      g_quark_from_static_string("LIBXML2_READER_ERROR"),
      xml_error->code,
      "%s",
      xml_error->message
    );

    return NULL;
  }

  return cur;
}

static gboolean
inf_test_text_replay_handle_advance_required_result(xmlTextReaderPtr reader,
                                                    int result,
                                                    GError** error)
{
  xmlError* xml_error;

  switch(result)
  {
  case -1:
    xml_error = xmlGetLastError();

    g_set_error(
      error,
      g_quark_from_static_string("LIBXML2_READER_ERROR"),
      xml_error->code,
      "%s",
      xml_error->message
    );

    return FALSE;
  case 0:
    g_set_error(
      error,
      inf_test_text_replay_error_quark,
      INF_TEST_TEXT_REPLAY_UNEXPECTED_EOF,
      "Unexpected end of document"
    );

    return FALSE;
  case 1:
    return TRUE;
  default:
    g_assert_not_reached();
    return FALSE;
  }
}

static gboolean
inf_test_text_replay_advance_required(xmlTextReaderPtr reader,
                                      GError** error)
{
  int result;
  result = xmlTextReaderRead(reader);

  return inf_test_text_replay_handle_advance_required_result(
    reader,
    result,
    error
  );
}

static gboolean
inf_test_text_replay_advance_subtree_required(xmlTextReaderPtr reader,
                                              GError** error)
{
  int result;
  result = xmlTextReaderNext(reader);

  return inf_test_text_replay_handle_advance_required_result(
    reader,
    result,
    error
  );
}

static gboolean
inf_test_text_replay_skip_whitespace_required(xmlTextReaderPtr reader,
                                              GError** error)
{
  while(xmlTextReaderNodeType(reader) ==
        XML_READER_TYPE_SIGNIFICANT_WHITESPACE)
  {
    if(inf_test_text_replay_advance_required(reader, error) == FALSE)
      return FALSE;
  }

  return TRUE;
}

static gboolean
inf_test_text_replay_advance_skip_whitespace_required(xmlTextReaderPtr reader,
                                                      GError** error)
{
  if(!inf_test_text_replay_advance_required(reader, error))
    return FALSE;
  if(!inf_test_text_replay_skip_whitespace_required(reader, error))
    return FALSE;
  return TRUE;
}

static gboolean
inf_test_text_replay_play_initial(xmlTextReaderPtr reader,
                                  InfConnectionManagerGroup* publisher_group,
                                  InfXmlConnection* publisher,
                                  GError** error)
{
  xmlNodePtr cur;

  if(!inf_test_text_replay_advance_skip_whitespace_required(reader, error))
    return FALSE;

  while(xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT)
  {
    cur = inf_test_text_replay_read_current(reader, error);
    if(cur == NULL) return FALSE;

    inf_connection_manager_group_send_to_connection(
      publisher_group,
      publisher,
      xmlCopyNode(cur, 1)
    );

    /* TODO: Check whether that caused an error */
    inf_simulated_connection_flush(INF_SIMULATED_CONNECTION(publisher));

    if(!inf_test_text_replay_advance_subtree_required(reader, error))
      return FALSE;

    if(!inf_test_text_replay_skip_whitespace_required(reader, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
inf_test_text_replay_play_requests(xmlTextReaderPtr reader,
                                   InfSession* session,
                                   InfConnectionManagerGroup* publisher_group,
                                   InfXmlConnection* publisher,
                                   GError** error)
{
  InfSessionClass* session_class;
  GArray* user_props;
  xmlNodePtr cur;
  InfUser* user;
  guint i;

  session_class = INF_SESSION_GET_CLASS(session);

  while(xmlTextReaderNodeType(reader) == XML_READER_TYPE_ELEMENT)
  {
    cur = inf_test_text_replay_read_current(reader, error);
    if(cur == NULL) return FALSE;

    if(strcmp((const char*)cur->name, "request") == 0)
    {
      inf_connection_manager_group_send_to_group(
        publisher_group,
        NULL,
        xmlCopyNode(cur, 1)
      );

      /* TODO: Check whether that caused an error */
      inf_simulated_connection_flush(INF_SIMULATED_CONNECTION(publisher));
    }
    else if(strcmp((const char*)cur->name, "user") == 0)
    {
      /* User join */
      user_props = session_class->get_xml_user_props(session, publisher, cur);

      user = inf_session_add_user(
        session,
        (const GParameter*)user_props->data,
        user_props->len,
        error
      );

      for(i = 0; i < user_props->len; ++ i)
        g_value_unset(&g_array_index(user_props, GParameter, i).value);

      g_array_free(user_props, TRUE);
      if(user == NULL) return FALSE;
    }
    else
    {
      g_set_error(
        error,
        inf_test_text_replay_error_quark,
        INF_TEST_TEXT_REPLAY_UNEXPECTED_NODE,
        "Unexpected node: '%s'",
	cur->name
      );

      return FALSE;
    }

    if(!inf_test_text_replay_advance_subtree_required(reader, error))
      return FALSE;

    if(!inf_test_text_replay_skip_whitespace_required(reader, error))
      return FALSE;
  }

  return TRUE;
}

static gboolean
inf_test_text_replay_play(xmlTextReaderPtr reader,
                          InfSession* session,
                          InfConnectionManagerGroup* publisher_group,
			  InfXmlConnection* publisher,
			  GError** error)
{
  gboolean result;

  /* Advance to root node */
  if(xmlTextReaderNodeType(reader) != XML_READER_TYPE_ELEMENT)
    if(inf_test_text_replay_advance_required(reader, error) == FALSE)
      return FALSE;

  if(strcmp((const char*)xmlTextReaderConstName(reader),
            "infinote-adopted-session-record") != 0)
  {
    g_set_error(
      error,
      inf_test_text_replay_error_quark,
      INF_TEST_TEXT_REPLAY_UNEXPECTED_NODE,
      "Document is not an infinote session record"
    );

    return FALSE;
  }

  if(!inf_test_text_replay_advance_skip_whitespace_required(reader, error))
    return FALSE;

  if(strcmp((const char*)xmlTextReaderConstName(reader), "initial") != 0)
  {
    g_set_error(
      error,
      inf_test_text_replay_error_quark,
      INF_TEST_TEXT_REPLAY_UNEXPECTED_NODE,
      "Initial document state missing in record"
    );

    return FALSE;
  }

  result = inf_test_text_replay_play_initial(
    reader,
    publisher_group,
    publisher,
    error
  );

  if(!result) return FALSE;

  if(!inf_test_text_replay_skip_whitespace_required(reader, error))
    return FALSE;

  if(xmlTextReaderNodeType(reader) != XML_READER_TYPE_END_ELEMENT)
  {
    g_set_error(
      error,
      inf_test_text_replay_error_quark,
      INF_TEST_TEXT_REPLAY_UNEXPECTED_NODE,
      "Expected ending of initial content"
    );

    return FALSE;
  }

  if(!inf_test_text_replay_advance_skip_whitespace_required(reader, error))
    return FALSE;

  result = inf_test_text_replay_play_requests(
    reader,
    session,
    publisher_group,
    publisher,
    error
  );

  if(!result) return FALSE;

  if(!inf_test_text_replay_skip_whitespace_required(reader, error))
    return FALSE;

  if(xmlTextReaderNodeType(reader) != XML_READER_TYPE_END_ELEMENT)
  {
    g_set_error(
      error,
      inf_test_text_replay_error_quark,
      INF_TEST_TEXT_REPLAY_UNEXPECTED_NODE,
      "Expected end of record"
    );

    return FALSE;
  }

  return TRUE;
}

static void
inf_test_text_replay_process(xmlTextReaderPtr reader)
{
  InfConnectionManager* publisher_manager;
  InfConnectionManager* client_manager;

  InfConnectionManagerGroup* publisher_group;
  InfConnectionManagerGroup* client_group;

  InfSimulatedConnection* publisher;
  InfSimulatedConnection* client;

  InfTextBuffer* buffer;
  InfIo* io;
  InfTextSession* session;
  InfTestTextReplayObject* replay_object;

  GError* error;

  publisher = inf_simulated_connection_new();
  client = inf_simulated_connection_new();
  inf_simulated_connection_connect(publisher, client);

  inf_simulated_connection_set_mode(
    publisher,
    INF_SIMULATED_CONNECTION_DELAYED
  );

  inf_simulated_connection_set_mode(
    client,
    INF_SIMULATED_CONNECTION_DELAYED
  );

  publisher_manager = inf_connection_manager_new();

  publisher_group = inf_connection_manager_open_group(
    publisher_manager,
    "InfAdoptedSessionReplay",
    NULL,
    METHODS
  );

  inf_connection_manager_group_add_connection(
    publisher_group,
    INF_XML_CONNECTION(publisher),
    NULL
  );

  client_manager = inf_connection_manager_new();

  client_group = inf_connection_manager_join_group(
    client_manager,
    "InfAdoptedSessionReplay",
    INF_XML_CONNECTION(client),
    NULL,
    &SIMULATED_CENTRAL_METHOD
  );

  buffer = INF_TEXT_BUFFER(inf_text_default_buffer_new("UTF-8"));
  io = INF_IO(inf_standalone_io_new());

  session = inf_text_session_new(
    client_manager,
    buffer,
    io,
    client_group,
    INF_XML_CONNECTION(client)
  );

  g_object_unref(buffer);
  g_object_unref(io);

  inf_connection_manager_group_set_object(
    client_group,
    INF_NET_OBJECT(session)
  );

  replay_object = g_object_new(INF_TEST_TYPE_TEXT_REPLAY_OBJECT, NULL);
  inf_connection_manager_group_set_object(
    publisher_group,
    INF_NET_OBJECT(replay_object)
  );

  inf_simulated_connection_flush(publisher);
  inf_simulated_connection_flush(client);

  error = NULL;
  inf_test_text_replay_play(
    reader,
    INF_SESSION(session),
    publisher_group,
    INF_XML_CONNECTION(publisher),
    &error
  );

  inf_connection_manager_group_unref(client_group);
  inf_connection_manager_group_unref(publisher_group);

  g_object_unref(replay_object);
  g_object_unref(session);

  g_object_unref(publisher);
  g_object_unref(client);

  g_object_unref(publisher_manager);
  g_object_unref(client_manager);

  if(error != NULL)
  {
    fprintf(
      stderr,
      "Line %d: %s\n",
      xmlTextReaderGetParserLineNumber(reader),
      error->message
    );

    g_error_free(error);
  }
  else
  {
    printf("Replayed record successfully\n");
  }
}

int main(int argc, char* argv[])
{
  xmlTextReaderPtr reader;

  g_type_init();
  inf_test_text_replay_error_quark =
    g_quark_from_static_string("INF_TEST_TEXT_REPLAY_ERROR");

  if(argc < 2)
  {
    fprintf(stderr, "Usage: %s <record-file>\n", argv[0]);
    return -1;
  }

  reader = xmlReaderForFile(
    argv[1],
    NULL,
    XML_PARSE_NOERROR | XML_PARSE_NOWARNING
  );

  if(!reader)
  {
    fprintf(stderr, "%s\n", xmlGetLastError()->message);
    return -1;
  }

  inf_test_text_replay_process(reader);

  if(xmlTextReaderClose(reader) == -1)
  {
    fprintf(stderr, "%s\n", xmlGetLastError()->message);
    return -1;
  }

  xmlFreeTextReader(reader);
  return 0;
}