/*
 * smart_url - Profanity plugin that annotates URLs in messages with short codes
 * and provides /surl open|save|copy commands to operate on URLs by code.
 */

#include <glib.h>
#include <strophe.h>
#include <profapi.h>

#ifdef HAS_CLIPBOARD
#include <gtk/gtk.h>
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

#define DEFAULT_MAXLEN 80
#define ELLIPSIS "…"
#define SETTINGS_GROUP "smart_url"
#define DEFAULT_ALPHABET "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"

/* ------------------------------------------------------------------ */
/*  Data                                                              */
/* ------------------------------------------------------------------ */

static GHashTable *code_to_url = NULL;
static GRegex *url_regex = NULL;
static unsigned long next_code_index = 0;
static gboolean shorten_enabled = TRUE;
static gint max_display_len = DEFAULT_MAXLEN;
static char *alphabet = NULL;          /* working copy (shuffled) */
static char *alphabet_orig = NULL;     /* original (saved to settings) */
static int alphabet_len = 0;
static xmpp_ctx_t *strophe_ctx = NULL;
static char *my_domain = NULL;         /* domain part of our own JID */

/* ------------------------------------------------------------------ */
/*  Forward declarations                                              */
/* ------------------------------------------------------------------ */

static char *generate_code(unsigned long index);
static char *register_url(const char *url);
static gchar *shorten_url(const char *url);
static char *rewrite_urls(const char *message, const char *sender_domain);
static gboolean url_replace_cb(const GMatchInfo *match_info, GString *result, gpointer user_data);
static gboolean clipboard_copy(const char *text);
static gboolean validate_alphabet(const char *alpha);
static void surl_command_cb(char **args);

/* ------------------------------------------------------------------ */
/*  Domain helpers                                                    */
/* ------------------------------------------------------------------ */

static gboolean is_subdomain(const char *host, const char *domain) {
    /* Returns TRUE if host equals domain or is a subdomain of domain.
     * Case-insensitive comparison.
     * E.g. is_subdomain("upload.example.org", "example.org") → TRUE
     *      is_subdomain("example.org", "example.org") → TRUE
     *      is_subdomain("other.org", "example.org") → FALSE
     */
    if (!host || !domain) return FALSE;
    if (g_ascii_strcasecmp(host, domain) == 0) return TRUE;
    gsize hlen = strlen(host);
    gsize dlen = strlen(domain);
    if (hlen <= dlen) return FALSE;
    /* host is longer — check it ends with ".domain" */
    return host[hlen - dlen - 1] == '.' &&
           g_ascii_strcasecmp(host + hlen - dlen, domain) == 0;
}

static const char *get_url_emoji(const char *url, const char *sender_domain) {
    /* Determine emoji for a URL:
     *  📤 if this is an aesgcm:// URL (XEP-0363 upload)
     *  📤 if host is (sub)domain of our own JID (likely own server upload)
     *  📤 if host is (sub)domain of sender's JID (likely peer's server upload)
     *  🔗 otherwise (external link)
     *
     * sender_domain may be NULL (e.g. MUC where real JID unavailable).
     */
    g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    if (!uri) return "🔗";

    const char *host = g_uri_get_host(uri);
    if (!host) return "🔗";

    const char *scheme = g_uri_get_scheme(uri);
    /* aesgcm:// is always an XEP-0363 upload */
    if (g_str_equal(scheme, "aesgcm"))
        return "📤";

    /* Heuristic: subdomain of our own JID domain */
    if (my_domain && is_subdomain(host, my_domain))
        return "📤";

    /* Heuristic: subdomain of the sender's JID domain */
    if (sender_domain && is_subdomain(host, sender_domain))
        return "📤";

    return "🔗";
}

/* ------------------------------------------------------------------ */
/*  Alphabet shuffle                                                   */
/* ------------------------------------------------------------------ */

static void shuffle_alphabet(void) {
    /* Fisher-Yates shuffle — produces a random permutation each time */
    for (int i = alphabet_len - 1; i > 0; i--) {
        int j = g_random_int_range(0, i + 1);
        char tmp = alphabet[i];
        alphabet[i] = alphabet[j];
        alphabet[j] = tmp;
    }
}

