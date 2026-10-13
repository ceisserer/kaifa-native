/*
 * kaifareader - read out the Kaifa MA309 smart meter via M-Bus serial interface
 *
 * C port of kaifareader.py. Behaviour (config format, framing, decryption,
 * OBIS parsing, Solarview / MQTT export and their output formatting) follows
 * the Python version. Reading and decoding is done by libkaifa (kaifa.c),
 * this file contains the config, logging and export parts.
 *
 * Dependencies: libkaifa (OpenSSL libcrypto), libmosquitto, cJSON
 *
 * Usage: kaifareader [config_file]   (default: /etc/kaifareader/meter.json)
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <mosquitto.h>

#include "kaifa.h"

#define DEFAULT_CONFIG_FILE "/etc/kaifareader/meter.json"

#define LOG_MAX_BYTES (1024 * 1024 * 2)

/* exit codes as in the Python version */
#define EXIT_CONFIG 10
#define EXIT_LOGGING 20
#define EXIT_MQTT 40
#define EXIT_EXPORT 50

/* Python logging levels */
#define LVL_DEBUG KAIFA_LOG_DEBUG
#define LVL_INFO KAIFA_LOG_INFO
#define LVL_WARNING KAIFA_LOG_WARNING
#define LVL_ERROR KAIFA_LOG_ERROR
#define LVL_CRITICAL 50

static volatile sig_atomic_t g_stop = 0;
static kaifa_reader_t *volatile g_reader = NULL;

/*
 * Logging (format and rotation like Python's RotatingFileHandler,
 * maxBytes=2MiB, backupCount=1)
 */

static struct {
    FILE *fp;
    const char *path;
    int level;
    pthread_mutex_t mu;
} g_log = { NULL, NULL, LVL_INFO, PTHREAD_MUTEX_INITIALIZER };

static const char *level_name(int level)
{
    switch (level) {
    case LVL_DEBUG: return "DEBUG";
    case LVL_INFO: return "INFO";
    case LVL_WARNING: return "WARNING";
    case LVL_ERROR: return "ERROR";
    default: return "CRITICAL";
    }
}

