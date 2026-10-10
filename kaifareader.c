/*
 * kaifareader - read out the Kaifa MA309 smart meter via M-Bus serial interface
 *
 * C port of kaifareader.py. Behaviour (config format, framing, decryption,
 * OBIS parsing, Solarview / MQTT export and their output formatting) follows
 * the Python version.
 *
 * Dependencies: OpenSSL libcrypto, libmosquitto, cJSON
 *
 * Usage: kaifareader [config_file]   (default: /etc/kaifareader/meter.json)
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <cjson/cJSON.h>
#include <mosquitto.h>
#include <openssl/evp.h>

#define DEFAULT_CONFIG_FILE "/etc/kaifareader/meter.json"

#define SERIAL_READ_CHUNK_SIZE 100
/* the Python version lets the stream grow without bound; a telegram pair is < 400 bytes */
#define STREAM_MAX 8192

#define LOG_MAX_BYTES (1024 * 1024 * 2)

/* exit codes as in the Python version */
#define EXIT_CONFIG 10
#define EXIT_LOGGING 20
#define EXIT_MQTT 40
#define EXIT_EXPORT 50

/* Python logging levels */
#define LVL_DEBUG 10
#define LVL_INFO 20
#define LVL_WARNING 30
#define LVL_ERROR 40
#define LVL_CRITICAL 50

/* DLMS data types (only those used by the parser) */
#define DT_DOUBLE_LONG_UNSIGNED 0x06
#define DT_OCTET_STRING 0x09
#define DT_LONG_UNSIGNED 0x12

static volatile sig_atomic_t g_stop = 0;

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

