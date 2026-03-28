#include "nut_server.h"
#include "ups_driver.h"
#include "nvs_config.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "sdkconfig.h"

static const char *TAG = "nut_server";

#define MAX_CLIENTS  8
#define RX_BUF_SIZE  256
#define TX_BUF_SIZE  1024

/* Returns the configured UPS name (from NVS, e.g. "RD-UPS01"). */
static const char *nut_ups_name(void) { return nvs_config_get()->ups_name; }

typedef struct {
    int  fd;
    bool logged_in;
    bool username_ok;
} nut_client_t;

static nut_client_t s_clients[MAX_CLIENTS];
static int          s_listen_fd = -1;

static void client_sendf(int fd, const char *fmt, ...)
{
    char buf[TX_BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    send(fd, buf, strlen(buf), 0);
}

static const char *ups_description(void)
{
    const char *desc = nvs_config_get()->ups_desc;
    if (desc && desc[0]) return desc;
    /* Fallback: "Manufacturer Model" from HID device info */
    static char buf[96];
    const char *mfr   = ups_driver_get_var("device.mfr");
    const char *model = ups_driver_get_var("device.model");
    if (mfr && model)
        snprintf(buf, sizeof(buf), "%s %s", mfr, model);
    else if (model)
        snprintf(buf, sizeof(buf), "%s", model);
    else
        snprintf(buf, sizeof(buf), "UPS");
    return buf;
}

static void handle_list_ups(int fd)
{
    client_sendf(fd,
        "BEGIN LIST UPS\n"
        "UPS %s \"%s\"\n"
        "END LIST UPS\n",
        nut_ups_name(), ups_description());
}

static void handle_list_var(int fd, const char *ups_name)
{
    if (strcmp(ups_name, nut_ups_name()) != 0) {
        client_sendf(fd, "ERR UNKNOWN-UPS\n");
        return;
    }
    char buf[TX_BUF_SIZE];
    client_sendf(fd, "BEGIN LIST VAR %s\n", nut_ups_name());
    ups_driver_list_vars(buf, sizeof(buf));
    send(fd, buf, strlen(buf), 0);
    client_sendf(fd, "END LIST VAR %s\n", nut_ups_name());
}

static void process_line(nut_client_t *client, char *line)
{
    int fd = client->fd;
    char *tokens[8];
    int   ntok = 0;
    char *p = line;

    while (*p && ntok < 8) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        tokens[ntok++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    if (ntok == 0) return;

    const char *cmd = tokens[0];
    const char *uname = nut_ups_name();

    if (strcasecmp(cmd, "USERNAME") == 0) {
        if (ntok < 2) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
        client->username_ok = (strcmp(tokens[1], nvs_config_get()->nut_user) == 0);
        client_sendf(fd, client->username_ok ? "OK\n" : "ERR ACCESS-DENIED\n");

    } else if (strcasecmp(cmd, "PASSWORD") == 0) {
        if (ntok < 2) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
        bool ok = client->username_ok &&
                  strcmp(tokens[1], nvs_config_get()->nut_pass) == 0;
        client_sendf(fd, ok ? "OK\n" : "ERR ACCESS-DENIED\n");

    } else if (strcasecmp(cmd, "LOGIN") == 0) {
        if (ntok < 2) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
        if (strcmp(tokens[1], uname) != 0) {
            client_sendf(fd, "ERR UNKNOWN-UPS\n"); return;
        }
        client->logged_in = true;
        client_sendf(fd, "OK\n");

    } else if (strcasecmp(cmd, "LOGOUT") == 0) {
        client_sendf(fd, "OK Goodbye\n");
        close(fd);
        client->fd = -1;

    } else if (strcasecmp(cmd, "LIST") == 0) {
        if (ntok < 2) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
        if (strcasecmp(tokens[1], "UPS") == 0) {
            handle_list_ups(fd);
        } else if (strcasecmp(tokens[1], "VAR") == 0) {
            if (ntok < 3) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
            handle_list_var(fd, tokens[2]);
        } else if (strcasecmp(tokens[1], "CMD")    == 0 ||
                   strcasecmp(tokens[1], "RW")     == 0 ||
                   strcasecmp(tokens[1], "CLIENT")  == 0 ||
                   strcasecmp(tokens[1], "ENUM")    == 0 ||
                   strcasecmp(tokens[1], "RANGE")   == 0) {
            if (ntok < 3) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
            client_sendf(fd, "BEGIN LIST %s %s\nEND LIST %s %s\n",
                         tokens[1], tokens[2], tokens[1], tokens[2]);
        } else {
            client_sendf(fd, "ERR INVALID-ARGUMENT\n");
        }

    } else if (strcasecmp(cmd, "GET") == 0) {
        if (ntok < 2) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
        if (strcasecmp(tokens[1], "VAR") == 0) {
            if (ntok < 4) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
            if (strcmp(tokens[2], uname) != 0) {
                client_sendf(fd, "ERR UNKNOWN-UPS\n"); return;
            }
            const char *val = ups_driver_get_var(tokens[3]);
            if (!val) { client_sendf(fd, "ERR VAR-NOT-SUPPORTED\n"); return; }
            client_sendf(fd, "VAR %s %s \"%s\"\n", uname, tokens[3], val);

        } else if (strcasecmp(tokens[1], "TYPE") == 0) {
            if (ntok < 4) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
            if (strcmp(tokens[2], uname) != 0) {
                client_sendf(fd, "ERR UNKNOWN-UPS\n"); return;
            }
            const char *val = ups_driver_get_var(tokens[3]);
            if (!val) { client_sendf(fd, "ERR VAR-NOT-SUPPORTED\n"); return; }
            client_sendf(fd, "TYPE %s %s STRING:64\n", uname, tokens[3]);

        } else if (strcasecmp(tokens[1], "DESC") == 0) {
            if (ntok < 4) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
            client_sendf(fd, "DESC %s %s \"%s\"\n", uname, tokens[3], tokens[3]);

        } else if (strcasecmp(tokens[1], "NUMLOGINS") == 0) {
            if (ntok < 3) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
            int logins = 0;
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (s_clients[i].fd >= 0 && s_clients[i].logged_in) logins++;
            client_sendf(fd, "NUMLOGINS %s %d\n", tokens[2], logins);

        } else if (strcasecmp(tokens[1], "UPSDESC") == 0) {
            if (ntok < 3) { client_sendf(fd, "ERR MISSING-ARGUMENT\n"); return; }
            client_sendf(fd, "UPSDESC %s \"%s\"\n", tokens[2], ups_description());

        } else {
            client_sendf(fd, "ERR INVALID-ARGUMENT\n");
        }

    } else if (strcasecmp(cmd, "VER") == 0) {
        client_sendf(fd, "esp32-apc-nut-server 1.0.0\n");
    } else if (strcasecmp(cmd, "NETVER") == 0) {
        client_sendf(fd, "1.3\n");
    } else if (strcasecmp(cmd, "HELP") == 0) {
        client_sendf(fd, "Commands: USERNAME PASSWORD LOGIN LOGOUT LIST GET VER NETVER HELP\n");
    } else {
        client_sendf(fd, "ERR UNKNOWN-COMMAND\n");
    }
}

esp_err_t nut_server_init(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) s_clients[i].fd = -1;
    return ESP_OK;
}

void nut_server_stop(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_clients[i].fd >= 0) {
            send(s_clients[i].fd, "OK Goodbye\n", 11, 0);
            close(s_clients[i].fd);
            s_clients[i].fd = -1;
        }
    }
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    ESP_LOGI(TAG, "NUT server stopped");
}

