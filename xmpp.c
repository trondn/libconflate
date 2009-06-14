#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <assert.h>

#include "conflate.h"
#include "conflate_internal.h"
#include "conflate_convenience.h"

/* Randomly generated by a fair dice roll */
#define INITIALIZATION_MAGIC 1422850115

/* The private key under which the JID to use is stored */
#define STORED_JID_KEY "stored_jid"

struct _conflate_form_result {
    xmpp_conn_t   *conn;
    xmpp_ctx_t    *ctx;
    xmpp_stanza_t *reply;
    xmpp_stanza_t *cmd_res;
    xmpp_stanza_t *container;
    xmpp_stanza_t *current;
};

struct command_def {
    char *name;
    char *description;
    conflate_mgmt_cb_t cb;
    struct command_def *next;
};

bool commands_initialized = false;
struct command_def *commands = NULL;

void conflate_register_mgmt_cb(const char *cmd, const char *desc,
                               conflate_mgmt_cb_t cb)
{
    struct command_def *c = calloc(1, sizeof(struct command_def));
    assert(c);

    c->name = safe_strdup(cmd);
    c->description = safe_strdup(desc);
    c->cb = cb;
    c->next = commands;

    commands = c;
}

static char *get_form_value(kvpair_t *form, const char *key)
{
    assert(key);

    char *rv = NULL;

    if (form) {
        kvpair_t *valnode = find_kvpair(form, key);
        if (valnode) {
            rv = safe_strdup(valnode->values[0]);
        }
    }

    return rv;
}

static void add_and_release(xmpp_stanza_t* parent, xmpp_stanza_t* child)
{
    xmpp_stanza_add_child(parent, child);
    xmpp_stanza_release(child);
}

/* Copy an attribute from one stanza to another iff it exists in the
   src. */
static void copy_attr(xmpp_ctx_t *ctx,
                      xmpp_stanza_t* src, xmpp_stanza_t* dest,
                      const char* attr)
{
    assert(src);
    assert(dest);
    assert(attr);

    char *val = xmpp_stanza_get_attribute(src, attr);
    if (val) {
        xmpp_stanza_set_attribute(dest, attr, val);
    }
}

/* Create an IQ response */
static xmpp_stanza_t* create_reply(xmpp_ctx_t* ctx, xmpp_stanza_t* stanza)
{
    xmpp_stanza_t* reply = xmpp_stanza_new(ctx);

    assert(reply);

    xmpp_stanza_set_name(reply, "iq");
    xmpp_stanza_set_type(reply, "result");
    if (xmpp_stanza_get_id(stanza)) {
        xmpp_stanza_set_id(reply, xmpp_stanza_get_id(stanza));
    }
    xmpp_stanza_set_attribute(reply, "to",
                              xmpp_stanza_get_attribute(stanza, "from"));

    return reply;
}

/* Create an XMPP command response */
static xmpp_stanza_t* create_cmd_response(xmpp_ctx_t* ctx,
                                          xmpp_stanza_t* cmd_stanza)
{
    xmpp_stanza_t* cmd_res = xmpp_stanza_new(ctx);
    assert(cmd_res);

    xmpp_stanza_set_name(cmd_res, "command");
    copy_attr(ctx, cmd_stanza, cmd_res, "xmlns");
    copy_attr(ctx, cmd_stanza, cmd_res, "node");
    copy_attr(ctx, cmd_stanza, cmd_res, "sessionid");
    xmpp_stanza_set_attribute(cmd_res, "status", "completed");

    return cmd_res;
}

