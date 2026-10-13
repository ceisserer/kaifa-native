/*
 * libkaifa - read out the Kaifa MA309 smart meter via M-Bus serial interface
 *
 * Framing, decryption and OBIS parsing follow kaifareader.py, see kaifa.h for
 * the interface.
 *
 * Dependencies: OpenSSL libcrypto
 */

#define _GNU_SOURCE
#define KAIFA_BUILD

#include "kaifa.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>

#define SERIAL_READ_CHUNK_SIZE 100
/* the Python version lets the stream grow without bound; a telegram pair is < 400 bytes */
#define STREAM_MAX 8192

/* DLMS data types (only those used by the parser) */
#define DT_DOUBLE_LONG_UNSIGNED 0x06
#define DT_OCTET_STRING 0x09
#define DT_LONG_UNSIGNED 0x12

/* serial_read() results besides the byte count */
#define READ_ERROR -1
#define READ_ABORTED -2

/*
 * Logging
 */

typedef struct {
    kaifa_log_fn fn;
    void *user;
    int level;
} logger_t;

static bool log_enabled(const logger_t *log, int level)
{
    return log->fn && level >= log->level;
}

static void log_msg(const logger_t *log, int level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void log_msg(const logger_t *log, int level, const char *fmt, ...)
{
    if (!log_enabled(log, level))
        return;

    char *msg = NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    log->fn(level, msg, log->user);
    free(msg);
}

#define log_debug(log, ...) log_msg(log, KAIFA_LOG_DEBUG, __VA_ARGS__)
#define log_error(log, ...) log_msg(log, KAIFA_LOG_ERROR, __VA_ARGS__)

static void log_hex(const logger_t *log, int level, const char *prefix, const uint8_t *buf,
                    size_t len)
{
    if (!log_enabled(log, level))
        return;

    char *hex = malloc(len * 2 + 1);
    if (!hex)
        return;
    for (size_t i = 0; i < len; i++)
        sprintf(hex + i * 2, "%02x", buf[i]);
    hex[len * 2] = '\0';
    log_msg(log, level, "%s%s", prefix, hex);
    free(hex);
}

/*
 * Suppliers
 */

static const uint8_t FRAME1_START_BYTES[4] = { 0x68, 0xfa, 0xfa, 0x68 };
static const uint8_t FRAME2_END_BYTE = 0x16;

typedef struct {
    uint8_t frame2_start_bytes[4];
    size_t ic_start_byte;
    size_t enc_data_start_byte;
} supplier_t;

static const supplier_t SUPPLIERS[] = {
    [KAIFA_SUPPLIER_TINETZ] = { { 0x68, 0x72, 0x72, 0x68 }, 23, 27 },
    [KAIFA_SUPPLIER_EVN] = { { 0x68, 0x14, 0x14, 0x68 }, 22, 26 },
};

/*
 * OBIS codes
 */

typedef struct {
    const char *name;
    const char *unit;
    const char *obis_str;
    uint8_t obis[6];
} value_def_t;

static const value_def_t VALUES[KAIFA_VALUE_COUNT] = {
    [KAIFA_VOLTAGE_L1] = { "voltage_l1", "V", "1.0.32.7.0.255", { 1, 0, 32, 7, 0, 255 } },
    [KAIFA_VOLTAGE_L2] = { "voltage_l2", "V", "1.0.52.7.0.255", { 1, 0, 52, 7, 0, 255 } },
    [KAIFA_VOLTAGE_L3] = { "voltage_l3", "V", "1.0.72.7.0.255", { 1, 0, 72, 7, 0, 255 } },
    [KAIFA_CURRENT_L1] = { "current_l1", "A", "1.0.31.7.0.255", { 1, 0, 31, 7, 0, 255 } },
    [KAIFA_CURRENT_L2] = { "current_l2", "A", "1.0.51.7.0.255", { 1, 0, 51, 7, 0, 255 } },
    [KAIFA_CURRENT_L3] = { "current_l3", "A", "1.0.71.7.0.255", { 1, 0, 71, 7, 0, 255 } },
    [KAIFA_POWER_IN] = { "power_in", "W", "1.0.1.7.0.255", { 1, 0, 1, 7, 0, 255 } },
    [KAIFA_POWER_OUT] = { "power_out", "W", "1.0.2.7.0.255", { 1, 0, 2, 7, 0, 255 } },
    [KAIFA_ENERGY_IN] = { "energy_in", "Wh", "1.0.1.8.0.255", { 1, 0, 1, 8, 0, 255 } },
    [KAIFA_ENERGY_OUT] = { "energy_out", "Wh", "1.0.2.8.0.255", { 1, 0, 2, 8, 0, 255 } },
    [KAIFA_POWER_FACTOR] = { "power_factor", "", "1.0.13.7.0.255", { 1, 0, 13, 7, 0, 255 } },
};

static kaifa_value_t *value_slot(kaifa_reading_t *r, const uint8_t *code)
{
    for (size_t i = 0; i < KAIFA_VALUE_COUNT; i++)
        if (memcmp(VALUES[i].obis, code, 6) == 0)
            return &r->values[i];
    return NULL;
}

/*
 * Decoder state shared by kaifa_decode() and kaifa_reader_t
 */

typedef struct {
    logger_t log;
    const supplier_t *supplier;
    uint8_t key[32];
    size_t key_len;
} decoder_t;

static bool parse_key(const char *hex, uint8_t *key, size_t *key_len)
{
    size_t n = hex ? strlen(hex) : 0;
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

static int decoder_init(decoder_t *dec, const kaifa_config_t *cfg)
{
    memset(dec, 0, sizeof(*dec));
    dec->log = (logger_t){ cfg->log_fn, cfg->log_user, cfg->log_level };

    if ((unsigned)cfg->supplier >= sizeof(SUPPLIERS) / sizeof(SUPPLIERS[0])) {
        log_error(&dec->log, "Supplier not supported: %d", (int)cfg->supplier);
        return KAIFA_ERR_CONFIG;
    }
    dec->supplier = &SUPPLIERS[cfg->supplier];

    if (!parse_key(cfg->key_hex, dec->key, &dec->key_len)) {
        log_error(&dec->log, "Invalid key (expected 32, 48 or 64 hex digits)");
        return KAIFA_ERR_CONFIG;
    }
    return KAIFA_OK;
}

/*
 * Serial port (settings and read semantics like pyserial)
 */

struct kaifa_reader {
    decoder_t dec;
    double chunk_timeout;
    int fd;
    int abort_pipe[2];
    uint8_t stream[STREAM_MAX];
    size_t stream_len;
};

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

static int serial_open(const kaifa_config_t *cfg, const logger_t *log)
{
    speed_t speed = baud_to_speed(cfg->baudrate);

    int fd = open(cfg->port, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        log_error(log, "Could not open port %s: %s", cfg->port, strerror(errno));
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) {
        int err = errno;
        log_error(log, "Could not configure port %s: %s", cfg->port, strerror(err));
        close(fd);
        errno = err;
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
        int err = errno;
        log_error(log, "Could not configure port %s: %s", cfg->port, strerror(err));
        close(fd);
        errno = err;
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
 * (total, not inter-byte) expired. Returns bytes read, READ_ERROR or
 * READ_ABORTED.
 */
static ssize_t serial_read(kaifa_reader_t *rd, uint8_t *buf, size_t size, double timeout)
{
    const logger_t *log = &rd->dec.log;
    double deadline = now_monotonic() + timeout;
    size_t got = 0;

    while (got < size) {
        double left = deadline - now_monotonic();
        int ms = left > 0 ? (int)ceil(left * 1000.0) : 0;
        struct pollfd pfd[2] = {
            { .fd = rd->fd, .events = POLLIN },
            { .fd = rd->abort_pipe[0], .events = POLLIN },
        };
        int r = poll(pfd, 2, ms);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            log_error(log, "Serial: poll failed: %s", strerror(errno));
            return READ_ERROR;
        }
        if (pfd[1].revents)
            return READ_ABORTED;
        if (r == 0)
            break;

        ssize_t n = read(rd->fd, buf + got, size - got);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            log_error(log, "Serial: read failed: %s", strerror(errno));
            return READ_ERROR;
        }
        if (n == 0) {
            log_error(log, "Serial: device reports readiness to read but returned no data "
                           "(device disconnected?)");
            return READ_ERROR;
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
 * The frames point into stream.
 */
static bool extract_frames(const supplier_t *sup, const uint8_t *stream, size_t len,
                           const uint8_t **frame1, size_t *frame1_len, const uint8_t **frame2,
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

    /* tokens i, i+1 and i+2, i+3 are contiguous */
    *frame1 = stream + tokens[i].off;
    *frame1_len = tokens[i].len + tokens[i + 1].len;
    *frame2 = stream + tokens[i + 2].off;
    *frame2_len = tokens[i + 2].len + tokens[i + 3].len;
    return true;
}

/*
 * Decryption (AES-GCM, tag not verified - same as the Python version)
 * with help of @micronano
 * https://www.photovoltaikforum.com/thread/157476-stromz%C3%A4hler-kaifa-ma309-welches-mbus-usb-kabel/?postID=2341069#post2341069
 */

static bool decrypt_frames(const decoder_t *dec, const uint8_t *frame1, size_t frame1_len,
                           const uint8_t *frame2, size_t frame2_len, uint8_t *out,
                           size_t *out_len, kaifa_reading_t *reading)
{
    const logger_t *log = &dec->log;
    const supplier_t *sup = dec->supplier;

    log_hex(log, KAIFA_LOG_DEBUG, "Decrypt: FRAME1:\n", frame1, frame1_len);
    log_hex(log, KAIFA_LOG_DEBUG, "Decrypt: FRAME2:\n", frame2, frame2_len);

    if (frame1_len < sup->enc_data_start_byte + 2 || frame2_len < 9 + 2) {
        log_error(log, "Decrypt: frames too short: %zu | %zu", frame1_len, frame2_len);
        return false;
    }

    uint8_t iv[12];
    memcpy(iv, frame1 + 11, 8); /* systitle at byte 12, length 8 */
    memcpy(iv + 8, frame1 + sup->ic_start_byte, 4); /* invocation counter, length 4 */
    memcpy(reading->system_title, iv, 8);
    reading->invocation_counter =
        (uint32_t)iv[8] << 24 | (uint32_t)iv[9] << 16 | (uint32_t)iv[10] << 8 | iv[11];
    log_hex(log, KAIFA_LOG_DEBUG, "SYSTITLE: ", iv, 8);
    log_debug(log, "IC: %02x%02x%02x%02x / %u", iv[8], iv[9], iv[10], iv[11],
              reading->invocation_counter);
    log_hex(log, KAIFA_LOG_DEBUG, "IV: ", iv, 12);

    /* excluding 2 bytes at end: checksum byte, end byte 0x16 */
    const uint8_t *data1 = frame1 + sup->enc_data_start_byte;
    size_t data1_len = frame1_len - sup->enc_data_start_byte - 2;
    const uint8_t *data2 = frame2 + 9;
    size_t data2_len = frame2_len - 9 - 2;
    log_hex(log, KAIFA_LOG_DEBUG, "DATA FRAME1\n", data1, data1_len);
    log_hex(log, KAIFA_LOG_DEBUG, "DATA FRAME2\n", data2, data2_len);

    const EVP_CIPHER *cipher = dec->key_len == 16 ? EVP_aes_128_gcm()
                             : dec->key_len == 24 ? EVP_aes_192_gcm()
                                                  : EVP_aes_256_gcm();
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n1 = 0, n2 = 0;
    bool ok = ctx && EVP_DecryptInit_ex(ctx, cipher, NULL, NULL, NULL) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), NULL) == 1 &&
              EVP_DecryptInit_ex(ctx, NULL, NULL, dec->key, iv) == 1 &&
              EVP_DecryptUpdate(ctx, out, &n1, data1, (int)data1_len) == 1 &&
              EVP_DecryptUpdate(ctx, out + n1, &n2, data2, (int)data2_len) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        log_error(log, "Decrypt: AES-GCM decryption failed");
        return false;
    }
    *out_len = (size_t)n1 + (size_t)n2;
    log_hex(log, KAIFA_LOG_DEBUG, "", out, *out_len);
    return true;
}

static void store_num(const logger_t *log, kaifa_reading_t *r, const uint8_t *code,
                      uint32_t value, int8_t scale, const char *type)
{
    double v;
    if (scale >= 0) {
        long long iv = value;
        for (int i = 0; i < scale && i < 9; i++)
            iv *= 10;
        v = (double)iv;
    } else {
        v = (double)value * pow(10.0, scale);
    }
    log_debug(log, "%s: %u, %d, %.17g", type, value, scale, v);

    kaifa_value_t *slot = value_slot(r, code);
    if (slot)
        *slot = (kaifa_value_t){ .valid = true, .raw = value, .scaler = scale, .value = v };
}

static void parse_all(const logger_t *log, const uint8_t *d, size_t total, kaifa_reading_t *r)
{
    size_t pos = 0;

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

        log_debug(log, "OBIS code %02x%02x%02x%02x%02x%02x DataType %u", obis_code[0],
                  obis_code[1], obis_code[2], obis_code[3], obis_code[4], obis_code[5],
                  data_type);

        if (data_type == DT_DOUBLE_LONG_UNSIGNED) {
            if (pos + 4 + 3 >= total)
                break;
            uint32_t value = (uint32_t)d[pos] << 24 | (uint32_t)d[pos + 1] << 16 |
                             (uint32_t)d[pos + 2] << 8 | d[pos + 3];
            int8_t scale = (int8_t)d[pos + 4 + 3];
            pos += 2 + 8;
            store_num(log, r, obis_code, value, scale, "DLU");
        } else if (data_type == DT_LONG_UNSIGNED) {
            if (pos + 2 + 3 >= total)
                break;
            uint32_t value = (uint32_t)d[pos] << 8 | d[pos + 1];
            int8_t scale = (int8_t)d[pos + 2 + 3];
            pos += 8;
            store_num(log, r, obis_code, value, scale, "LU");
        } else if (data_type == DT_OCTET_STRING) {
            if (pos >= total)
                break;
            size_t octet_len = d[pos];
            size_t avail = total - pos - 1 < octet_len ? total - pos - 1 : octet_len;
            /* like the Python version: an octet string replaces a number with the same code */
            kaifa_value_t *slot = value_slot(r, obis_code);
            if (slot)
                *slot = (kaifa_value_t){ 0 };
            if (log_enabled(log, KAIFA_LOG_DEBUG)) {
                char prefix[32];
                snprintf(prefix, sizeof(prefix), "OCTET: %zu, ", octet_len);
                log_hex(log, KAIFA_LOG_DEBUG, prefix, d + pos + 1, avail);
            }
            pos += 1 + octet_len + 2;
        }
    }
}

static int decode(const decoder_t *dec, const uint8_t *frame1, size_t frame1_len,
                  const uint8_t *frame2, size_t frame2_len, kaifa_reading_t *reading)
{
    uint8_t *decrypted = malloc(frame1_len + frame2_len);
    size_t decrypted_len;

    if (!decrypted)
        return KAIFA_ERR_NOMEM;
    memset(reading, 0, sizeof(*reading));
    bool ok = decrypt_frames(dec, frame1, frame1_len, frame2, frame2_len, decrypted,
                             &decrypted_len, reading);
    if (ok)
        parse_all(&dec->log, decrypted, decrypted_len, reading);
    free(decrypted);
    return ok ? KAIFA_OK : KAIFA_ERR_FRAME;
}

/*
 * Public interface
 */

void kaifa_config_init(kaifa_config_t *cfg)
{
    *cfg = (kaifa_config_t){
        .baudrate = 2400,
        .parity = 'N',
        .stopbits = 1,
        .bytesize = 8,
        .supplier = KAIFA_SUPPLIER_TINETZ,
        .chunk_timeout = 1.0,
        .log_level = KAIFA_LOG_INFO,
    };
}

int kaifa_open(const kaifa_config_t *cfg, kaifa_reader_t **reader)
{
    *reader = NULL;
    kaifa_reader_t *rd = calloc(1, sizeof(*rd));
    if (!rd)
        return KAIFA_ERR_NOMEM;
    rd->fd = -1;
    rd->abort_pipe[0] = rd->abort_pipe[1] = -1;

    int rc = decoder_init(&rd->dec, cfg);
    if (rc != KAIFA_OK)
        goto fail;
    if (!cfg->port || cfg->chunk_timeout < 0 || !baud_to_speed(cfg->baudrate)) {
        log_error(&rd->dec.log, "Invalid config: %s",
                  !cfg->port ? "port not set"
                  : cfg->chunk_timeout < 0 ? "negative chunk_timeout"
                                           : "unsupported baudrate");
        rc = KAIFA_ERR_CONFIG;
        goto fail;
    }
    rd->chunk_timeout = cfg->chunk_timeout;

    if (pipe2(rd->abort_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        rc = KAIFA_ERR_OPEN;
        goto fail;
    }
    rd->fd = serial_open(cfg, &rd->dec.log);
    if (rd->fd < 0) {
        rc = KAIFA_ERR_OPEN;
        goto fail;
    }
    *reader = rd;
    return KAIFA_OK;

fail: {
    int err = errno;
    kaifa_close(rd);
    errno = err;
    return rc;
}
}

int kaifa_read(kaifa_reader_t *rd, kaifa_reading_t *reading, int timeout_ms)
{
    const logger_t *log = &rd->dec.log;
    const supplier_t *sup = rd->dec.supplier;
    double deadline = now_monotonic() + timeout_ms / 1000.0;
    uint8_t chunk[SERIAL_READ_CHUNK_SIZE];

    for (;;) {
        /*
         * Read in chunks. Each chunk will wait as long as specified by
         * serial timeout. As the meters we tested send data every 5s the
         * timeout must be <5. Lower timeouts make us fail quicker.
         */
        double chunk_timeout = rd->chunk_timeout;
        if (timeout_ms >= 0) {
            double left = deadline - now_monotonic();
            if (left <= 0)
                return KAIFA_ERR_TIMEOUT;
            if (left < chunk_timeout)
                chunk_timeout = left;
        }
        ssize_t n = serial_read(rd, chunk, sizeof(chunk), chunk_timeout);
        if (n == READ_ABORTED)
            return KAIFA_ERR_ABORTED;
        if (n < 0)
            return KAIFA_ERR_IO;

        if (rd->stream_len + (size_t)n > STREAM_MAX) {
            /* never seen a complete telegram pair; keep the newest half */
            size_t keep = STREAM_MAX / 2;
            memmove(rd->stream, rd->stream + rd->stream_len - keep, keep);
            rd->stream_len = keep;
            log_debug(log, "stream buffer overflow, discarded old data");
        }
        memcpy(rd->stream + rd->stream_len, chunk, (size_t)n);
        rd->stream_len += (size_t)n;

        uint8_t *stream = rd->stream;
        size_t stream_len = rd->stream_len;
        ssize_t frame1_start_pos = find_bytes(stream, stream_len, FRAME1_START_BYTES, 4);
        ssize_t frame2_start_pos = find_bytes(stream, stream_len, sup->frame2_start_bytes, 4);

        /* fail as early as possible if we find the segment is not complete yet */
        if (frame1_start_pos < 0 || frame2_start_pos <= 0 || stream_len == 0 ||
            stream[stream_len - 1] != FRAME2_END_BYTE || n == SERIAL_READ_CHUNK_SIZE) {
            log_debug(log, "pos: %zd | %zd", frame1_start_pos, frame2_start_pos);
            log_hex(log, KAIFA_LOG_DEBUG, "incomplete segment: ", stream, stream_len);
            log_hex(log, KAIFA_LOG_DEBUG, "received chunk: ", chunk, (size_t)n);
            continue;
        }

        log_debug(log, "pos: %zd | %zd", frame1_start_pos, frame2_start_pos);

        /* frame2_start_pos could be smaller than frame1_start_pos */
        if (frame2_start_pos < frame1_start_pos) {
            /* start over with the stream from frame1 pos */
            memmove(stream, stream + frame1_start_pos, stream_len - (size_t)frame1_start_pos);
            rd->stream_len -= (size_t)frame1_start_pos;
            continue;
        }

        log_hex(log, KAIFA_LOG_DEBUG, "", stream, stream_len);

        /* take the first two matching telegrams, the rest of the stream is dropped */
        const uint8_t *frame1, *frame2;
        size_t frame1_len, frame2_len;
        rd->stream_len = 0;
        if (!extract_frames(sup, stream, stream_len, &frame1, &frame1_len, &frame2,
                            &frame2_len)) {
            log_error(log, "Could not extract Frame1 and Frame2 from stream, discarding it");
            continue;
        }

        log_hex(log, KAIFA_LOG_DEBUG, "TELEGRAM1:\n", frame1, frame1_len);
        log_hex(log, KAIFA_LOG_DEBUG, "TELEGRAM2:\n", frame2, frame2_len);
        return decode(&rd->dec, frame1, frame1_len, frame2, frame2_len, reading);
    }
}

void kaifa_abort(kaifa_reader_t *rd)
{
    int saved = errno; /* async-signal-safe */
    ssize_t r = write(rd->abort_pipe[1], "", 1);
    (void)r; /* pipe full: already aborted */
    errno = saved;
}

void kaifa_close(kaifa_reader_t *rd)
{
    if (!rd)
        return;
    if (rd->fd >= 0)
        close(rd->fd);
    if (rd->abort_pipe[0] >= 0)
        close(rd->abort_pipe[0]);
    if (rd->abort_pipe[1] >= 0)
        close(rd->abort_pipe[1]);
    free(rd);
}

int kaifa_decode(const kaifa_config_t *cfg, const uint8_t *frame1, size_t frame1_len,
                 const uint8_t *frame2, size_t frame2_len, kaifa_reading_t *reading)
{
    decoder_t dec;
    int rc = decoder_init(&dec, cfg);
    if (rc != KAIFA_OK)
        return rc;
    return decode(&dec, frame1, frame1_len, frame2, frame2_len, reading);
}

const char *kaifa_value_name(kaifa_value_id_t id)
{
    return (unsigned)id < KAIFA_VALUE_COUNT ? VALUES[id].name : NULL;
}

const char *kaifa_value_unit(kaifa_value_id_t id)
{
    return (unsigned)id < KAIFA_VALUE_COUNT ? VALUES[id].unit : NULL;
}

const char *kaifa_value_obis(kaifa_value_id_t id)
{
    return (unsigned)id < KAIFA_VALUE_COUNT ? VALUES[id].obis_str : NULL;
}

const char *kaifa_strerror(int rc)
{
    switch (rc) {
    case KAIFA_OK: return "success";
    case KAIFA_ERR_CONFIG: return "invalid configuration";
    case KAIFA_ERR_OPEN: return "could not open serial port";
    case KAIFA_ERR_IO: return "serial read failed";
    case KAIFA_ERR_TIMEOUT: return "timeout";
    case KAIFA_ERR_ABORTED: return "aborted";
    case KAIFA_ERR_FRAME: return "invalid telegram";
    case KAIFA_ERR_NOMEM: return "out of memory";
    default: return "unknown error";
    }
}