static void log_hex(int level, const char *prefix, const uint8_t *buf, size_t len)
{
    if (level < g_log.level)
        return;

    char *hex = malloc(len * 2 + 1);
    if (!hex)
        return;
    for (size_t i = 0; i < len; i++)
        sprintf(hex + i * 2, "%02x", buf[i]);
    hex[len * 2] = '\0';
    log_msg(level, "%s%s", prefix, hex);
    free(hex);
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

static const uint8_t FRAME1_START_BYTES[4] = { 0x68, 0xfa, 0xfa, 0x68 };
static const uint8_t FRAME2_END_BYTE = 0x16;

typedef struct {
    const char *name;
    uint8_t frame2_start_bytes[4];
    size_t ic_start_byte;
    size_t enc_data_start_byte;
} supplier_t;

static const supplier_t SUPPLIERS[] = {
    { "TINETZ", { 0x68, 0x72, 0x72, 0x68 }, 23, 27 },
    { "EVN", { 0x68, 0x14, 0x14, 0x68 }, 22, 26 },
};

/*
 * OBIS codes
 */

static const uint8_t OBIS_VOLTAGE_L1[6] = { 1, 0, 32, 7, 0, 255 };
static const uint8_t OBIS_VOLTAGE_L2[6] = { 1, 0, 52, 7, 0, 255 };
static const uint8_t OBIS_VOLTAGE_L3[6] = { 1, 0, 72, 7, 0, 255 };
static const uint8_t OBIS_CURRENT_L1[6] = { 1, 0, 31, 7, 0, 255 };
static const uint8_t OBIS_CURRENT_L2[6] = { 1, 0, 51, 7, 0, 255 };
static const uint8_t OBIS_CURRENT_L3[6] = { 1, 0, 71, 7, 0, 255 };
static const uint8_t OBIS_REAL_POWER_IN[6] = { 1, 0, 1, 7, 0, 255 };
static const uint8_t OBIS_REAL_POWER_OUT[6] = { 1, 0, 2, 7, 0, 255 };
static const uint8_t OBIS_REAL_ENERGY_IN[6] = { 1, 0, 1, 8, 0, 255 };
static const uint8_t OBIS_REAL_ENERGY_OUT[6] = { 1, 0, 2, 8, 0, 255 };
static const uint8_t OBIS_FACTOR[6] = { 1, 0, 13, 7, 0, 255 };

#define OBIS_REAL_ENERGY_IN_S "1.8.0"
#define OBIS_REAL_ENERGY_OUT_S "2.8.0"

/*
 * Values that can be published via MQTT (see "export_mqtt_topics")
 */

typedef enum {
    VAL_RAW, /* OBIS value as is */
    VAL_KWH, /* OBIS value (Wh) / 1000, a Python float like in kaifareader.py */
    VAL_NET, /* OBIS value - second OBIS value */
} value_kind_t;

typedef struct {
    const char *name;
    value_kind_t kind;
    const uint8_t *obis;
    const uint8_t *obis2; /* VAL_NET only */
} value_def_t;

static const value_def_t VALUES[] = {
    { "energy_in_kwh", VAL_KWH, OBIS_REAL_ENERGY_IN, NULL },
    { "energy_out_kwh", VAL_KWH, OBIS_REAL_ENERGY_OUT, NULL },
    { "energy_in", VAL_RAW, OBIS_REAL_ENERGY_IN, NULL },
    { "energy_out", VAL_RAW, OBIS_REAL_ENERGY_OUT, NULL },
    { "voltage_l1", VAL_RAW, OBIS_VOLTAGE_L1, NULL },
    { "voltage_l2", VAL_RAW, OBIS_VOLTAGE_L2, NULL },
    { "voltage_l3", VAL_RAW, OBIS_VOLTAGE_L3, NULL },
    { "current_l1", VAL_RAW, OBIS_CURRENT_L1, NULL },
    { "current_l2", VAL_RAW, OBIS_CURRENT_L2, NULL },
    { "current_l3", VAL_RAW, OBIS_CURRENT_L3, NULL },
    { "power_in", VAL_RAW, OBIS_REAL_POWER_IN, NULL },
    { "power_out", VAL_RAW, OBIS_REAL_POWER_OUT, NULL },
    { "power_net", VAL_NET, OBIS_REAL_POWER_IN, OBIS_REAL_POWER_OUT },
    { "power_factor", VAL_RAW, OBIS_FACTOR, NULL },
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

#define MAX_OBIS 64

typedef struct {
    uint8_t code[6];
    bool is_octet;
    pynum_t num;
} obis_entry_t;

typedef struct {
    obis_entry_t entries[MAX_OBIS];
    size_t count;
} obis_table_t;

static obis_entry_t *obis_slot(obis_table_t *t, const uint8_t *code)
{
    for (size_t i = 0; i < t->count; i++)
        if (memcmp(t->entries[i].code, code, 6) == 0)
            return &t->entries[i];
    if (t->count == MAX_OBIS)
        return NULL;
    obis_entry_t *e = &t->entries[t->count++];
    memcpy(e->code, code, 6);
    return e;
}

static const pynum_t *obis_get(const obis_table_t *t, const uint8_t *code)
{
    for (size_t i = 0; i < t->count; i++)
        if (memcmp(t->entries[i].code, code, 6) == 0)
            return t->entries[i].is_octet ? NULL : &t->entries[i].num;
    return NULL;
}

/*
 * Config
 */

typedef struct {
    cJSON *root;
    int loglevel;
    const char *logfile;
    const char *port;
    int baudrate;
    char parity; /* N, E, O, M, S */
    int stopbits; /* 1 or 2 (1.5 is treated as 2, like pyserial) */
    int bytesize;
    uint8_t key[32];
    size_t key_len;
    double interval;
    const supplier_t *supplier;
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

static bool parse_key(const char *hex, uint8_t *key, size_t *key_len)
{
    size_t n = strlen(hex);
    if (n != 32 && n != 48 && n != 64)
        return false;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned int b;
        if (!isxdigit((unsigned char)hex[2 * i]) || !isxdigit((unsigned char)hex[2 * i + 1]) ||
            sscanf(hex + 2 * i, "%2x", &b) != 1)
            return false;
        key[i] = (uint8_t)b;
    }
    *key_len = n / 2;
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
    cfg->port = item->valuestring;

    REQUIRE("baudrate");
    if (!cJSON_IsNumber(item))
        INVALID("baudrate");
    cfg->baudrate = item->valueint;

    REQUIRE("parity");
    if (!parse_parity(item, &cfg->parity))
        INVALID("parity");

    REQUIRE("stopbits");
    if (!parse_stopbits(item, &cfg->stopbits))
        INVALID("stopbits");

    REQUIRE("bytesize");
    if (!parse_bytesize(item, &cfg->bytesize))
        INVALID("bytesize");

    REQUIRE("key_hex_string");
    if (!cJSON_IsString(item) || !parse_key(item->valuestring, cfg->key, &cfg->key_len))
        INVALID("key_hex_string (expected 32, 48 or 64 hex digits)");

    REQUIRE("interval");
    if (!cJSON_IsNumber(item) || item->valuedouble < 0)
        INVALID("interval");
    cfg->interval = item->valuedouble;

    REQUIRE("supplier");
    if (!cJSON_IsString(item))
        INVALID("supplier");
    for (size_t i = 0; i < sizeof(SUPPLIERS) / sizeof(SUPPLIERS[0]); i++)
        if (strcasecmp(item->valuestring, SUPPLIERS[i].name) == 0)
            cfg->supplier = &SUPPLIERS[i];
    if (!cfg->supplier) {
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

    return true;
}

static bool export_format_is(const config_t *cfg, const char *fmt)
{
    return cfg->export_format && strcmp(cfg->export_format, fmt) == 0;
}

/*
 * Serial port (settings and read semantics like pyserial)
 */

static speed_t baud_to_speed(int baud)
{
    switch (baud) {
    case 300: return B300;
    case 600: return B600;
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

static int serial_open(const config_t *cfg)
{
    speed_t speed = baud_to_speed(cfg->baudrate);
    if (!speed) {
        fprintf(stderr, "Unsupported baudrate: %d\n", cfg->baudrate);
        return -1;
    }

    int fd = open(cfg->port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Could not open port %s: %s\n", cfg->port, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        fprintf(stderr, "Could not configure port %s: %s\n", cfg->port, strerror(errno));
        close(fd);
        return -1;
    }

    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG | IEXTEN | ECHOCTL | ECHOKE);
    tio.c_oflag &= ~(OPOST | ONLCR | OCRNL);
    tio.c_iflag &= ~(INLCR | IGNCR | ICRNL | IGNBRK | IUCLC | PARMRK | INPCK | ISTRIP);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD | CMSPAR | CRTSCTS);

    switch (cfg->bytesize) {
    case 5: tio.c_cflag |= CS5; break;
    case 6: tio.c_cflag |= CS6; break;
    case 7: tio.c_cflag |= CS7; break;
    default: tio.c_cflag |= CS8; break;
    }
    if (cfg->stopbits == 2)
        tio.c_cflag |= CSTOPB;
    switch (cfg->parity) {
    case 'E': tio.c_cflag |= PARENB; break;
    case 'O': tio.c_cflag |= PARENB | PARODD; break;
    case 'M': tio.c_cflag |= PARENB | CMSPAR | PARODD; break;
    case 'S': tio.c_cflag |= PARENB | CMSPAR; break;
    default: break;
    }
    if (cfg->parity != 'N')
        tio.c_iflag |= INPCK;

    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);

    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        fprintf(stderr, "Could not configure port %s: %s\n", cfg->port, strerror(errno));
        close(fd);
        return -1;
    }

    /* pyserial asserts DTR and RTS on open (errors ignored, e.g. on ptys) */
    int bits = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIS, &bits);

    tcflush(fd, TCIFLUSH);
    return fd;
}