static int version_handler(xmpp_conn_t * const conn,
                           xmpp_stanza_t * const stanza,
                           void * const userdata)
{
    xmpp_stanza_t *reply, *query, *name, *version, *text;
    conflate_handle_t *handle = (conflate_handle_t*) userdata;
    xmpp_ctx_t *ctx = handle->ctx;
    char *ns;

    printf("Received version request from %s\n", xmpp_stanza_get_attribute(stanza, "from"));

    reply = xmpp_stanza_new(ctx);
    assert(reply);
    xmpp_stanza_set_name(reply, "iq");
    xmpp_stanza_set_type(reply, "result");
    xmpp_stanza_set_id(reply, xmpp_stanza_get_id(stanza));
    xmpp_stanza_set_attribute(reply, "to", xmpp_stanza_get_attribute(stanza, "from"));

    query = xmpp_stanza_new(ctx);
    assert(query);
    xmpp_stanza_set_name(query, "query");
    ns = xmpp_stanza_get_ns(xmpp_stanza_get_children(stanza));
    if (ns) {
        xmpp_stanza_set_ns(query, ns);
    }

    name = xmpp_stanza_new(ctx);
    assert(name);
    xmpp_stanza_set_name(name, "name");
    add_and_release(query, name);

    text = xmpp_stanza_new(ctx);
    assert(text);
    xmpp_stanza_set_text(text, handle->conf->software);
    add_and_release(name, text);

    version = xmpp_stanza_new(ctx);
    assert(version);
    xmpp_stanza_set_name(version, "version");
    add_and_release(query, version);

    text = xmpp_stanza_new(ctx);
    assert(text);
    xmpp_stanza_set_text(text, handle->conf->version);
    add_and_release(version, text);

    add_and_release(reply, query);

    xmpp_send(conn, reply);
    xmpp_stanza_release(reply);
    return 1;
}

static char **get_form_values(xmpp_stanza_t *t) {
    xmpp_stanza_t *current = NULL;
    int i = 0, allocated = 8;
    char **rv = calloc(allocated, sizeof(char*));
    assert(rv);

    current = xmpp_stanza_get_child_by_name(t, "value");
    while (current) {
        xmpp_stanza_t *val = xmpp_stanza_get_children(current);
        /* xmpp_stanza_get_children allocates */
        char *v = xmpp_stanza_get_text(val);

        if (i + 1 >= allocated) {
            int new_allocated = allocated << 1;
            rv = realloc(rv, sizeof(char*) * new_allocated);
            assert(rv);
        }

        rv[i++] = v;

        current = xmpp_stanza_get_next(current);
    }

    /* List terminator */
    rv[i] = NULL;

    return rv;
}

static char **get_specific_form_values(xmpp_stanza_t *field, const char *var) {
    char **rv = NULL;

    while (field && strcmp(xmpp_stanza_get_attribute(field, "var"), var) != 0) {
        field = xmpp_stanza_get_next(field);
    }

    if (field) {
        rv = get_form_values(field);
    }

    return rv;
}

static kvpair_t *grok_form(xmpp_stanza_t *fields)
{
    kvpair_t *rv = NULL;

    /* walk the field peers and grab their values and stuff */
    while (fields) {
        if (xmpp_stanza_is_tag(fields)) {
            char* k = xmpp_stanza_get_attribute(fields, "var");
            char **vals = get_specific_form_values(fields, k);
            kvpair_t* thispair = mk_kvpair(k, vals);
            free_string_list(vals);
            thispair->next = rv;
            rv = thispair;
        }
        fields = xmpp_stanza_get_next(fields);
    }

    return rv;
}

static enum conflate_mgmt_cb_result process_serverlist(void *opaque,
                                                       conflate_handle_t *handle,
                                                       const char *cmd,
                                                       bool direct,
                                                       kvpair_t *conf,
                                                       conflate_form_result *r)
{
    /* If we have "config_is_private" set to "yes" we should only
       process this if it's direct (i.e. ignore pubsub) */
    if (!direct) {
        char *priv = conflate_get_private(handle, "config_is_private",
                                          handle->conf->save_path);

        if (priv && strcmp(priv, "yes") == 0) {
            CONFLATE_LOG(handle, INFO,
                         "Currently using a private config, ignoring update.");
            return RV_OK;
        }
        free(priv);
    }

    CONFLATE_LOG(handle, INFO, "Processing a serverlist");

    /* Persist the config lists */
    if (!save_kvpairs(handle, conf, handle->conf->save_path)) {
        CONFLATE_LOG(handle, ERROR, "Can not save config to %s",
                     handle->conf->save_path);
    }

    /* Send the config to the callback */
    handle->conf->new_config(handle->conf->userdata, conf);

    return RV_OK;
}