static bool log_init(const char *path, int level)
{
    g_log.path = path;
    g_log.level = level;
    g_log.fp = fopen(path, "a");
    if (!g_log.fp) {
        fprintf(stderr, "Could not initialize logging system: %s: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

static void log_rollover_if_needed(size_t msg_len)
{
    struct stat st;

    if (fstat(fileno(g_log.fp), &st) != 0 || !S_ISREG(st.st_mode))
        return;
    if ((size_t)st.st_size + msg_len < LOG_MAX_BYTES)
        return;

    char backup[4096];
    snprintf(backup, sizeof(backup), "%s.1", g_log.path);
    fclose(g_log.fp);
    unlink(backup);
    rename(g_log.path, backup);
    g_log.fp = fopen(g_log.path, "a");
}

static void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void log_msg(int level, const char *fmt, ...)
{
    if (level < g_log.level || !g_log.fp)
        return;

    char *msg = NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    struct timespec ts;
    struct tm tm;
    char tbuf[32];
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm);

    pthread_mutex_lock(&g_log.mu);
    /* "%(asctime)s [%(levelname)s]:  %(message)s" */
    char head[64];
    int hlen = snprintf(head, sizeof(head), "%s,%03ld [%s]:  ", tbuf, ts.tv_nsec / 1000000L,
                        level_name(level));
    log_rollover_if_needed((size_t)hlen + (size_t)n + 1);
    if (g_log.fp) {
        fprintf(g_log.fp, "%s%s\n", head, msg);
        fflush(g_log.fp);
    }
    pthread_mutex_unlock(&g_log.mu);
    free(msg);
}

#define log_debug(...) log_msg(LVL_DEBUG, __VA_ARGS__)
#define log_info(...) log_msg(LVL_INFO, __VA_ARGS__)
#define log_error(...) log_msg(LVL_ERROR, __VA_ARGS__)

/* receives the log messages of libkaifa */
static void log_from_lib(int level, const char *msg, void *user)
{
    (void)user;
    log_msg(level, "%s", msg);
}

/*
 * Numbers with Python semantics: OBIS values are int (scale >= 0) or float
 * (scale < 0) in the Python version and are published via str(), so the
 * MQTT payloads must be formatted the same way (e.g. "230.10000000000002").
 */

typedef struct {
    bool is_float;
    long long i;
    double f;
} pynum_t;

static double pynum_as_double(pynum_t n)
{
    return n.is_float ? n.f : (double)n.i;
}

static pynum_t pynum_from_value(const kaifa_value_t *v)
{
    if (v->scaler < 0)
        return (pynum_t){ .is_float = true, .f = v->value };
    long long i = v->raw;
    for (int k = 0; k < v->scaler && k < 9; k++)
        i *= 10;
    return (pynum_t){ .is_float = false, .i = i };
}

static pynum_t pynum_sub(pynum_t a, pynum_t b)
{
    pynum_t r = { 0 };
    if (a.is_float || b.is_float) {
        r.is_float = true;
        r.f = pynum_as_double(a) - pynum_as_double(b);
    } else {
        r.i = a.i - b.i;
    }
    return r;
}

/* equivalent of Python's repr(float) for finite values */
static void format_py_float(double v, char *out, size_t sz)
{
    char buf[40];
    int prec;

    if (!isfinite(v)) {
        snprintf(out, sz, "%s", isnan(v) ? "nan" : (v > 0 ? "inf" : "-inf"));
        return;
    }
    if (v == 0.0) {
        snprintf(out, sz, "%s", signbit(v) ? "-0.0" : "0.0");
        return;
    }

    /* shortest representation that round-trips */
    for (prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*e", prec - 1, v);
        if (strtod(buf, NULL) == v)
            break;
    }

    /* split "-d.ddde+XX" into sign, digits and exponent */
    const char *p = buf;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        p++;
    }
    char digits[24];
    int nd = 0;
    for (; *p && *p != 'e'; p++)
        if (*p != '.')
            digits[nd++] = *p;
    while (nd > 1 && digits[nd - 1] == '0')
        nd--;
    digits[nd] = '\0';
    int exp10 = atoi(p + 1);
    int decpt = exp10 + 1; /* value = 0.digits * 10^decpt */

    char res[64];
    int pos = 0;
    if (neg)
        res[pos++] = '-';
    if (decpt <= -4 || decpt > 16) {
        res[pos++] = digits[0];
        if (nd > 1) {
            res[pos++] = '.';
            memcpy(res + pos, digits + 1, (size_t)nd - 1);
            pos += nd - 1;
        }
        pos += snprintf(res + pos, sizeof(res) - (size_t)pos, "e%c%02d", exp10 < 0 ? '-' : '+',
                        abs(exp10));
    } else if (decpt <= 0) {
        res[pos++] = '0';
        res[pos++] = '.';
        for (int i = 0; i < -decpt; i++)
            res[pos++] = '0';
        memcpy(res + pos, digits, (size_t)nd);
        pos += nd;
    } else if (decpt >= nd) {
        memcpy(res + pos, digits, (size_t)nd);
        pos += nd;
        for (int i = nd; i < decpt; i++)
            res[pos++] = '0';
        res[pos++] = '.';
        res[pos++] = '0';
    } else {
        memcpy(res + pos, digits, (size_t)decpt);
        pos += decpt;
        res[pos++] = '.';
        memcpy(res + pos, digits + decpt, (size_t)(nd - decpt));
        pos += nd - decpt;
    }
    res[pos] = '\0';
    snprintf(out, sz, "%s", res);
}

static void pynum_format(pynum_t n, char *out, size_t sz)
{
    if (n.is_float)
        format_py_float(n.f, out, sz);
    else
        snprintf(out, sz, "%lld", n.i);
}

/*
 * Suppliers
 */

static const struct {
    const char *name;
    kaifa_supplier_t id;
} SUPPLIERS[] = {
    { "TINETZ", KAIFA_SUPPLIER_TINETZ },
    { "EVN", KAIFA_SUPPLIER_EVN },
};

#define OBIS_REAL_ENERGY_IN_S "1.8.0"
#define OBIS_REAL_ENERGY_OUT_S "2.8.0"

/*
 * Values that can be published via MQTT (see "export_mqtt_topics")
 */

typedef enum {
    VAL_RAW, /* meter value as is */
    VAL_KWH, /* meter value (Wh) / 1000, a Python float like in kaifareader.py */
    VAL_NET, /* meter value - second meter value */
} value_kind_t;

typedef struct {
    const char *name;
    value_kind_t kind;
    kaifa_value_id_t id;
    kaifa_value_id_t id2; /* VAL_NET only */
} value_def_t;

static const value_def_t VALUES[] = {
    { "energy_in_kwh", VAL_KWH, KAIFA_ENERGY_IN, 0 },
    { "energy_out_kwh", VAL_KWH, KAIFA_ENERGY_OUT, 0 },
    { "energy_in", VAL_RAW, KAIFA_ENERGY_IN, 0 },
    { "energy_out", VAL_RAW, KAIFA_ENERGY_OUT, 0 },
    { "voltage_l1", VAL_RAW, KAIFA_VOLTAGE_L1, 0 },
    { "voltage_l2", VAL_RAW, KAIFA_VOLTAGE_L2, 0 },
    { "voltage_l3", VAL_RAW, KAIFA_VOLTAGE_L3, 0 },
    { "current_l1", VAL_RAW, KAIFA_CURRENT_L1, 0 },
    { "current_l2", VAL_RAW, KAIFA_CURRENT_L2, 0 },
    { "current_l3", VAL_RAW, KAIFA_CURRENT_L3, 0 },
    { "power_in", VAL_RAW, KAIFA_POWER_IN, 0 },
    { "power_out", VAL_RAW, KAIFA_POWER_OUT, 0 },
    { "power_net", VAL_NET, KAIFA_POWER_IN, KAIFA_POWER_OUT },
    { "power_factor", VAL_RAW, KAIFA_POWER_FACTOR, 0 },
};

static const value_def_t *value_find(const char *name)
{
    for (size_t i = 0; i < sizeof(VALUES) / sizeof(VALUES[0]); i++)
        if (strcmp(VALUES[i].name, name) == 0)
            return &VALUES[i];
    return NULL;
}

typedef struct {
    const char *topic;
    const char *value;
} topic_map_t;

/* published if "export_mqtt_topics" is not set: topics of kaifareader.py first */
static const topic_map_t DEFAULT_MQTT_TOPICS[] = {
    { "RealEnergyIn_S", "energy_in_kwh" },
    { "RealEnergyOut_S", "energy_out_kwh" },
    { "VoltageL1", "voltage_l1" },
    { "VoltageL2", "voltage_l2" },
    { "VoltageL3", "voltage_l3" },
    { "CurrentL1", "current_l1" },
    { "CurrentL2", "current_l2" },
    { "CurrentL3", "current_l3" },
    { "RealEnergyIn", "energy_in" },
    { "RealEnergyOut", "energy_out" },
    { "RealPowerIn", "power_in" },
    { "RealPowerOut", "power_out" },
    { "RealPower", "power_net" },
    { "PowerFactor", "power_factor" },
};

#define MAX_MQTT_TOPICS 64

typedef struct {
    const char *topic;
    const value_def_t *value;
} mqtt_topic_t;

/* meter value with Python semantics, NULL if not contained in the telegram */
static const pynum_t *reading_get(const kaifa_reading_t *r, kaifa_value_id_t id, pynum_t *buf)
{
    if (!r->values[id].valid)
        return NULL;
    *buf = pynum_from_value(&r->values[id]);
    return buf;
}

/*
 * Config
 */

typedef struct {
    cJSON *root;
    int loglevel;
    const char *logfile;
    kaifa_config_t meter; /* serial port, key, supplier, interval */
    const char *export_format;
    const char *export_file_abspath;
    const char *mqtt_server;
    int mqtt_port;
    const char *mqtt_user;
    const char *mqtt_password;
    const char *mqtt_basetopic;
    mqtt_topic_t mqtt_topics[MAX_MQTT_TOPICS];
    size_t mqtt_topic_count;
} config_t;

/* strip the module prefix of values that the Python version eval()s, e.g. "serial.PARITY_NONE" */
static const char *strip_prefix(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0 ? s + n : s;
}

static bool parse_loglevel(const cJSON *item, int *out)
{
    static const struct { const char *name; int level; } levels[] = {
        { "DEBUG", LVL_DEBUG }, { "INFO", LVL_INFO }, { "WARNING", LVL_WARNING },
        { "WARN", LVL_WARNING }, { "ERROR", LVL_ERROR }, { "CRITICAL", LVL_CRITICAL },
        { "FATAL", LVL_CRITICAL },
    };
    if (cJSON_IsNumber(item)) {
        *out = item->valueint;
        return true;
    }
    if (!cJSON_IsString(item))
        return false;
    const char *s = strip_prefix(item->valuestring, "logging.");
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(s, levels[i].name) == 0) {
            *out = levels[i].level;
            return true;
        }
    }
    char *end;
    long v = strtol(s, &end, 10);
    if (*s && !*end) {
        *out = (int)v;
        return true;
    }
    return false;
}

