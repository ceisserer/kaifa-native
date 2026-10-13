/*
 * libkaifa - read out the Kaifa MA309 smart meter via M-Bus serial interface
 *
 * Opens the serial port, reassembles the telegram pairs sent by the meter,
 * decrypts them (AES-GCM) and decodes the OBIS values.
 *
 * Minimal usage:
 *
 *     kaifa_config_t cfg;
 *     kaifa_config_init(&cfg);
 *     cfg.port = "/dev/ttyUSB0";
 *     cfg.key_hex = "00112233445566778899aabbccddeeff";
 *     cfg.supplier = KAIFA_SUPPLIER_TINETZ;
 *
 *     kaifa_reader_t *reader;
 *     int rc = kaifa_open(&cfg, &reader);
 *     if (rc != KAIFA_OK) { ...kaifa_strerror(rc)... }
 *
 *     kaifa_reading_t r;
 *     while ((rc = kaifa_read(reader, &r, -1)) != KAIFA_ERR_ABORTED) {
 *         if (rc == KAIFA_OK && r.values[KAIFA_POWER_IN].valid)
 *             printf("%.0f W\n", r.values[KAIFA_POWER_IN].value);
 *     }
 *     kaifa_close(reader);
 *
 * Dependencies: OpenSSL libcrypto
 */

#ifndef KAIFA_H
#define KAIFA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(KAIFA_BUILD) && defined(__GNUC__)
#define KAIFA_API __attribute__((visibility("default")))
#else
#define KAIFA_API
#endif

#define KAIFA_VERSION_MAJOR 1
#define KAIFA_VERSION_MINOR 0

/* return codes */
enum {
    KAIFA_OK = 0,
    KAIFA_ERR_CONFIG = -1,   /* invalid configuration (key, baudrate, ...) */
    KAIFA_ERR_OPEN = -2,     /* serial port could not be opened/configured, see errno */
    KAIFA_ERR_IO = -3,       /* serial read failed (e.g. device disconnected) */
    KAIFA_ERR_TIMEOUT = -4,  /* no complete telegram pair within the timeout */
    KAIFA_ERR_ABORTED = -5,  /* kaifa_abort() was called */
    KAIFA_ERR_FRAME = -6,    /* telegram pair malformed or not decryptable */
    KAIFA_ERR_NOMEM = -7,
};

/* log levels (same values as Python's logging module) */
enum {
    KAIFA_LOG_DEBUG = 10,
    KAIFA_LOG_INFO = 20,
    KAIFA_LOG_WARNING = 30,
    KAIFA_LOG_ERROR = 40,
};

/* grid operators; each uses its own telegram layout */
typedef enum {
    KAIFA_SUPPLIER_TINETZ,
    KAIFA_SUPPLIER_EVN,
} kaifa_supplier_t;

/* called for every log message with level >= log_level; msg has no trailing newline */
typedef void (*kaifa_log_fn)(int level, const char *msg, void *user);

typedef struct {
    const char *port;         /* serial device, e.g. "/dev/ttyUSB0" */
    int baudrate;             /* default 2400 */
    char parity;              /* 'N' (default), 'E', 'O', 'M' or 'S' */
    int stopbits;             /* 1 (default) or 2 */
    int bytesize;             /* 5..8, default 8 */
    const char *key_hex;      /* AES key from the grid operator: 32, 48 or 64 hex digits */
    kaifa_supplier_t supplier;
    double chunk_timeout;     /* seconds to wait for a chunk of serial data, default 1 */
    kaifa_log_fn log_fn;      /* optional, NULL disables logging */
    void *log_user;           /* passed to log_fn */
    int log_level;            /* default KAIFA_LOG_INFO */
} kaifa_config_t;