static void add_form_values(xmpp_ctx_t* ctx, xmpp_stanza_t *parent,
                            const char *key, const char **values)
{
    xmpp_stanza_t* field = xmpp_stanza_new(ctx);
    assert(field);

    xmpp_stanza_set_name(field, "field");
    xmpp_stanza_set_attribute(field, "var", key);
    add_and_release(parent, field);

    for (int i = 0; values[i]; i++) {
        xmpp_stanza_t* value = xmpp_stanza_new(ctx);
        xmpp_stanza_t* text = xmpp_stanza_new(ctx);
        assert(value);
        assert(text);

        xmpp_stanza_set_name(value, "value");
        add_and_release(field, value);

        xmpp_stanza_set_text(text, values[i]);
        add_and_release(value, text);
    }
}

static void add_cmd_error(xmpp_ctx_t *ctx,
                          xmpp_stanza_t * reply, const char *code,
                          const char *ns, const char *name,
                          const char *specificns, const char *specificcond)
{
    xmpp_stanza_set_attribute(reply, "type", "error");

    xmpp_stanza_t *error = xmpp_stanza_new(ctx);
    assert(error);

    xmpp_stanza_set_name(error, "error");
    xmpp_stanza_set_attribute(error, "type", "modify");
    xmpp_stanza_set_attribute(error, "code", code);

    add_and_release(reply, error);

    xmpp_stanza_t *etype = xmpp_stanza_new(ctx);
    assert(etype);

    xmpp_stanza_set_name(etype, name);
    xmpp_stanza_set_attribute(etype, "xmlns", ns);
    add_and_release(error, etype);

    if (specificns && specificcond) {
        xmpp_stanza_t *specific = xmpp_stanza_new(ctx);
        assert(specific);

        xmpp_stanza_set_name(specific, specificcond);
        xmpp_stanza_set_attribute(specific, "xmlns", specificns);

        add_and_release(error, specific);
    }
}

static xmpp_stanza_t* error_unknown_command(const char *cmd,
                                            xmpp_stanza_t * const cmd_stanza,
                                            xmpp_conn_t* const conn,
                                            xmpp_stanza_t * const stanza,
                                            void * const userdata,
                                            bool direct)
{
    conflate_handle_t *handle = (conflate_handle_t*) userdata;
    xmpp_ctx_t *ctx = handle->ctx;

    xmpp_stanza_t* reply = create_reply(ctx, stanza);
    add_and_release(reply, create_cmd_response(ctx, cmd_stanza));
    add_cmd_error(ctx, reply, "404",
                  "urn:ietf:params:xml:ns:xmpp-stanzas", "item-not-found",
                  NULL, NULL);
    return reply;
}

void conflate_init_form(conflate_form_result *r)
{
    if (!r->container) {
        r->container = xmpp_stanza_new(r->ctx);

        /* X data in the command response */
        xmpp_stanza_set_name(r->container, "x");
        xmpp_stanza_set_attribute(r->container, "xmlns", "jabber:x:data");
        xmpp_stanza_set_type(r->container, "result");
        add_and_release(r->cmd_res, r->container);
    }
}

void conflate_add_field_multi(conflate_form_result *r, const char *k,
                              const char **v)
{
    conflate_init_form(r);
    if (!r->current) {
        r->current = r->container;
    }
    if (k) {
        add_form_values(r->ctx, r->current, k, v);
    }
}

void conflate_add_field(conflate_form_result *r, const char *k, const char *v)
{
    const char *vals[2] = { v, NULL };
    conflate_add_field_multi(r, k, vals);
}