static bool parse_parity(const cJSON *item, char *out)
{
    static const struct { const char *name; char p; } parities[] = {
        { "PARITY_NONE", 'N' }, { "PARITY_EVEN", 'E' }, { "PARITY_ODD", 'O' },
        { "PARITY_MARK", 'M' }, { "PARITY_SPACE", 'S' },
        { "N", 'N' }, { "E", 'E' }, { "O", 'O' }, { "M", 'M' }, { "S", 'S' },
        { "'N'", 'N' }, { "'E'", 'E' }, { "'O'", 'O' }, { "'M'", 'M' }, { "'S'", 'S' },
    };
    if (!cJSON_IsString(item))
        return false;
    const char *s = strip_prefix(item->valuestring, "serial.");
    for (size_t i = 0; i < sizeof(parities) / sizeof(parities[0]); i++) {
        if (strcmp(s, parities[i].name) == 0) {
            *out = parities[i].p;
            return true;
        }
    }
    return false;
}

static bool parse_stopbits(const cJSON *item, int *out)
{
    double v;
    if (cJSON_IsNumber(item)) {
        v = item->valuedouble;
    } else if (cJSON_IsString(item)) {
        const char *s = strip_prefix(item->valuestring, "serial.");
        if (strcmp(s, "STOPBITS_ONE") == 0)
            v = 1;
        else if (strcmp(s, "STOPBITS_ONE_POINT_FIVE") == 0)
            v = 1.5;
        else if (strcmp(s, "STOPBITS_TWO") == 0)
            v = 2;
        else
            v = strtod(s, NULL);
    } else {
        return false;
    }
    if (v == 1) {
        *out = 1;
        return true;
    }
    /* 1.5 is treated as 2, like pyserial */
    if (v == 1.5 || v == 2) {
        *out = 2;
        return true;
    }
    return false;
}