/* ------------------------------------------------------------------ */
/*  Settings                                                          */
/* ------------------------------------------------------------------ */

static void settings_load(void) {
    shorten_enabled = prof_settings_boolean_get(SETTINGS_GROUP, "shorten", TRUE);
    max_display_len = prof_settings_int_get(SETTINGS_GROUP, "maxlen", DEFAULT_MAXLEN);
    if (max_display_len < 10) max_display_len = DEFAULT_MAXLEN;

    g_autofree char *saved_alpha = prof_settings_string_get(SETTINGS_GROUP, "alphabet", DEFAULT_ALPHABET);
    g_free(alphabet_orig);
    alphabet_orig = validate_alphabet(saved_alpha) ? g_strdup(saved_alpha) : g_strdup(DEFAULT_ALPHABET);

    /* Working copy: shuffled randomly each session */
    g_free(alphabet);
    alphabet = g_strdup(alphabet_orig);
    alphabet_len = (int)strlen(alphabet);
    shuffle_alphabet();
}

static void settings_save(void) {
    prof_settings_boolean_set(SETTINGS_GROUP, "shorten", shorten_enabled);
    prof_settings_int_set(SETTINGS_GROUP, "maxlen", max_display_len);
    /* Save the original (un-shuffled) alphabet for settings persistence */
    prof_settings_string_set(SETTINGS_GROUP, "alphabet", alphabet_orig);
}

/* ------------------------------------------------------------------ */
/*  Code generation (sqids-style)                                     */
/* ------------------------------------------------------------------ */

static char *generate_code(unsigned long index) {
    /* Sqids-inspired encoding: compute an offset from the index, rotate & reverse
     * the alphabet, then encode with a prefix + base-(len-1) digits.
     * This makes consecutive indices produce very different-looking codes. */

    int len = alphabet_len;
    int base = len - 1;

    /* Compute offset from the number (deterministic, like sqids) */
    unsigned long offset = ((unsigned long)alphabet[index % len] + index + 1) % len;

    /* Rotate working alphabet by offset */
    char work[len + 1];
    int rot = (int)offset;
    memcpy(work, alphabet + rot, len - rot);
    memcpy(work + len - rot, alphabet, rot);
    work[len] = '\0';

    /* Reverse working alphabet */
    for (int j = 0; j < len / 2; j++) {
        char tmp = work[j];
        work[j] = work[len - 1 - j];
        work[len - 1 - j] = tmp;
    }

    /* Prefix character identifies the offset (enables unambiguous codes) */
    char prefix = alphabet[rot];

    /* Encode index in base (len-1), using work[1..len-1] as digit chars.
     * work[0] is reserved as separator (unused for single-number encoding). */
    unsigned long n = index;
    int digits = 0;
    unsigned long tmp_n = n;
    do { digits++; tmp_n /= base; } while (tmp_n);

    char *code = g_malloc(1 + digits + 1); /* prefix + digits + null */
    code[0] = prefix;

    for (int i = digits; i >= 1; i--) {
        code[i] = work[1 + (n % base)];
        n /= base;
    }
    code[1 + digits] = '\0';

    return code;
}

static char *register_url(const char *url) {
    g_autofree char *code = generate_code(next_code_index++);

    g_hash_table_insert(code_to_url, g_strdup(code), g_strdup(url));

    char *items[] = { code, NULL };
    prof_completer_add("/surl open", items);
    prof_completer_add("/surl save", items);
    prof_completer_add("/surl copy", items);
    prof_completer_add("/suo", items);
    prof_completer_add("/sus", items);
    prof_completer_add("/suc", items);

    return g_steal_pointer(&code);
}

/* ------------------------------------------------------------------ */
/*  Alphabet validation                                               */
/* ------------------------------------------------------------------ */

