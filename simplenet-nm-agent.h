/* NetworkManager secret-agent lifecycle, as used by nmtui-connect.
 * Only Wi-Fi is exposed by SimpleNet. NM owns profiles and persistent secrets;
 * this short-lived agent supplies interactive answers for one activation.
 */
typedef struct {
    NMSecretAgentOld parent;
    char *path;
    NMConnection *connection;
    char *request_path;
    char *setting_name;
    char **hints;
    NMSecretAgentOldGetSecretsFunc callback;
    gpointer callback_data;
    guint serial;
    bool user_cancelled;
} SimpleNetAgent;

typedef NMSecretAgentOldClass SimpleNetAgentClass;
G_DEFINE_TYPE(SimpleNetAgent, simplenet_agent, NM_TYPE_SECRET_AGENT_OLD)

static void nm_agent_clear_request(SimpleNetAgent *agent)
{
    g_clear_object(&agent->connection);
    g_clear_pointer(&agent->request_path, g_free);
    g_clear_pointer(&agent->setting_name, g_free);
    g_clear_pointer(&agent->hints, g_strfreev);
    agent->callback = NULL;
    agent->callback_data = NULL;
    agent->serial++;
}

static void nm_agent_reply(SimpleNetAgent *agent, GVariant *secrets,
                           NMSecretAgentError code, const char *message)
{
    NMConnection *connection;
    NMSecretAgentOldGetSecretsFunc callback = agent->callback;
    gpointer data = agent->callback_data;
    GError *error = message
        ? g_error_new_literal(NM_SECRET_AGENT_ERROR, code, message) : NULL;

    if (callback) {
        connection = g_object_ref(agent->connection);
        nm_agent_clear_request(agent);
        callback(NM_SECRET_AGENT_OLD(agent), connection, secrets, error, data);
        g_object_unref(connection);
    }
    g_clear_error(&error);
}

static void nm_agent_get_secrets(NMSecretAgentOld *base,
    NMConnection *connection, const char *path, const char *setting_name,
    const char **hints, NMSecretAgentGetSecretsFlags flags,
    NMSecretAgentOldGetSecretsFunc callback, gpointer data)
{
    SimpleNetAgent *agent = (SimpleNetAgent *)base;
    GError *error = NULL;

    if (!(flags & NM_SECRET_AGENT_GET_SECRETS_FLAG_ALLOW_INTERACTION))
        error = g_error_new_literal(NM_SECRET_AGENT_ERROR,
            NM_SECRET_AGENT_ERROR_NO_SECRETS, "Stored passwords belong to NetworkManager");
    else if (agent->path && g_strcmp0(path, agent->path))
        error = g_error_new_literal(NM_SECRET_AGENT_ERROR,
            NM_SECRET_AGENT_ERROR_NO_SECRETS, "This agent handles another connection");
    else if (agent->connection)
        error = g_error_new_literal(NM_SECRET_AGENT_ERROR,
            NM_SECRET_AGENT_ERROR_FAILED, "An authentication request is already pending");
    if (error) {
        callback(base, connection, NULL, error, data);
        g_error_free(error);
        return;
    }
    agent->connection = g_object_ref(connection);
    agent->request_path = g_strdup(path);
    agent->setting_name = g_strdup(setting_name);
    agent->hints = g_strdupv((char **)hints);
    agent->callback = callback;
    agent->callback_data = data;
    agent->serial++;
}

static void nm_agent_cancel_secrets(NMSecretAgentOld *base,
                                    const char *path, const char *setting)
{
    SimpleNetAgent *agent = (SimpleNetAgent *)base;
    if (!g_strcmp0(agent->request_path, path) &&
        !g_strcmp0(agent->setting_name, setting))
        nm_agent_reply(agent, NULL, NM_SECRET_AGENT_ERROR_AGENT_CANCELED,
                       "NetworkManager cancelled authentication");
}

static void nm_agent_save_secrets(NMSecretAgentOld *agent,
    NMConnection *connection, const char *path,
    NMSecretAgentOldSaveSecretsFunc callback, gpointer data)
{
    (void)path;
    callback(agent, connection, NULL, data);
}