static bool parse_bytesize(const cJSON *item, int *out)
{
    static const struct { const char *name; int bits; } sizes[] = {
        { "FIVEBITS", 5 }, { "SIXBITS", 6 }, { "SEVENBITS", 7 }, { "EIGHTBITS", 8 },
    };
    int v = 0;
    if (cJSON_IsNumber(item)) {
        v = item->valueint;
    } else if (cJSON_IsString(item)) {
        const char *s = strip_prefix(item->valuestring, "serial.");
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            if (strcmp(s, sizes[i].name) == 0)
                v = sizes[i].bits;
        if (!v)
            v = atoi(s);
    } else {
        return false;
    }
    if (v < 5 || v > 8)
        return false;
    *out = v;
    return true;
}

/* libkaifa checks the key as well, but here it is reported as config error */
static bool valid_key(const char *hex)
{
    size_t n = strlen(hex);
    if (n != 32 && n != 48 && n != 64)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)hex[i]))
            return false;
    return true;
}

/* optional string value, NULL if missing (Python: None) */
static const char *cfg_opt_string(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static bool config_load(config_t *cfg, const char *file)
{
    memset(cfg, 0, sizeof(*cfg));
    kaifa_config_init(&cfg->meter);

    FILE *f = fopen(file, "r");
    if (!f) {
        fprintf(stderr, "Error loading config file %s: %s\n", file, strerror(errno));
        return false;
    }
    char *data = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *tmp = realloc(data, cap);
            if (!tmp) {
                free(data);
                fclose(f);
                return false;
            }
            data = tmp;
        }
        size_t n = fread(data + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0)
            break;
    }
    fclose(f);
    data[len] = '\0';
    cfg->root = cJSON_Parse(data);
    free(data);
    if (!cfg->root) {
        fprintf(stderr, "Error loading config file %s: invalid JSON\n", file);
        return false;
    }

    const cJSON *r = cfg->root;
    const cJSON *item;

#define REQUIRE(name)                                                                  \
    item = cJSON_GetObjectItemCaseSensitive(r, name);                                  \
    if (!item) {                                                                       \
        fprintf(stderr, "Config file %s: missing key \"%s\"\n", file, name);           \
        return false;                                                                  \
    }