static gboolean validate_alphabet(const char *alpha) {
    if (!alpha) return FALSE;
    gsize len = strlen(alpha);
    if (len < 3) return FALSE;
    /* All characters must be printable ASCII (no spaces or control chars) */
    for (gsize i = 0; i < len; i++)
        if (!g_ascii_isgraph(alpha[i])) return FALSE;
    /* Check all characters are unique */
    for (gsize i = 0; i < len; i++)
        for (gsize j = i + 1; j < len; j++)
            if (alpha[i] == alpha[j]) return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  URL shortening                                                    */
/* ------------------------------------------------------------------ */

static gchar *shorten_url(const char *url) {
    if (strlen(url) <= (gsize)max_display_len)
        return g_strdup(url);

    g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    if (!uri)
        return g_strdup_printf("%.*s%s",
                               (int)(max_display_len - strlen(ELLIPSIS)),
                               url, ELLIPSIS);

    const gchar *host = g_uri_get_host(uri);
    const gchar *path = g_uri_get_path(uri);

    const gchar *filename = strrchr(path, '/');
    filename = filename ? filename + 1 : path;

    GString *display = g_string_new(host);

    if (*filename) {
        g_string_append_printf(display, "/" ELLIPSIS "/%s", filename);
    } else if (path && !g_str_equal(path, "/") && strlen(path) > 1) {
        g_string_append(display, "/" ELLIPSIS);
    }

    if (display->len > (gsize)max_display_len) {
        gsize target = max_display_len - strlen(ELLIPSIS);
        if (target > display->len) target = display->len;
        g_string_truncate(display, target);
        g_string_append(display, ELLIPSIS);
    }

    return g_string_free(display, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Clipboard                                                         */
/* ------------------------------------------------------------------ */

#ifdef HAS_CLIPBOARD

static gboolean clipboard_copy(const char *text) {
    GdkDisplay *display = gdk_display_get_default();
    if (!display) display = gdk_display_open(NULL);
    if (!display) {
        prof_cons_show("Smart URL: cannot connect to display, clipboard unavailable");
        return FALSE;
    }

    GtkClipboard *cb = gtk_clipboard_get_for_display(display, GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, text, -1);
    gtk_clipboard_store(cb);
    return TRUE;
}

#else /* !HAS_CLIPBOARD */

static gboolean clipboard_copy(const char *text) {
    (void)text;
    prof_cons_show("Smart URL: clipboard support not compiled in");
    prof_cons_show("Rebuild without WITHOUT_CLIPBOARD=1 to enable /surl copy");
    return FALSE;
}

#endif /* HAS_CLIPBOARD */

/* ------------------------------------------------------------------ */
/*  URL rewriting                                                     */
/* ------------------------------------------------------------------ */

static gboolean url_replace_cb(const GMatchInfo *match_info, GString *result,
                               gpointer user_data) {
    const char *sender_domain = (const char *)user_data;
    g_autofree gchar *url = g_match_info_fetch(match_info, 0);
    g_autofree char *code = register_url(url);
    g_autofree gchar *display = shorten_url(url);
    const char *emoji = get_url_emoji(url, sender_domain);

    g_string_append_printf(result, "%s[%s]%s", emoji, code, display);

    return FALSE;
}

static char *rewrite_urls(const char *message, const char *sender_domain) {
    if (!message || !shorten_enabled) return NULL;
    if (!g_regex_match(url_regex, message, 0, NULL)) return NULL;

    g_autofree gchar *rewritten = g_regex_replace_eval(url_regex, message, -1, 0,
                                                        0, url_replace_cb, (gpointer)sender_domain, NULL);
    if (!rewritten || g_str_equal(rewritten, message)) return NULL;

    return g_steal_pointer(&rewritten);
}

/* ------------------------------------------------------------------ */
/*  Pre-display hooks                                                 */
/* ------------------------------------------------------------------ */

static char *rewrite_message(const char *barejid, const char *message, gboolean use_sender) {
    char *sender_domain = (use_sender && barejid) ? xmpp_jid_domain(strophe_ctx, barejid) : NULL;
    char *ret = rewrite_urls(message, sender_domain);
    if (sender_domain) xmpp_free(strophe_ctx, sender_domain);
    return ret;
}

char *prof_pre_chat_message_display(const char *const barejid,
                                    G_GNUC_UNUSED const char *const resource,
                                    const char *message) {
    return rewrite_message(barejid, message, TRUE);
}

char *prof_pre_room_message_display(G_GNUC_UNUSED const char *const barejid,
                                    G_GNUC_UNUSED const char *const nick,
                                    const char *message) {
    /* MUC: real sender JID not available, only check own domain and aesgcm */
    return rewrite_message(barejid, message, FALSE);
}

char *prof_pre_priv_message_display(const char *const barejid,
                                    G_GNUC_UNUSED const char *const nick,
                                    const char *message) {
    return rewrite_message(barejid, message, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Action helpers                                                    */
/* ------------------------------------------------------------------ */

static void code_not_found(const char *code) {
    g_autofree char *msg = g_strdup_printf("Smart URL: unknown code \"%s\"", code);
    prof_cons_show(msg);
}

static void do_open(const char *code) {
    const char *url = g_hash_table_lookup(code_to_url, code);
    if (!url) { code_not_found(code); return; }
    g_autofree char *cmd = g_strdup_printf("/url open %s", url);
    prof_send_line(cmd);
}

static void do_save(const char *code) {
    const char *url = g_hash_table_lookup(code_to_url, code);
    if (!url) { code_not_found(code); return; }
    g_autofree char *cmd = g_strdup_printf("/url save %s", url);
    prof_send_line(cmd);
}

static void do_copy(const char *code) {
    const char *url = g_hash_table_lookup(code_to_url, code);
    if (!url) { code_not_found(code); return; }
    if (clipboard_copy(url)) {
        g_autofree char *msg = g_strdup_printf("Smart URL: copied %s", code);
        prof_cons_show(msg);
    }
}

/* ------------------------------------------------------------------ */
/*  /surl sub-handlers                                                */
/* ------------------------------------------------------------------ */

static void cmd_handle_open(char **args) {
    if (!args || !args[0]) { prof_cons_show("Usage: /surl open <code>"); return; }
    do_open(args[0]);
}

static void cmd_handle_save(char **args) {
    if (!args || !args[0]) { prof_cons_show("Usage: /surl save <code>"); return; }
    do_save(args[0]);
}

static void cmd_handle_copy(char **args) {
    if (!args || !args[0]) { prof_cons_show("Usage: /surl copy <code>"); return; }
    do_copy(args[0]);
}

static void cmd_handle_short(char **args) {
    if (!args || !args[0]) {
        g_autofree char *msg = g_strdup_printf("URL shortening is %s",
                                               shorten_enabled ? "on" : "off");
        prof_cons_show(msg);
        return;
    }
    if (g_str_equal(args[0], "on")) {
        shorten_enabled = TRUE;
        settings_save();
        prof_cons_show("Smart URL: shortening enabled");
    } else if (g_str_equal(args[0], "off")) {
        shorten_enabled = FALSE;
        settings_save();
        prof_cons_show("Smart URL: shortening disabled — URLs pass through unchanged");
    } else {
        prof_cons_show("Usage: /surl short on|off");
    }
}

static void cmd_handle_maxlen(char **args) {
    if (!args || !args[0]) {
        g_autofree char *msg = g_strdup_printf("Smart URL: maxlen is %d", max_display_len);
        prof_cons_show(msg);
        return;
    }
    gchar *endptr = NULL;
    gint64 val = g_ascii_strtoll(args[0], &endptr, 10);
    if (endptr == args[0] || *endptr != '\0' || val < 10 || val > 1000) {
        prof_cons_show("Smart URL: maxlen must be 10–1000");
        return;
    }
    max_display_len = (gint)val;
    settings_save();
    g_autofree char *msg = g_strdup_printf("Smart URL: maxlen set to %d", max_display_len);
    prof_cons_show(msg);
}

static void cmd_handle_alphabet(char **args) {
    if (!args || !args[0]) {
        g_autofree char *msg = g_strdup_printf("Smart URL: alphabet is \"%s\" (%d chars)", alphabet_orig, alphabet_len);
        prof_cons_show(msg);
        return;
    }
    if (!validate_alphabet(args[0])) {
        gsize len = args[0] ? strlen(args[0]) : 0;
        g_autofree char *msg = g_strdup_printf(
            "Smart URL: alphabet must have at least 3 unique printable ASCII characters (got %lu)", (unsigned long)len);
        prof_cons_show(msg);
        g_autofree char *msg2 = g_strdup_printf("  (current: \"%s\")", alphabet_orig);
        prof_cons_show(msg2);
        return;
    }
    g_free(alphabet_orig);
    alphabet_orig = g_strdup(args[0]);
    g_free(alphabet);
    alphabet = g_strdup(alphabet_orig);
    alphabet_len = (int)strlen(alphabet);
    shuffle_alphabet();
    settings_save();
    g_autofree char *msg = g_strdup_printf("Smart URL: alphabet set to \"%s\" (%d chars), shuffled for use",
                                           alphabet_orig, alphabet_len);
    prof_cons_show(msg);
}

/* ------------------------------------------------------------------ */
/*  /surl command dispatch                                            */
/* ------------------------------------------------------------------ */

typedef void (*cmd_handler_fn)(char **args);

typedef struct {
    const gchar *name;
    cmd_handler_fn handler;
} CmdDispatch;

static const CmdDispatch cmd_dispatch[] = {
    { "open",     cmd_handle_open     },
    { "save",     cmd_handle_save     },
    { "copy",     cmd_handle_copy     },
    { "short",    cmd_handle_short    },
    { "maxlen",   cmd_handle_maxlen   },
    { "alphabet", cmd_handle_alphabet },
    { NULL, NULL }
};

static void cmd_handle_status(void) {
    prof_cons_show("Smart URL settings:");
    g_autofree char *s_short = g_strdup_printf("  shortening: %s", shorten_enabled ? "ON" : "OFF");
    prof_cons_show(s_short);
    g_autofree char *s_maxlen = g_strdup_printf("  maxlen: %d", max_display_len);
    prof_cons_show(s_maxlen);
    g_autofree char *s_alpha = g_strdup_printf("  alphabet: \"%s\" (%d chars)", alphabet_orig, alphabet_len);
    prof_cons_show(s_alpha);
}

static void surl_command_cb(char **args) {
    if (!args || !args[0]) {
        cmd_handle_status();
        return;
    }

    for (const CmdDispatch *d = cmd_dispatch; d->name; d++) {
        if (g_str_equal(args[0], d->name)) {
            d->handler(args + 1);
            return;
        }
    }

    g_autofree char *msg = g_strdup_printf("Smart URL: unknown subcommand \"%s\"", args[0]);
    prof_cons_show(msg);
    prof_cons_bad_cmd_usage("/surl");
}

/* ------------------------------------------------------------------ */
/*  Shorthand commands                                                */
/* ------------------------------------------------------------------ */

static void suo_command_cb(char **args) {
    if (!args || !args[0]) { prof_cons_show("Usage: /suo <code>"); return; }
    do_open(args[0]);
}

static void sus_command_cb(char **args) {
    if (!args || !args[0]) { prof_cons_show("Usage: /sus <code>"); return; }
    do_save(args[0]);
}

static void suc_command_cb(char **args) {
    if (!args || !args[0]) { prof_cons_show("Usage: /suc <code>"); return; }
    do_copy(args[0]);
}

/* ------------------------------------------------------------------ */
/*  Plugin lifecycle                                                  */
/* ------------------------------------------------------------------ */

void prof_init(G_GNUC_UNUSED const char *const version,
               G_GNUC_UNUSED const char *const status,
               G_GNUC_UNUSED const char *const account_name,
               G_GNUC_UNUSED const char *const fulljid) {
    code_to_url = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    url_regex = g_regex_new("(https?|aesgcm)://[\\w\\-.~:/?#[\\]@!$&'()*+,;=%]+", 0, 0, NULL);
    strophe_ctx = xmpp_ctx_new(NULL, NULL);
    settings_load();

    /* /surl */
    static char *surl_synopsis[] = {
        "/surl open <code>",
        "/surl save <code>",
        "/surl copy <code>",
        "/surl short on|off",
        "/surl maxlen <number>",
        "/surl alphabet <chars>",
        NULL
    };
    static char *surl_arguments[][2] = {
        { "open|save|copy",  "Subcommand: open, save, or copy a URL" },
        { "short on|off",    "Enable/disable URL shortening" },
        { "maxlen <number>", "Max display length (10–1000, default 80)" },
        { "alphabet <chars>", "Set alphabet for codes (3+ unique printable ASCII chars)" },
        { "code",            "Short code shown next to the URL" },
        { NULL, NULL }
    };
    static char *surl_examples[] = {
        "/surl open kR",
        "/surl save 7p",
        "/surl copy Q3",
        "/surl short off",
        "/surl maxlen 80",
        "/surl alphabet abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789",
        NULL
    };
    prof_register_command("/surl", 0, 2, surl_synopsis,
        "Smart URL — operate on URLs by their short codes. "
        "URLs in incoming messages are annotated with a code and emoji: "
        "📤 for uploads (aesgcm://, your or the sender's domain), 🔗 for external links "
        "(e.g. 📤[kR]upload.example.org/…/file.pdf). ",
        surl_arguments, surl_examples, surl_command_cb);

    /* /suo */
    static char *suo_synopsis[] = { "/suo <code>", NULL };
    static char *suo_arguments[][2] = { { "code", "Short code shown next to the URL" }, { NULL, NULL } };
    static char *suo_examples[] = { "/suo kR", NULL };
    prof_register_command("/suo", 1, 1, suo_synopsis,
        "Shorthand for /surl open.", suo_arguments, suo_examples, suo_command_cb);

    /* /sus */
    static char *sus_synopsis[] = { "/sus <code>", NULL };
    static char *sus_arguments[][2] = { { "code", "Short code shown next to the URL" }, { NULL, NULL } };
    static char *sus_examples[] = { "/sus 7p", NULL };
    prof_register_command("/sus", 1, 1, sus_synopsis,
        "Shorthand for /surl save.", sus_arguments, sus_examples, sus_command_cb);

    /* /suc */
    static char *suc_synopsis[] = { "/suc <code>", NULL };
    static char *suc_arguments[][2] = { { "code", "Short code shown next to the URL" }, { NULL, NULL } };
    static char *suc_examples[] = { "/suc Q3", NULL };
    prof_register_command("/suc", 1, 1, suc_synopsis,
        "Shorthand for /surl copy.", suc_arguments, suc_examples, suc_command_cb);

    /* Autocompletion */
    static char *subcmds[] = { "open", "save", "copy", "short", "maxlen", "alphabet", NULL };
    prof_completer_add("/surl", subcmds);
    static char *short_vals[] = { "on", "off", NULL };
    prof_completer_add("/surl short", short_vals);
}

void prof_on_start(void) {
    settings_load();
}

void prof_on_connect(G_GNUC_UNUSED const char *const account_name,
                     const char *const fulljid) {
    if (fulljid) {
        g_free(my_domain);
        char *domain = xmpp_jid_domain(strophe_ctx, fulljid);
        my_domain = domain ? g_strdup(domain) : NULL;
        if (domain) xmpp_free(strophe_ctx, domain);
    }
}

void prof_on_disconnect(G_GNUC_UNUSED const char *const account_name,
                        G_GNUC_UNUSED const char *const fulljid) {
    g_free(my_domain);
    my_domain = NULL;
    /* Clear URL codes — they're session-specific and use the shuffled alphabet */
    if (code_to_url)
        g_hash_table_remove_all(code_to_url);
    next_code_index = 0;
}

void prof_on_shutdown(void) {
    settings_save();
    g_clear_pointer(&url_regex, g_regex_unref);
    g_clear_pointer(&code_to_url, g_hash_table_destroy);
    g_clear_pointer(&alphabet, g_free);
    g_clear_pointer(&alphabet_orig, g_free);
    g_clear_pointer(&my_domain, g_free);
    g_clear_pointer(&strophe_ctx, xmpp_ctx_free);
}

void prof_on_unload(void) {
    prof_on_shutdown();

    prof_completer_clear("/surl");
    prof_completer_clear("/surl open");
    prof_completer_clear("/surl save");
    prof_completer_clear("/surl copy");
    prof_completer_clear("/surl short");
    prof_completer_clear("/suo");
    prof_completer_clear("/sus");
    prof_completer_clear("/suc");
}
