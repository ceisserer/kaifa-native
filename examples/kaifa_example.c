/*
 * Minimal libkaifa example: print the meter values to stdout.
 *
 * Usage: kaifa_example <port> <key_hex> [TINETZ|EVN]
 * Build: cc -o kaifa_example kaifa_example.c $(pkg-config --cflags --libs kaifa)
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <kaifa.h>

static kaifa_reader_t *volatile g_reader;

static void on_signal(int sig)
{
    (void)sig;
    kaifa_abort(g_reader);
}

static void on_log(int level, const char *msg, void *user)
{
    (void)user;
    fprintf(stderr, "[%d] %s\n", level, msg);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <port> <key_hex> [TINETZ|EVN]\n", argv[0]);
        return 1;
    }

    kaifa_config_t cfg;
    kaifa_config_init(&cfg);
    cfg.port = argv[1];
    cfg.key_hex = argv[2];
    cfg.supplier = argc > 3 && strcmp(argv[3], "EVN") == 0 ? KAIFA_SUPPLIER_EVN
                                                            : KAIFA_SUPPLIER_TINETZ;
    cfg.log_fn = on_log;
    cfg.log_level = KAIFA_LOG_WARNING;

    kaifa_reader_t *reader;
    int rc = kaifa_open(&cfg, &reader);
    if (rc != KAIFA_OK) {
        fprintf(stderr, "kaifa_open: %s\n", kaifa_strerror(rc));
        return 1;
    }
    g_reader = reader;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    kaifa_reading_t r;
    for (;;) {
        rc = kaifa_read(reader, &r, 30000);
        if (rc == KAIFA_ERR_ABORTED || rc == KAIFA_ERR_IO)
            break;
        if (rc != KAIFA_OK) {
            fprintf(stderr, "kaifa_read: %s\n", kaifa_strerror(rc));
            continue;
        }
        printf("telegram %u\n", r.invocation_counter);
        for (int id = 0; id < KAIFA_VALUE_COUNT; id++)
            if (r.values[id].valid)
                printf("  %-13s %12.3f %s\n", kaifa_value_name(id), r.values[id].value,
                       kaifa_value_unit(id));
        fflush(stdout);
    }

    kaifa_close(reader);
    return rc == KAIFA_ERR_IO;
}