#define INVALID(name)                                                                  \
    do {                                                                               \
        fprintf(stderr, "Config file %s: invalid value for \"%s\"\n", file, name);     \
        return false;                                                                  \
    } while (0)

    REQUIRE("loglevel");
    if (!parse_loglevel(item, &cfg->loglevel))
        INVALID("loglevel");

    REQUIRE("logfile");
    if (!cJSON_IsString(item))
        INVALID("logfile");
    cfg->logfile = item->valuestring;

    REQUIRE("port");
    if (!cJSON_IsString(item))
        INVALID("port");
    cfg->meter.port = item->valuestring;

    REQUIRE("baudrate");
    if (!cJSON_IsNumber(item))
        INVALID("baudrate");
    cfg->meter.baudrate = item->valueint;

    REQUIRE("parity");
    if (!parse_parity(item, &cfg->meter.parity))
        INVALID("parity");

    REQUIRE("stopbits");
    if (!parse_stopbits(item, &cfg->meter.stopbits))
        INVALID("stopbits");

    REQUIRE("bytesize");
    if (!parse_bytesize(item, &cfg->meter.bytesize))
        INVALID("bytesize");

    REQUIRE("key_hex_string");
    if (!cJSON_IsString(item) || !valid_key(item->valuestring))
        INVALID("key_hex_string (expected 32, 48 or 64 hex digits)");
    cfg->meter.key_hex = item->valuestring;

    REQUIRE("interval");
    if (!cJSON_IsNumber(item) || item->valuedouble < 0)
        INVALID("interval");
    cfg->meter.chunk_timeout = item->valuedouble;

    REQUIRE("supplier");
    if (!cJSON_IsString(item))
        INVALID("supplier");
    bool supplier_found = false;
    for (size_t i = 0; i < sizeof(SUPPLIERS) / sizeof(SUPPLIERS[0]); i++) {
        if (strcasecmp(item->valuestring, SUPPLIERS[i].name) == 0) {
            cfg->meter.supplier = SUPPLIERS[i].id;
            supplier_found = true;
        }
    }
    if (!supplier_found) {
        fprintf(stderr, "Supplier not supported: %s\n", item->valuestring);
        return false;
    }

#undef REQUIRE
#undef INVALID

    cfg->export_format = cfg_opt_string(r, "export_format");
    cfg->export_file_abspath = cfg_opt_string(r, "export_file_abspath");
    cfg->mqtt_server = cfg_opt_string(r, "export_mqtt_server");
    item = cJSON_GetObjectItemCaseSensitive(r, "export_mqtt_port");
    cfg->mqtt_port = cJSON_IsNumber(item) ? item->valueint : 1883;
    cfg->mqtt_user = cfg_opt_string(r, "export_mqtt_user");
    cfg->mqtt_password = cfg_opt_string(r, "export_mqtt_password");
    cfg->mqtt_basetopic = cfg_opt_string(r, "export_mqtt_basetopic");

    item = cJSON_GetObjectItemCaseSensitive(r, "export_mqtt_topics");
    if (item) {
        const cJSON *entry;
        if (!cJSON_IsObject(item)) {
            fprintf(stderr, "Config file %s: \"export_mqtt_topics\" must be an object\n", file);
            return false;
        }
        cJSON_ArrayForEach(entry, item) {
            const value_def_t *v = cJSON_IsString(entry) ? value_find(entry->valuestring) : NULL;
            if (!v) {
                fprintf(stderr, "Config file %s: unknown value for MQTT topic \"%s\"\n", file,
                        entry->string);
                return false;
            }
            if (cfg->mqtt_topic_count == MAX_MQTT_TOPICS) {
                fprintf(stderr, "Config file %s: too many MQTT topics (max %d)\n", file,
                        MAX_MQTT_TOPICS);
                return false;
            }
            cfg->mqtt_topics[cfg->mqtt_topic_count++] = (mqtt_topic_t){ entry->string, v };
        }
    } else {
        for (size_t i = 0; i < sizeof(DEFAULT_MQTT_TOPICS) / sizeof(DEFAULT_MQTT_TOPICS[0]); i++)
            cfg->mqtt_topics[cfg->mqtt_topic_count++] =
                (mqtt_topic_t){ DEFAULT_MQTT_TOPICS[i].topic, value_find(DEFAULT_MQTT_TOPICS[i].value) };
    }

    cfg->meter.log_fn = log_from_lib;
    cfg->meter.log_level = cfg->loglevel;
    return true;
}