static double now_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * Read up to size bytes; return when size bytes are read or the timeout
 * (total, not inter-byte) expired. Returns bytes read, -1 on error,
 * -2 if a stop signal arrived.
 */
static ssize_t serial_read(int fd, uint8_t *buf, size_t size, double timeout)
{
    double deadline = now_monotonic() + timeout;
    size_t got = 0;

    while (got < size) {
        if (g_stop)
            return -2;

        double left = deadline - now_monotonic();
        int ms = left > 0 ? (int)ceil(left * 1000.0) : 0;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int r = poll(&pfd, 1, ms);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            log_error("Serial: poll failed: %s", strerror(errno));
            return -1;
        }
        if (r == 0)
            break;

        ssize_t n = read(fd, buf + got, size - got);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            log_error("Serial: read failed: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            log_error("Serial: device reports readiness to read but returned no data "
                      "(device disconnected?)");
            return -1;
        }
        got += (size_t)n;
        if (now_monotonic() >= deadline)
            break;
    }
    return (ssize_t)got;
}

/*
 * Telegram framing
 */

static ssize_t find_bytes(const uint8_t *hay, size_t hay_len, const uint8_t *needle, size_t n)
{
    const uint8_t *p = memmem(hay, hay_len, needle, n);
    return p ? p - hay : -1;
}

typedef struct {
    size_t off;
    size_t len;
} token_t;

/*
 * Equivalent of the Python version's
 *   l = filter(None, re.split(b'(FRAME1_START|FRAME2_START)', stream))
 * followed by taking the first FRAME1_START and the three following elements:
 *   frame1 = l[i] + l[i+1], frame2 = l[i+2] + l[i+3]
 */