static void nm_agent_delete_secrets(NMSecretAgentOld *agent,
    NMConnection *connection, const char *path,
    NMSecretAgentOldDeleteSecretsFunc callback, gpointer data)
{
    (void)path;
    callback(agent, connection, NULL, data);
}

static void nm_agent_dispose(GObject *object)
{
    SimpleNetAgent *agent = (SimpleNetAgent *)object;
    nm_agent_reply(agent, NULL, NM_SECRET_AGENT_ERROR_AGENT_CANCELED,
                   "Authentication agent closed");
    g_clear_pointer(&agent->path, g_free);
    G_OBJECT_CLASS(simplenet_agent_parent_class)->dispose(object);
}

static void simplenet_agent_class_init(SimpleNetAgentClass *klass)
{
    NMSecretAgentOldClass *base = NM_SECRET_AGENT_OLD_CLASS(klass);
    G_OBJECT_CLASS(klass)->dispose = nm_agent_dispose;
    base->get_secrets = nm_agent_get_secrets;
    base->cancel_get_secrets = nm_agent_cancel_secrets;
    base->save_secrets = nm_agent_save_secrets;
    base->delete_secrets = nm_agent_delete_secrets;
}

static void simplenet_agent_init(SimpleNetAgent *agent) { (void)agent; }

typedef struct {
    NMSetting *setting;
    char property[64];
    bool hidden;
} NmSecretField;

static bool nm_agent_add_field(NmSecretField *fields, int *count,
                               NMSetting *setting, const char *property)
{
    GParamSpec *spec;
    if (!setting || !property || *count >= 16) return false;
    spec = g_object_class_find_property(G_OBJECT_GET_CLASS(setting), property);
    if (!spec || (G_PARAM_SPEC_VALUE_TYPE(spec) != G_TYPE_STRING &&
                  G_PARAM_SPEC_VALUE_TYPE(spec) != G_TYPE_BYTES)) return false;
    fields[*count].setting = setting;
    fields[*count].hidden = (spec->flags & NM_SETTING_PARAM_SECRET) != 0;
    copy_text(fields[*count].property, sizeof(fields[*count].property), property);
    (*count)++;
    return true;
}

/* The Wi-Fi field selection in nmtui's NMSecretAgentSimple: PSK/SAE,
 * indexed WEP keys, LEAP, and the configured 802.1X method or NM hints.
 */
static int nm_agent_fields(NMConnection *connection, const char **hints,
                           NmSecretField *fields)
{
    NMSettingWirelessSecurity *security =
        nm_connection_get_setting_wireless_security(connection);
    NMSetting8021x *eap = nm_connection_get_setting_802_1x(connection);
    const char *key = security
        ? nm_setting_wireless_security_get_key_mgmt(security) : NULL;
    const char *method;
    int count = 0;

    if (!key) return 0;
    if (!strcmp(key, "wpa-psk") || !strcmp(key, "sae")) {
        nm_agent_add_field(fields, &count, NM_SETTING(security), "psk");
    } else if (!strcmp(key, "none")) {
        char property[32];
        snprintf(property, sizeof(property), "wep-key%u",
                 nm_setting_wireless_security_get_wep_tx_keyidx(security));
        nm_agent_add_field(fields, &count, NM_SETTING(security), property);
    } else if (!strcmp(key, "ieee8021x") &&
               !g_strcmp0(nm_setting_wireless_security_get_auth_alg(security), "leap")) {
        nm_agent_add_field(fields, &count, NM_SETTING(security), "leap-password");
    } else if (eap) {
        if (hints && hints[0]) {
            for (int i = 0; hints[i]; i++) {
                GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(eap), hints[i]);
                if (!spec || !(spec->flags & NM_SETTING_PARAM_SECRET)) return 0;
                if (!nm_agent_add_field(fields, &count, NM_SETTING(eap), hints[i]))
                    return 0;
            }
        } else if ((method = nm_setting_802_1x_get_eap_method(eap, 0))) {
            if (!strcmp(method, "md5") || !strcmp(method, "leap") ||
                !strcmp(method, "ttls") || !strcmp(method, "peap")) {
                nm_agent_add_field(fields, &count, NM_SETTING(eap), "identity");
                nm_agent_add_field(fields, &count, NM_SETTING(eap), "password");
            } else if (!strcmp(method, "tls")) {
                nm_agent_add_field(fields, &count, NM_SETTING(eap), "identity");
                nm_agent_add_field(fields, &count, NM_SETTING(eap), "private-key-password");
            }
        }
    }
    return count;
}