static enum conflate_mgmt_cb_result process_stats(void *opaque,
                                                  conflate_handle_t *handle,
                                                  const char *cmd,
                                                  bool direct,
                                                  kvpair_t *form,
                                                  conflate_form_result *r)
{
    /* Only direct stat requests are handled. */
    assert(direct);

    char *subtype = NULL;
    kvpair_t *valnode = find_kvpair(form, "-subtype-");
    if (valnode) {
        subtype = valnode->values[0];
    }

    CONFLATE_LOG(handle, DEBUG, "Handling stats request with subtype:  %s",
                 subtype ? subtype : "(null)");

    handle->conf->get_stats(handle->conf->userdata, r,
                            subtype, form);

    return RV_OK;
}

static enum conflate_mgmt_cb_result process_reset_stats(void *opaque,
                                                        conflate_handle_t *handle,
                                                        const char *cmd,
                                                        bool direct,
                                                        kvpair_t *form,
                                                        conflate_form_result *r)
{
    char *subtype = get_form_value(form, "-subtype-");

    CONFLATE_LOG(handle, DEBUG, "Handling stats reset with subtype:  %s",
                 subtype ? subtype : "(null)");

    handle->conf->reset_stats(handle->conf->userdata, subtype, form);

    free(subtype);
    return RV_OK;
}

void conflate_next_fieldset(conflate_form_result *r)
{
        conflate_init_form(r);
        /* Fail if some k/v pairs were already added */
        assert(r->current == NULL || r->current != r->container);

        r->current = xmpp_stanza_new(r->ctx);
        assert(r->current);

        xmpp_stanza_set_name(r->current, "item");
        add_and_release(r->container, r->current);
}

static enum conflate_mgmt_cb_result process_ping_test(void *opaque,
                                                      conflate_handle_t *handle,
                                                      const char *cmd,
                                                      bool direct,
                                                      kvpair_t *form,
                                                      conflate_form_result *r)
{
    handle->conf->ping_test(handle->conf->userdata, r, form);
    return RV_OK;
}

static enum conflate_mgmt_cb_result process_set_private(void *opaque,
                                                        conflate_handle_t *handle,
                                                        const char *cmd,
                                                        bool direct,
                                                        kvpair_t *form,
                                                        conflate_form_result *r)
{
    /* Only direct stat requests are handled. */
    assert(direct);
    enum conflate_mgmt_cb_result rv = RV_ERROR;

    char *key = get_form_value(form, "key");
    char *value = get_form_value(form, "value");

    if (key && value) {
        if (conflate_save_private(handle, key, value,
                                  handle->conf->save_path)) {
            rv = RV_OK;
        }
    } else {
        rv = RV_BADARG;
    }

    free(key);
    free(value);

    return rv;
}

static enum conflate_mgmt_cb_result process_get_private(void *opaque,
                                                        conflate_handle_t *handle,
                                                        const char *cmd,
                                                        bool direct,
                                                        kvpair_t *form,
                                                        conflate_form_result *r)
{
    /* Only direct stat requests are handled. */
    assert(direct);
    enum conflate_mgmt_cb_result rv = RV_ERROR;

    char *key = get_form_value(form, "key");

    if (key) {
        /* Initialize the form so there's always one there */
        conflate_init_form(r);
        char *value = conflate_get_private(handle, key,
                                           handle->conf->save_path);
        if (value) {
            conflate_add_field(r, key, value);
            free(value);
        }

        rv = RV_OK;
    } else {
        rv = RV_BADARG;
    }

    free(key);

    return rv;
}