static bool extract_frames(const supplier_t *sup, const uint8_t *stream, size_t len,
                           uint8_t *frame1, size_t *frame1_len, uint8_t *frame2,
                           size_t *frame2_len)
{
    token_t tokens[STREAM_MAX / 4 * 2 + 2];
    size_t ntok = 0, start = 0, i = 0;

    while (i + 4 <= len) {
        if (memcmp(stream + i, FRAME1_START_BYTES, 4) == 0 ||
            memcmp(stream + i, sup->frame2_start_bytes, 4) == 0) {
            if (i > start)
                tokens[ntok++] = (token_t){ start, i - start };
            tokens[ntok++] = (token_t){ i, 4 };
            i += 4;
            start = i;
        } else {
            i++;
        }
    }
    if (len > start)
        tokens[ntok++] = (token_t){ start, len - start };

    for (i = 0; i < ntok; i++) {
        if (tokens[i].len == 4 && memcmp(stream + tokens[i].off, FRAME1_START_BYTES, 4) == 0)
            break;
    }
    if (i + 3 >= ntok)
        return false;

    *frame1_len = tokens[i].len + tokens[i + 1].len;
    memcpy(frame1, stream + tokens[i].off, *frame1_len); /* tokens i, i+1 are contiguous */
    *frame2_len = tokens[i + 2].len + tokens[i + 3].len;
    memcpy(frame2, stream + tokens[i + 2].off, *frame2_len);
    return true;
}

/*
 * Decryption (AES-GCM, tag not verified - same as the Python version)
 * with help of @micronano
 * https://www.photovoltaikforum.com/thread/157476-stromz%C3%A4hler-kaifa-ma309-welches-mbus-usb-kabel/?postID=2341069#post2341069
 */

static bool decrypt_frames(const config_t *cfg, const uint8_t *frame1, size_t frame1_len,
                           const uint8_t *frame2, size_t frame2_len, uint8_t *out,
                           size_t *out_len)
{
    const supplier_t *sup = cfg->supplier;

    log_hex(LVL_DEBUG, "Decrypt: FRAME1:\n", frame1, frame1_len);
    log_hex(LVL_DEBUG, "Decrypt: FRAME2:\n", frame2, frame2_len);

    if (frame1_len < sup->enc_data_start_byte + 2 || frame2_len < 9 + 2) {
        log_error("Decrypt: frames too short: %zu | %zu", frame1_len, frame2_len);
        return false;
    }

    uint8_t iv[12];
    memcpy(iv, frame1 + 11, 8); /* systitle at byte 12, length 8 */
    memcpy(iv + 8, frame1 + sup->ic_start_byte, 4); /* invocation counter, length 4 */
    log_hex(LVL_DEBUG, "SYSTITLE: ", iv, 8);
    log_debug("IC: %02x%02x%02x%02x / %u", iv[8], iv[9], iv[10], iv[11],
              (unsigned)iv[8] << 24 | (unsigned)iv[9] << 16 | (unsigned)iv[10] << 8 | iv[11]);
    log_hex(LVL_DEBUG, "IV: ", iv, 12);

    /* excluding 2 bytes at end: checksum byte, end byte 0x16 */
    const uint8_t *data1 = frame1 + sup->enc_data_start_byte;
    size_t data1_len = frame1_len - sup->enc_data_start_byte - 2;
    const uint8_t *data2 = frame2 + 9;
    size_t data2_len = frame2_len - 9 - 2;
    log_hex(LVL_DEBUG, "DATA FRAME1\n", data1, data1_len);
    log_hex(LVL_DEBUG, "DATA FRAME2\n", data2, data2_len);

    const EVP_CIPHER *cipher = cfg->key_len == 16 ? EVP_aes_128_gcm()
                             : cfg->key_len == 24 ? EVP_aes_192_gcm()
                                                  : EVP_aes_256_gcm();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n1 = 0, n2 = 0;
    bool ok = ctx && EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), NULL) == 1 &&
              EVP_DecryptInit_ex(ctx, NULL, NULL, cfg->key, iv) == 1 &&
              EVP_DecryptUpdate(ctx, out, &n1, data1, (int)data1_len) == 1 &&
              EVP_DecryptUpdate(ctx, out + n1, &n2, data2, (int)data2_len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        log_error("Decrypt: AES-GCM decryption failed");
        return false;
    }
    *out_len = (size_t)n1 + (size_t)n2;
    log_hex(LVL_DEBUG, "", out, *out_len);
    return true;
}