static bool export_format_is(const config_t *cfg, const char *fmt)
{
    return cfg->export_format && strcmp(cfg->export_format, fmt) == 0;
}

/*
 * Export
 */

static bool export_solarview(const config_t *cfg, const kaifa_reading_t *r)
{
    pynum_t in_buf, out_buf;
    const pynum_t *in = reading_get(r, KAIFA_ENERGY_IN, &in_buf);
    const pynum_t *out = reading_get(r, KAIFA_ENERGY_OUT, &out_buf);

    if (!in || !out) {
        log_error("Error writing to file %s: energy values missing in telegram",
                  cfg->export_file_abspath ? cfg->export_file_abspath : "None");
        return true; /* skip this telegram */
    }
    if (!cfg->export_file_abspath) {
        log_error("Error writing to file None: export_file_abspath not configured");
        return false;
    }

    FILE *f = fopen(cfg->export_file_abspath, "w");
    if (!f) {
        log_error("Error writing to file %s: %s", cfg->export_file_abspath, strerror(errno));
        return false;
    }
    fprintf(f, "/?!\n");    /* Start bytes */
    fprintf(f, "/KAIFA\n"); /* Meter ID */
    /* e.g. 1.8.0(005305.034*kWh) */
    fprintf(f, "%s(%010.3F*kWh)\n", OBIS_REAL_ENERGY_IN_S, pynum_as_double(*in) / 1000);
    fprintf(f, "%s(%010.3F*kWh)\n", OBIS_REAL_ENERGY_OUT_S, pynum_as_double(*out) / 1000);
    fprintf(f, "!\n"); /* End byte */
    if (fclose(f) != 0) {
        log_error("Error writing to file %s: %s", cfg->export_file_abspath, strerror(errno));
        return false;
    }
    return true;
}

static void mqtt_on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;
    if (rc == 0)
        log_info("MQTT: Client connected; rc=%d", rc);
    else
        log_error("MQTT: Client bad RC; rc=%d", rc);
}

static void mqtt_on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;
    log_info("MQTT: Client disconnected; rc=%d", rc);
    if (rc != 0)
        log_info("MQTT: Trying auto-reconnect; rc=%d", rc);
}

static struct mosquitto *mqtt_start(const config_t *cfg)
{
    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new("kaifareader", true, NULL);
    if (!mosq) {
        fprintf(stderr, "Failed to connect: could not create MQTT client\n");
        return NULL;
    }
    mosquitto_connect_callback_set(mosq, mqtt_on_connect);
    mosquitto_disconnect_callback_set(mosq, mqtt_on_disconnect);
    mosquitto_reconnect_delay_set(mosq, 1, 120, true);

    int rc = mosquitto_username_pw_set(mosq, cfg->mqtt_user, cfg->mqtt_password);
    if (rc == MOSQ_ERR_SUCCESS) {
        if (!cfg->mqtt_server)
            rc = MOSQ_ERR_INVAL;
        else
            rc = mosquitto_connect(mosq, cfg->mqtt_server, cfg->mqtt_port, 60);
    }
    if (rc == MOSQ_ERR_SUCCESS)
        rc = mosquitto_loop_start(mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        /* libmosquitto clobbers errno, so MOSQ_ERR_ERRNO has no usable detail */
        fprintf(stderr, "Failed to connect to %s:%d: %s\n",
                cfg->mqtt_server ? cfg->mqtt_server : "None", cfg->mqtt_port,
                rc == MOSQ_ERR_ERRNO ? "connection failed" : mosquitto_strerror(rc));
        mosquitto_destroy(mosq);
        return NULL;
    }
    return mosq;
}

static void mqtt_publish_num(struct mosquitto *mosq, const config_t *cfg, const char *name,
                             const pynum_t *value)
{
    /* Python formats a missing basetopic as "None" */
    const char *base = cfg->mqtt_basetopic ? cfg->mqtt_basetopic : "None";
    char topic[512];
    char payload[64];

    snprintf(topic, sizeof(topic), "%s/%s", base, name);
    if (!value) {
        log_error("MQTT: no value for %s in telegram", topic);
        return;
    }
    pynum_format(*value, payload, sizeof(payload));
    int rc = mosquitto_publish(mosq, NULL, topic, (int)strlen(payload), payload, 0, false);
    if (rc != MOSQ_ERR_SUCCESS)
        log_debug("MQTT: publish %s failed: %s", topic, mosquitto_strerror(rc));
}