static enum conflate_mgmt_cb_result process_delete_private(void *opaque,
                                                           conflate_handle_t *handle,
                                                           const char *cmd,
                                                           bool direct,
                                                           kvpair_t *form,
                                                           conflate_form_result *r)
{
    /* Only direct stat requests are handled. */
    assert(direct);
    enum conflate_mgmt_cb_result rv = RV_ERROR;

    char *key = get_form_value(form, "key");

    if (key) {
        if (conflate_delete_private(handle, key,
                                    handle->conf->save_path)) {
            rv = RV_OK;
        }
    } else {
        rv = RV_BADARG;
    }

    free(key);

    return rv;
}

static char* cb_name(enum conflate_mgmt_cb_result r)
{
    char *rv = "UNKNOWN";
    switch(r) {
    case RV_OK:     rv = "RV_OK";     break;
    case RV_ERROR:  rv = "RV_ERROR";  break;
    case RV_BADARG: rv = "RV_BADARG"; break;
    }
    return rv;
}

static xmpp_stanza_t* n_handler(const char *cmd,
                                xmpp_stanza_t* cmd_stanza,
                                xmpp_conn_t * const conn,
                                xmpp_stanza_t * const stanza,
                                void * const userdata,
                                bool direct,
                                conflate_mgmt_cb_t cb)
{
    conflate_handle_t *handle = (conflate_handle_t*) userdata;
    xmpp_ctx_t *ctx = handle->ctx;
    conflate_form_result result = { .conn = conn,
                                    .ctx = ctx,
                                    .reply = NULL,
                                    .cmd_res = NULL,
                                    .container = NULL };
    kvpair_t *form = NULL;

    assert(cb);

    result.reply = create_reply(ctx, stanza);
    result.cmd_res = create_cmd_response(ctx, cmd_stanza);

    add_and_release(result.reply, result.cmd_res);

    xmpp_stanza_t *x = xmpp_stanza_get_child_by_name(cmd_stanza, "x");
    if (x) {
        xmpp_stanza_t *fields = xmpp_stanza_get_child_by_name(x, "field");
        if (fields) {
            form = grok_form(fields);
        }
    }

    enum conflate_mgmt_cb_result rv = cb(handle->conf->userdata, handle,
                                         cmd, direct, form, &result);

    CONFLATE_LOG(handle, DEBUG, "Result of %s:  %s", cmd, cb_name(rv));

    switch (rv) {
    case RV_ERROR:
        add_cmd_error(ctx, result.reply, "500",
                      "urn:ietf:params:xml:ns:xmpp-stanzas",
                      "internal-server-error", NULL, NULL);
        break;
    case RV_BADARG:
        add_cmd_error(ctx, result.reply, "400",
                      "urn:ietf:params:xml:ns:xmpp-stanzas", "bad-request",
                      "http://jabber.org/protocol/commands", "bad-payload");
        break;
    case RV_OK:
        /* Things are good, use the built form */
        break;
    }

    free_kvpair(form);

    return result.reply;
}

static xmpp_stanza_t* command_dispatch(xmpp_conn_t * const conn,
                                       xmpp_stanza_t * const stanza,
                                       void * const userdata,
                                       const char* cmd,
                                       xmpp_stanza_t *req,
                                       bool direct)
{
    conflate_handle_t *handle = (conflate_handle_t*) userdata;

    conflate_mgmt_cb_t cb = NULL;
    struct command_def *p = commands;

    for (p = commands; p && !cb; p = p->next) {
        if (strcmp(cmd, p->name) == 0) {
            cb = p->cb;
        }
    }

    if (cb) {
        return n_handler(cmd, req, conn, stanza, handle, direct, cb);
    } else {
        return error_unknown_command(cmd, req, conn, stanza, handle, direct);
    }
}

static int command_handler(xmpp_conn_t * const conn,
                           xmpp_stanza_t * const stanza,
                           void * const userdata)
{
    xmpp_stanza_t *reply = NULL, *req = NULL;
    char *cmd = NULL;

    /* Figure out what the command is */
    req = xmpp_stanza_get_child_by_name(stanza, "command");
    assert(req);
    assert(strcmp(xmpp_stanza_get_name(req), "command") == 0);
    cmd = xmpp_stanza_get_attribute(req, "node");
    assert(cmd);

    CONFLATE_LOG(((conflate_handle_t *)userdata), INFO, "Command:  %s", cmd);

    reply = command_dispatch(conn, stanza, userdata, cmd, req, true);

    if (reply) {
        xmpp_send(conn, reply);
        xmpp_stanza_release(reply);
    }

    return 1;
}