/* values sent by the meter */
typedef enum {
    KAIFA_VOLTAGE_L1,   /* V,  OBIS 1.0.32.7.0.255 */
    KAIFA_VOLTAGE_L2,   /* V,  OBIS 1.0.52.7.0.255 */
    KAIFA_VOLTAGE_L3,   /* V,  OBIS 1.0.72.7.0.255 */
    KAIFA_CURRENT_L1,   /* A,  OBIS 1.0.31.7.0.255 */
    KAIFA_CURRENT_L2,   /* A,  OBIS 1.0.51.7.0.255 */
    KAIFA_CURRENT_L3,   /* A,  OBIS 1.0.71.7.0.255 */
    KAIFA_POWER_IN,     /* W,  OBIS 1.0.1.7.0.255, active power import */
    KAIFA_POWER_OUT,    /* W,  OBIS 1.0.2.7.0.255, active power export */
    KAIFA_ENERGY_IN,    /* Wh, OBIS 1.0.1.8.0.255, active energy import */
    KAIFA_ENERGY_OUT,   /* Wh, OBIS 1.0.2.8.0.255, active energy export */
    KAIFA_POWER_FACTOR, /*     OBIS 1.0.13.7.0.255 */
    KAIFA_VALUE_COUNT
} kaifa_value_id_t;

typedef struct {
    bool valid;     /* false if the value was not contained in the telegram */
    uint32_t raw;   /* value as transmitted */
    int8_t scaler;  /* decimal exponent as transmitted */
    double value;   /* raw * 10^scaler, in the unit of kaifa_value_unit() */
} kaifa_value_t;

typedef struct {
    kaifa_value_t values[KAIFA_VALUE_COUNT]; /* indexed by kaifa_value_id_t */
    uint8_t system_title[8];                 /* meter system title */
    uint32_t invocation_counter;             /* increases with every telegram */
} kaifa_reading_t;

typedef struct kaifa_reader kaifa_reader_t;

/* fill cfg with defaults; port and key_hex must be set by the caller */
KAIFA_API void kaifa_config_init(kaifa_config_t *cfg);

/*
 * Open and configure the serial port. cfg is copied, it need not outlive the call.
 * On KAIFA_ERR_OPEN errno describes the cause.
 */
KAIFA_API int kaifa_open(const kaifa_config_t *cfg, kaifa_reader_t **reader);

/*
 * Wait for the next telegram pair and decode it into *reading.
 * timeout_ms < 0 waits forever. Returns KAIFA_OK, KAIFA_ERR_TIMEOUT,
 * KAIFA_ERR_ABORTED, KAIFA_ERR_FRAME (a telegram pair was received but could
 * not be decoded; calling again is fine) or KAIFA_ERR_IO (port unusable).
 */
KAIFA_API int kaifa_read(kaifa_reader_t *reader, kaifa_reading_t *reading, int timeout_ms);

/*
 * Make a pending and all further kaifa_read() calls return KAIFA_ERR_ABORTED.
 * Safe to call from other threads and from signal handlers.
 */
KAIFA_API void kaifa_abort(kaifa_reader_t *reader);

/* close the port and free the reader; NULL is ignored */
KAIFA_API void kaifa_close(kaifa_reader_t *reader);

/*
 * Decrypt and decode a telegram pair obtained by other means (no serial port
 * needed). Only key_hex, supplier and the log_* fields of cfg are used.
 */
KAIFA_API int kaifa_decode(const kaifa_config_t *cfg, const uint8_t *frame1, size_t frame1_len,
                           const uint8_t *frame2, size_t frame2_len, kaifa_reading_t *reading);

/* e.g. "voltage_l1"; NULL for an invalid id */
KAIFA_API const char *kaifa_value_name(kaifa_value_id_t id);

/* e.g. "V"; "" for the power factor, NULL for an invalid id */
KAIFA_API const char *kaifa_value_unit(kaifa_value_id_t id);

/* e.g. "1.0.32.7.0.255"; NULL for an invalid id */
KAIFA_API const char *kaifa_value_obis(kaifa_value_id_t id);

KAIFA_API const char *kaifa_strerror(int rc);

#ifdef __cplusplus
}
#endif

#endif /* KAIFA_H */