void nut_server_task(void *arg)
{
    esp_task_wdt_add(NULL);

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen_fd < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
        return;
    }
    int listen_fd = s_listen_fd;

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(nvs_config_get()->nut_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listen_fd, MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "bind/listen failed: %d", errno);
        close(listen_fd);
        s_listen_fd = -1;
        esp_task_wdt_delete(NULL);
        vTaskDelete(NULL);
        return;
    }

    fcntl(listen_fd, F_SETFL, fcntl(listen_fd, F_GETFL, 0) | O_NONBLOCK);
    ESP_LOGI(TAG, "NUT server listening on port %d as \"%s\"",
             nvs_config_get()->nut_port, nut_ups_name());

    char rx_buf[RX_BUF_SIZE];
    char line_buf[MAX_CLIENTS][RX_BUF_SIZE];
    int  line_len[MAX_CLIENTS];
    memset(line_len, 0, sizeof(line_len));

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int max_fd = listen_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0) {
                FD_SET(s_clients[i].fd, &rfds);
                if (s_clients[i].fd > max_fd) max_fd = s_clients[i].fd;
            }
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        if (select(max_fd + 1, &rfds, NULL, NULL, &tv) < 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        esp_task_wdt_reset();

        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in cli_addr;
            socklen_t cli_len = sizeof(cli_addr);
            int cli_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
            if (cli_fd >= 0) {
                int slot = -1;
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (s_clients[i].fd < 0) { slot = i; break; }
                if (slot < 0) {
                    send(cli_fd, "ERR TOO-MANY-CONNECTIONS\n", 25, 0);
                    close(cli_fd);
                } else {
                    s_clients[slot].fd          = cli_fd;
                    s_clients[slot].logged_in   = false;
                    s_clients[slot].username_ok = false;
                    line_len[slot]              = 0;
                    fcntl(cli_fd, F_SETFL, fcntl(cli_fd, F_GETFL, 0) | O_NONBLOCK);
                    ESP_LOGI(TAG, "Client connected (slot %d)", slot);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_clients[i].fd < 0 || !FD_ISSET(s_clients[i].fd, &rfds)) continue;

            int n = recv(s_clients[i].fd, rx_buf, sizeof(rx_buf) - 1, 0);
            if (n <= 0) {
                ESP_LOGI(TAG, "Client slot %d disconnected", i);
                close(s_clients[i].fd);
                s_clients[i].fd = -1;
                line_len[i]     = 0;
                continue;
            }

            for (int b = 0; b < n; b++) {
                char c = rx_buf[b];
                if (c == '\r') continue;
                if (c == '\n') {
                    line_buf[i][line_len[i]] = '\0';
                    if (line_len[i] > 0)
                        process_line(&s_clients[i], line_buf[i]);
                    line_len[i] = 0;
                    if (s_clients[i].fd < 0) break;
                } else if (line_len[i] < RX_BUF_SIZE - 1) {
                    line_buf[i][line_len[i]++] = c;
                }
            }
        }
    }
}