static void obis_store_num(obis_table_t *t, const uint8_t *code, uint32_t value, int8_t scale)
{
    obis_entry_t *e = obis_slot(t, code);
    if (!e)
        return;
    e->is_octet = false;
    if (scale >= 0) {
        long long v = value;
        for (int i = 0; i < scale && i < 9; i++)
            v *= 10;
        e->num = (pynum_t){ .is_float = false, .i = v };
    } else {
        /* Python: value * 10**scale with 10**scale == pow(10.0, scale) */
        e->num = (pynum_t){ .is_float = true, .f = (double)value * pow(10.0, scale) };
    }
}

static void parse_all(const uint8_t *d, size_t total, obis_table_t *t)
{
    size_t pos = 0;
    char numbuf[64];

    t->count = 0;
    while (pos < total) {
        if (d[pos] != DT_OCTET_STRING) {
            pos += 1;
            continue;
        }
        if (pos + 1 >= total)
            break;
        if (d[pos + 1] != 6) {
            pos += 1;
            continue;
        }
        if (pos + 2 + 6 >= total)
            break;
        const uint8_t *obis_code = d + pos + 2;
        uint8_t data_type = d[pos + 2 + 6];
        pos += 2 + 6 + 1;

        log_debug("OBIS code %02x%02x%02x%02x%02x%02x DataType %u", obis_code[0], obis_code[1],
                  obis_code[2], obis_code[3], obis_code[4], obis_code[5], data_type);

        if (data_type == DT_DOUBLE_LONG_UNSIGNED) {
            if (pos + 4 + 3 >= total)
                break;
            uint32_t value = (uint32_t)d[pos] << 24 | (uint32_t)d[pos + 1] << 16 |
                             (uint32_t)d[pos + 2] << 8 | d[pos + 3];
            int8_t scale = (int8_t)d[pos + 4 + 3];
            pos += 2 + 8;
            obis_store_num(t, obis_code, value, scale);
            if (g_log.level <= LVL_DEBUG) {
                const pynum_t *n = obis_get(t, obis_code);
                pynum_format(n ? *n : (pynum_t){ 0 }, numbuf, sizeof(numbuf));
                log_debug("DLU: %u, %d, %s", value, scale, numbuf);
            }
        } else if (data_type == DT_LONG_UNSIGNED) {
            if (pos + 2 + 3 >= total)
                break;
            uint32_t value = (uint32_t)d[pos] << 8 | d[pos + 1];
            int8_t scale = (int8_t)d[pos + 2 + 3];
            pos += 8;
            obis_store_num(t, obis_code, value, scale);
            if (g_log.level <= LVL_DEBUG) {
                const pynum_t *n = obis_get(t, obis_code);
                pynum_format(n ? *n : (pynum_t){ 0 }, numbuf, sizeof(numbuf));
                log_debug("LU: %u, %d, %s", value, scale, numbuf);
            }
        } else if (data_type == DT_OCTET_STRING) {
            if (pos >= total)
                break;
            size_t octet_len = d[pos];
            size_t avail = total - pos - 1 < octet_len ? total - pos - 1 : octet_len;
            obis_entry_t *e = obis_slot(t, obis_code);
            if (e)
                e->is_octet = true;
            if (g_log.level <= LVL_DEBUG) {
                char prefix[32];
                snprintf(prefix, sizeof(prefix), "OCTET: %zu, ", octet_len);
                log_hex(LVL_DEBUG, prefix, d + pos + 1, avail);
            }
            pos += 1 + octet_len + 2;
        }
    }
}

/*
 * Export
 */