typedef struct { SimpleNetAgent *agent; guint serial; } NmPromptRequest;

static bool nm_prompt_cancelled(void *data)
{
    NmPromptRequest *request = data;
    return stop_requested || request->agent->serial != request->serial;
}

static void nm_agent_prompt(SimpleNetAgent *agent)
{
    NmSecretField fields[16] = {0};
    NMConnection *connection;
    GVariantBuilder settings, values;
    GVariant *reply;
    NmPromptRequest request = {agent, agent->serial};
    int count;

    /* New profiles acquire a path in the AddAndActivate completion callback.
     * Wait for that path before accepting any pending secret request.
     */
    if (!agent->path || !agent->connection) return;
    if (strcmp(agent->path, agent->request_path)) {
        nm_agent_reply(agent, NULL, NM_SECRET_AGENT_ERROR_NO_SECRETS,
                       "This agent handles another connection");
        return;
    }
    connection = g_object_ref(agent->connection);
    count = nm_agent_fields(connection, (const char **)agent->hints, fields);
    if (!count) {
        nm_agent_reply(agent, NULL, NM_SECRET_AGENT_ERROR_NO_SECRETS,
                       "Authentication needs a configured Wi-Fi profile; edit it in nmtui");
        g_object_unref(connection);
        return;
    }
    g_variant_builder_init(&settings, NM_VARIANT_TYPE_CONNECTION);
    g_variant_builder_init(&values, NM_VARIANT_TYPE_SETTING);
    for (int i = 0; i < count; i++) {
        char value[1024] = "";
        GParamSpec *spec = g_object_class_find_property(
            G_OBJECT_GET_CLASS(fields[i].setting), fields[i].property);
        bool bytes = G_PARAM_SPEC_VALUE_TYPE(spec) == G_TYPE_BYTES;
        if (!fields[i].hidden && !bytes) {
            char *initial = NULL;
            g_object_get(fields[i].setting, fields[i].property, &initial, NULL);
            copy_text(value, sizeof(value), initial);
            g_free(initial);
        }
        if (!prompt_value(nm_connection_get_id(connection), fields[i].property,
                          fields[i].hidden, value, sizeof(value),
                          nm_prompt_cancelled, &request)) {
            wipe_secret(value, sizeof(value));
            if (agent->serial == request.serial) {
                agent->user_cancelled = true;
                nm_agent_reply(agent, NULL, NM_SECRET_AGENT_ERROR_USER_CANCELED,
                               "Authentication cancelled");
            }
            g_variant_builder_clear(&values);
            g_variant_builder_clear(&settings);
            g_object_unref(connection);
            return;
        }
        g_variant_builder_add(&values, "{sv}", fields[i].property,
            bytes ? g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, value,
                                              strlen(value), sizeof(guint8))
                  : g_variant_new_string(value));
        wipe_secret(value, sizeof(value));
    }
    /* Wi-Fi secret requests contain fields from one setting, including
     * the non-secret 802.1X identity when the authentication method needs it.
     */
    g_variant_builder_add(&settings, "{sa{sv}}",
        nm_setting_get_name(fields[0].setting), &values);
    reply = g_variant_ref_sink(g_variant_builder_end(&settings));
    nm_agent_reply(agent, reply, NM_SECRET_AGENT_ERROR_FAILED, NULL);
    g_variant_unref(reply);
    g_object_unref(connection);
}