static int keepalive_handler(xmpp_conn_t * const conn, void * const userdata)
{
    xmpp_send_raw(conn, " ", 1);
    return 1;
}

static void add_disco_item(xmpp_ctx_t* ctx, xmpp_stanza_t* query,
                           const char* jid, char* node, char* name)
{
    xmpp_stanza_t* item = xmpp_stanza_new(ctx);
    assert(item);
    assert(ctx);
    assert(query);
    assert(jid);
    assert(node);
    assert(name);

    xmpp_stanza_set_name(item, "item");
    xmpp_stanza_set_attribute(item, "jid", jid);
    xmpp_stanza_set_attribute(item, "node", node);
    xmpp_stanza_set_attribute(item, "name", name);

    add_and_release(query, item);
}

static int disco_items_handler(xmpp_conn_t * const conn,
                               xmpp_stanza_t * const stanza,
                               void * const userdata)
{
    xmpp_stanza_t *reply, *query;
    const char* myjid = xmpp_conn_get_bound_jid(conn);
    conflate_handle_t *handle = (conflate_handle_t*) userdata;

    assert(conn);
    assert(myjid);
    assert(stanza);
    assert(userdata);

    reply = xmpp_stanza_new(handle->ctx);
    assert(reply);
    xmpp_stanza_set_name(reply, "iq");
    xmpp_stanza_set_type(reply, "result");
    xmpp_stanza_set_id(reply, xmpp_stanza_get_id(stanza));
    xmpp_stanza_set_attribute(reply, "to",
                              xmpp_stanza_get_attribute(stanza, "from"));
    xmpp_stanza_set_attribute(reply, "from", myjid);

    query = xmpp_stanza_new(handle->ctx);
    assert(query);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_attribute(query, "xmlns", XMPP_NS_DISCO_ITEMS);
    xmpp_stanza_set_attribute(query, "node", "http://jabber.org/protocol/commands");
    add_and_release(reply, query);

    for (struct command_def *p = commands; p; p = p->next) {
        add_disco_item(handle->ctx, query, myjid, p->name, p->description);
    }

    xmpp_send(conn, reply);
    xmpp_stanza_release(reply);

    return 1;
}

static int message_handler(xmpp_conn_t * const conn,
                           xmpp_stanza_t * const stanza,
                           void * const userdata)
{
    xmpp_stanza_t* event = NULL, *items = NULL, *item = NULL,
        *command = NULL, *reply = NULL;
    conflate_handle_t *handle = (conflate_handle_t*)userdata;
    CONFLATE_LOG(handle, DEBUG, "Got a message from %s",
                 xmpp_stanza_get_attribute(stanza, "from"));

    event = xmpp_stanza_get_child_by_name(stanza, "event");
    assert(event);
    items = xmpp_stanza_get_child_by_name(event, "items");
    assert(items);
    item = xmpp_stanza_get_child_by_name(items, "item");

    if (item) {
        command = xmpp_stanza_get_child_by_name(item, "command");
        assert(command);

        CONFLATE_LOG(handle, INFO, "Pubsub comand:  %s",
                     xmpp_stanza_get_attribute(command, "command"));

        reply = command_dispatch(conn, stanza, userdata,
                                 xmpp_stanza_get_attribute(command, "command"),
                                 command, false);

        if (reply) {
            xmpp_stanza_release(reply);
        }
    } else {
        CONFLATE_LOG(handle, INFO, "Received pubsub event with no items.");
    }

    return 1;
}