static bool export_solarview(const config_t *cfg, const obis_table_t *t)
{
    const pynum_t *in = obis_get(t, OBIS_REAL_ENERGY_IN);
    const pynum_t *out = obis_get(t, OBIS_REAL_ENERGY_OUT);

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

static void export_mqtt(struct mosquitto *mosq, const config_t *cfg, const obis_table_t *t)
{
    for (size_t i = 0; i < cfg->mqtt_topic_count; i++) {
        const value_def_t *v = cfg->mqtt_topics[i].value;
        const pynum_t *a = obis_get(t, v->obis);
        const pynum_t *b = v->obis2 ? obis_get(t, v->obis2) : NULL;
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
    (void)sig;
    g_stop = 1;
}

/* fill frame1/frame2 with the next telegram pair; false on stop signal or serial error */
static bool read_telegrams(int fd, const config_t *cfg, uint8_t *frame1, size_t *frame1_len,
                           uint8_t *frame2, size_t *frame2_len, int *exit_code)
{
    const supplier_t *sup = cfg->supplier;
    static uint8_t stream[STREAM_MAX];
    size_t stream_len = 0;
    uint8_t chunk[SERIAL_READ_CHUNK_SIZE];

    for (;;) {
        /*
         * Read in chunks. Each chunk will wait as long as specified by
         * serial timeout. As the meters we tested send data every 5s the
         * timeout must be <5. Lower timeouts make us fail quicker.
         */
        ssize_t n = serial_read(fd, chunk, sizeof(chunk), cfg->interval);
        if (n < 0) {
            *exit_code = n == -2 ? 0 : 1;
            return false;
        }
        if (stream_len + (size_t)n > STREAM_MAX) {
            /* never seen a complete telegram pair; keep the newest half */
            size_t keep = STREAM_MAX / 2;
            memmove(stream, stream + stream_len - keep, keep);
            stream_len = keep;
            log_debug("stream buffer overflow, discarded old data");
        }
        memcpy(stream + stream_len, chunk, (size_t)n);
        stream_len += (size_t)n;

        ssize_t frame1_start_pos = find_bytes(stream, stream_len, FRAME1_START_BYTES, 4);
        ssize_t frame2_start_pos = find_bytes(stream, stream_len, sup->frame2_start_bytes, 4);

        /* fail as early as possible if we find the segment is not complete yet */
        if (frame1_start_pos < 0 || frame2_start_pos <= 0 || stream_len == 0 ||
            stream[stream_len - 1] != FRAME2_END_BYTE || n == SERIAL_READ_CHUNK_SIZE) {
            log_debug("pos: %zd | %zd", frame1_start_pos, frame2_start_pos);
            log_hex(LVL_DEBUG, "incomplete segment: ", stream, stream_len);
            log_hex(LVL_DEBUG, "received chunk: ", chunk, (size_t)n);
            continue;
        }

        log_debug("pos: %zd | %zd", frame1_start_pos, frame2_start_pos);

        /* frame2_start_pos could be smaller than frame1_start_pos */
        if (frame2_start_pos < frame1_start_pos) {
            /* start over with the stream from frame1 pos */
            memmove(stream, stream + frame1_start_pos, stream_len - (size_t)frame1_start_pos);
            stream_len -= (size_t)frame1_start_pos;
            continue;
        }

        log_hex(LVL_DEBUG, "", stream, stream_len);

        /* take the first two matching telegrams */
        if (!extract_frames(sup, stream, stream_len, frame1, frame1_len, frame2, frame2_len)) {
            log_error("Could not extract Frame1 and Frame2 from stream, discarding it");
            stream_len = 0;
            continue;
        }

        log_hex(LVL_DEBUG, "TELEGRAM1:\n", frame1, *frame1_len);
        log_hex(LVL_DEBUG, "TELEGRAM2:\n", frame2, *frame2_len);
        return true;
    }
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

    int fd = serial_open(&cfg);
    if (fd < 0)
        return 1;

    /* connect to mqtt broker */
    struct mosquitto *mosq = NULL;
    if (export_format_is(&cfg, "MQTT")) {
        /* keep signals away from the mosquitto thread, so they interrupt the serial poll */
        sigset_t block, old;
        sigemptyset(&block);
        sigaddset(&block, SIGINT);
        sigaddset(&block, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &block, &old);
        mosq = mqtt_start(&cfg);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        if (!mosq) {
            close(fd);
            return EXIT_MQTT;
        }
    }

    static uint8_t frame1[STREAM_MAX], frame2[STREAM_MAX], decrypted[2 * STREAM_MAX];
    size_t frame1_len, frame2_len, decrypted_len;
    static obis_table_t obis;

    /* main task endless loop */
    while (!g_stop) {
        if (!read_telegrams(fd, &cfg, frame1, &frame1_len, frame2, &frame2_len, &exit_code))
            break;

        if (!decrypt_frames(&cfg, frame1, frame1_len, frame2, frame2_len, decrypted,
                            &decrypted_len))
            continue;
        parse_all(decrypted, decrypted_len, &obis);

        if (export_format_is(&cfg, "SOLARVIEW") && !export_solarview(&cfg, &obis)) {
            log_error("Could not export data");
            exit_code = EXIT_EXPORT;
            break;
        }

        if (mosq)
            export_mqtt(mosq, &cfg, &obis);
    }

    if (g_stop)
        printf("Aborted by signal!\n");

    if (mosq) {
        mosquitto_disconnect(mosq);
        mosquitto_loop_stop(mosq, false);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
    }
    close(fd);
    cJSON_Delete(cfg.root);
    return exit_code;
}