static void export_mqtt(struct mosquitto *mosq, const config_t *cfg, const kaifa_reading_t *r)
{
    for (size_t i = 0; i < cfg->mqtt_topic_count; i++) {
        const value_def_t *v = cfg->mqtt_topics[i].value;
        pynum_t a_buf, b_buf;
        const pynum_t *a = reading_get(r, v->id, &a_buf);
        const pynum_t *b = v->kind == VAL_NET ? reading_get(r, v->id2, &b_buf) : NULL;
        pynum_t num;
        bool ok = a != NULL;

        switch (v->kind) {
        case VAL_RAW:
            if (ok)
                num = *a;
            break;
        case VAL_KWH:
            if (ok)
                num = (pynum_t){ .is_float = true, .f = pynum_as_double(*a) / 1000 };
            break;
        case VAL_NET:
            ok = a && b;
            if (ok)
                num = pynum_sub(*a, *b);
            break;
        }
        mqtt_publish_num(mosq, cfg, cfg->mqtt_topics[i].topic, ok ? &num : NULL);
    }
}

/*
 * Main
 */

static void signal_handler(int sig)
{
    kaifa_reader_t *reader = g_reader;

    (void)sig;
    g_stop = 1;
    if (reader)
        kaifa_abort(reader);
}

int main(int argc, char **argv)
{
    const char *config_file = argc > 1 ? argv[1] : DEFAULT_CONFIG_FILE;
    config_t cfg;
    int exit_code = 0;

    if (argc > 2 || (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))) {
        fprintf(stderr, "Usage: %s [config_file]   (default: %s)\n", argv[0], DEFAULT_CONFIG_FILE);
        return argc > 2 ? 1 : 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!config_load(&cfg, config_file)) {
        fprintf(stderr, "Could not load config file\n");
        return EXIT_CONFIG;
    }

    if (!log_init(cfg.logfile, cfg.loglevel))
        return EXIT_LOGGING;

    kaifa_reader_t *reader;
    int rc = kaifa_open(&cfg.meter, &reader);
    if (rc != KAIFA_OK) {
        if (rc == KAIFA_ERR_OPEN)
            fprintf(stderr, "Could not open port %s: %s\n", cfg.meter.port, strerror(errno));
        else
            fprintf(stderr, "Could not open port %s: %s\n", cfg.meter.port, kaifa_strerror(rc));
        return rc == KAIFA_ERR_CONFIG ? EXIT_CONFIG : 1;
    }
    g_reader = reader;
    if (g_stop) /* signal arrived before g_reader was set */
        kaifa_abort(reader);

    /* connect to mqtt broker */
    struct mosquitto *mosq = NULL;
    if (export_format_is(&cfg, "MQTT")) {
        /* keep signals away from the mosquitto thread */
        sigset_t block, old;
        sigemptyset(&block);
        sigaddset(&block, SIGINT);
        sigaddset(&block, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &block, &old);
        mosq = mqtt_start(&cfg);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        if (!mosq) {
            g_reader = NULL;
            kaifa_close(reader);
            return EXIT_MQTT;
        }
    }

    kaifa_reading_t reading;

    /* main task endless loop */
    while (!g_stop) {
        rc = kaifa_read(reader, &reading, -1);
        if (rc == KAIFA_ERR_FRAME)
            continue;
        if (rc != KAIFA_OK) {
            exit_code = rc == KAIFA_ERR_ABORTED ? 0 : 1;
            break;
        }

        if (export_format_is(&cfg, "SOLARVIEW") && !export_solarview(&cfg, &reading)) {
            log_error("Could not export data");
            exit_code = EXIT_EXPORT;
            break;
        }

        if (mosq)
            export_mqtt(mosq, &cfg, &reading);
    }

    if (g_stop)
        printf("Aborted by signal!\n");

    /* no kaifa_abort() from the signal handler while the reader is freed */
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, NULL);

    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, false);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
    }
    g_reader = NULL;
    kaifa_close(reader);
    cJSON_Delete(cfg.root);
    return exit_code;
}