static void conn_handler(xmpp_conn_t * const conn, const xmpp_conn_event_t status,
                         const int error, xmpp_stream_error_t * const stream_error,
                         void * const userdata)
{
    conflate_handle_t *handle = (conflate_handle_t *)userdata;

    if (status == XMPP_CONN_CONNECT) {
        xmpp_stanza_t* pres = NULL, *priority = NULL, *pri_text = NULL;
        CONFLATE_LOG(handle, INFO, "Connected.");
        xmpp_handler_add(conn, version_handler, "jabber:iq:version", "iq", NULL, handle);
        xmpp_handler_add(conn, command_handler, "http://jabber.org/protocol/commands",
                         "iq", NULL, handle);
        xmpp_handler_add(conn, disco_items_handler,
                         "http://jabber.org/protocol/disco#items", "iq", NULL, handle);
        xmpp_handler_add(conn, message_handler, NULL, "message", NULL, handle);
        xmpp_timed_handler_add(conn, keepalive_handler, 60000, handle);

        /* Send initial <presence/> so that we appear online to contacts */
        pres = xmpp_stanza_new(handle->ctx);
        assert(pres);
        xmpp_stanza_set_name(pres, "presence");

        priority = xmpp_stanza_new(handle->ctx);
        assert(priority);
        xmpp_stanza_set_name(priority, "priority");
        add_and_release(pres, priority);

        pri_text = xmpp_stanza_new(handle->ctx);
        assert(pri_text);
        xmpp_stanza_set_text(pri_text, "5");
        add_and_release(priority, pri_text);

        xmpp_send(conn, pres);
        xmpp_stanza_release(pres);

        /* Store the bound jid */
        if (!conflate_save_private(handle, STORED_JID_KEY,
                                   xmpp_conn_get_bound_jid(conn),
                                   handle->conf->save_path)) {
            CONFLATE_LOG(handle, WARN, "Failed to save the bound jid");
        }
    }
    else {
        CONFLATE_LOG(handle, INFO, "disconnected.");
        xmpp_stop(handle->ctx);
    }
}

static void conflate_strophe_logger(void *const userdata,
                                    const xmpp_log_level_t level,
                                    const char *const area,
                                    const char *const msg)
{
    enum conflate_log_level lvl = ERROR;
    switch(level) {
    case XMPP_LEVEL_DEBUG: lvl = DEBUG; break;
    case XMPP_LEVEL_INFO: lvl = INFO; break;
    case XMPP_LEVEL_WARN: lvl = WARN; break;
    case XMPP_LEVEL_ERROR: lvl = ERROR; break;
    }

    conflate_handle_t *handle = (conflate_handle_t*)userdata;
    CONFLATE_LOG(handle, lvl, "%s", msg);
}

static void* run_conflate(void *arg) {
    conflate_handle_t* handle = (conflate_handle_t*)arg;

    /* Before connecting and all that, load the stored config */
    kvpair_t* conf = load_kvpairs(handle, handle->conf->save_path);
    if (conf) {
        handle->conf->new_config(handle->conf->userdata, conf);
        free_kvpair(conf);
    }

    xmpp_log_t strophe_logger = { &conflate_strophe_logger, handle };

    /* Run forever */
    for (;;) {
        handle->ctx = xmpp_ctx_new(NULL, &strophe_logger);
        assert(handle->ctx);

        handle->conn = xmpp_conn_new(handle->ctx);
        assert(handle->conn);

        /* Use the stored jid if there is one */
        char *db_jid = conflate_get_private(handle, STORED_JID_KEY,
                                            handle->conf->save_path);
        if (db_jid) {
            CONFLATE_LOG(handle, DEBUG, "Using jid from db: %s", db_jid);
            xmpp_conn_set_jid(handle->conn, db_jid);
            free(db_jid);
        } else {
            CONFLATE_LOG(handle, DEBUG, "Using provided jid:  %s",
                         handle->conf->jid);
            xmpp_conn_set_jid(handle->conn, handle->conf->jid);
        }

        xmpp_conn_set_pass(handle->conn, handle->conf->pass);

        xmpp_connect_client(handle->conn, handle->conf->host, 0,
                            conn_handler, handle);
        xmpp_run(handle->ctx);
        CONFLATE_LOG(handle, INFO, "xmpp_run exited");

        xmpp_conn_release(handle->conn);
        xmpp_ctx_free(handle->ctx);

        sleep(5);
        CONFLATE_LOG(handle, INFO, "reconnecting");
    }
    CONFLATE_LOG(handle, FATAL, "Exited an infinite loop.");
    return NULL;
}

conflate_config_t* dup_conf(conflate_config_t c) {
    conflate_config_t *rv = calloc(sizeof(conflate_config_t), 1);
    assert(rv);

    rv->jid = safe_strdup(c.jid);
    rv->pass = safe_strdup(c.pass);
    if (c.host) {
        rv->host = safe_strdup(c.host);
    }
    rv->software = safe_strdup(c.software);
    rv->version = safe_strdup(c.version);
    rv->save_path = safe_strdup(c.save_path);
    rv->userdata = c.userdata;
    rv->log = c.log;
    rv->new_config = c.new_config;
    rv->get_stats = c.get_stats;
    rv->reset_stats = c.reset_stats;
    rv->ping_test = c.ping_test;

    rv->initialization_marker = (void*)INITIALIZATION_MAGIC;

    return rv;
}

static char* lvl_name(enum conflate_log_level lvl)
{
    char *rv = NULL;

    switch(lvl) {
    case FATAL: rv = "FATAL"; break;
    case ERROR: rv = "ERROR"; break;
    case WARN: rv = "WARN"; break;
    case INFO: rv = "INFO"; break;
    case DEBUG: rv = "DEBUG"; break;
    }

    return rv;
}

static void default_logger(void *userdata, enum conflate_log_level lvl,
                           const char *msg, ...)
{
    char fmt[strlen(msg) + 16];
    snprintf(fmt, sizeof(fmt), "%s: %s\n", lvl_name(lvl), msg);

    va_list ap;
    va_start(ap, msg);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void init_commands(void)
{
    if (commands_initialized) {
        return;
    }

    conflate_register_mgmt_cb("set_private",
                              "Set a private value on the agent.",
                              process_set_private);
    conflate_register_mgmt_cb("get_private",
                              "Get a private value from the agent.",
                              process_get_private);
    conflate_register_mgmt_cb("rm_private",
                              "Delete a private value from the agent.",
                              process_delete_private);

    conflate_register_mgmt_cb("client_stats", "Retrieves stats from the agent",
                              process_stats);
    conflate_register_mgmt_cb("reset_stats", "Reset stats on the agent",
                              process_reset_stats);

    conflate_register_mgmt_cb("ping_test", "Perform a ping test",
                              process_ping_test);

    conflate_register_mgmt_cb("serverlist", "Configure a server list.",
                              process_serverlist);

    commands_initialized = true;
}

void init_conflate(conflate_config_t *conf)
{
    assert(conf);
    memset(conf, 0x00, sizeof(conflate_config_t));
    conf->log = default_logger;
    conf->initialization_marker = (void*)INITIALIZATION_MAGIC;

    init_commands();
}

bool start_conflate(conflate_config_t conf) {
    /* Don't start if we don't believe initialization has occurred. */
    if (conf.initialization_marker != (void*)INITIALIZATION_MAGIC) {
        assert(conf.initialization_marker == (void*)INITIALIZATION_MAGIC);
        return false;
    }

    conflate_handle_t *handle = calloc(1, sizeof(conflate_handle_t));
    assert(handle);

    xmpp_initialize();

    handle->conf = dup_conf(conf);

    if (pthread_create(&handle->thread, NULL, run_conflate, handle) == 0) {
        return true;
    } else {
        perror("pthread_create");
    }

    return false;
}
