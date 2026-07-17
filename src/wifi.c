#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/wifi_utils.h>
#include <zephyr/sys/printk.h>
#include <zephyr/fs/fs.h>
//#include <mbedtls/aes.h>
#include <stdio.h>
#include <string.h>

#include "common.h"

LOG_MODULE_REGISTER(wifi_handler, LOG_LEVEL_DBG);
#define WIFI_SSID "ZEPHYR"
#define WIFI_PASS "Tvadivel1."
static void wifi_reconnect_work_handler(struct k_work *work);
static struct k_work_delayable wifi_reconnect_work;
wifi_credentials_t wifi_cred;

#define WIFI_SHELL_MGMT_EVENTS (            \
                NET_EVENT_WIFI_CONNECT_RESULT     |\
                NET_EVENT_WIFI_DISCONNECT_RESULT  |\
                NET_EVENT_WIFI_TWT                |\
                NET_EVENT_WIFI_AP_ENABLE_RESULT   |\
                NET_EVENT_WIFI_AP_DISABLE_RESULT  |\
                NET_EVENT_WIFI_AP_STA_CONNECTED   |\
                NET_EVENT_WIFI_AP_STA_DISCONNECTED|\
                NET_EVENT_WIFI_SIGNAL_CHANGE      |\
                NET_EVENT_WIFI_NEIGHBOR_REP_COMP)

/* Simple connect attempt (synchronous loop until requested to stop) */

static int read_wifi_credentials(wifi_credentials_t *cred)
{
    struct fs_file_t wifi_cred_file;
    fs_file_t_init(&wifi_cred_file);
    int ret = fs_open(&wifi_cred_file, WIFI_FILE_PATH, FS_O_READ);
    if (ret < 0) {
        LOG_ERR("Failed to open %s (%d)", WIFI_FILE_PATH, ret);
        return ret;
    }
    ret = fs_read(&wifi_cred_file, cred, sizeof(wifi_credentials_t));
    if (ret < 0) {
        LOG_ERR("Failed to read %s (%d)", WIFI_FILE_PATH, ret);
        fs_close(&wifi_cred_file);
        return ret;
    }
    fs_close(&wifi_cred_file);
    return 0;
}

int update_wifi_cred()
{
    struct fs_file_t wifi_cred_file;
    fs_file_t_init(&wifi_cred_file);
    int ret = fs_open(&wifi_cred_file, WIFI_FILE_PATH, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0) {
        LOG_ERR("Failed to open %s (%d)", WIFI_FILE_PATH, ret);
        return ret;
    }
    ret = fs_write(&wifi_cred_file, &wifi_cred, sizeof(wifi_credentials_t));
    if (ret < 0) {
        LOG_ERR("Failed to read %s (%d)", WIFI_FILE_PATH, ret);
        fs_close(&wifi_cred_file);
        return ret;
    }
    fs_close(&wifi_cred_file);
    wifi_disconnect();
    return 0;
}

static int wifi_connect_once(void)
{
    int err;
    do {
        LOG_INF("Trying to reconnect");
        if (read_wifi_credentials(&wifi_cred) < 0) {
            err = -1;
            break;
        }
        if (strlen(wifi_cred.password) < WIFI_PSK_MIN_LEN) {
            LOG_ERR("Wifi PSK length less than required");
            err = -2;
            break;
        }
        struct net_if *iface = net_if_get_default();
        struct wifi_connect_req_params params = {
            .ssid = wifi_cred.ssid,
            .ssid_length = strlen(wifi_cred.ssid),
            .psk = wifi_cred.password,
            .psk_length = strlen(wifi_cred.password),
            .security = WIFI_SECURITY_TYPE_PSK,
            .channel = WIFI_CHANNEL_ANY,
        };
        err = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
        if (err != 0) {
            LOG_ERR("net_mgmt connect request returned %d\n", err);
            break;
        }
    } while(0);

    if (err != 0) {
        k_work_schedule(&wifi_reconnect_work, K_SECONDS(5));
    }

    return 0;
}

/* Work handler scheduled on disconnect events */
static void wifi_reconnect_work_handler(struct k_work *work)
{
    printk("Wi-Fi reconnect work: attempting reconnect\n");
    /* Try repeatedly until success */
    int r = wifi_connect_once();
    if (r == 0) {
        struct net_if *iface = net_if_get_default();
        if (net_if_is_up(iface)) {
            printk("Wi-Fi connect requested; interface up\n");
        }
    }
}

void wifi_disconnect(void)
{
        struct net_if *iface = net_if_get_default();
        int r = net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
        if (r < 0) {
            LOG_INF("Failed to disconnect wifi (err %d)", r);
        }
}

/* net_mgmt event callback to handle Wi-Fi disconnects */
static struct net_mgmt_event_callback wifi_mgmt_cb;
static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                    uint64_t mgmt_event, struct net_if *iface)
{
    switch (mgmt_event)
    {
        case NET_EVENT_WIFI_CONNECT_RESULT:
            printk("Wi-Fi connected\n");
            break;
        case NET_EVENT_WIFI_DISCONNECT_RESULT:
            printk("Wi-Fi disconnected (event). Scheduling reconnect.\n");
            k_work_schedule(&wifi_reconnect_work, K_SECONDS(2));
            break;
        case NET_EVENT_WIFI_AP_STA_CONNECTED:
            printk("Station connected\n");
            break;
    }
}

bool check_wifi_status()
{
    struct net_if *iface = net_if_get_default();
    struct wifi_iface_status status = {0};

    if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(struct wifi_iface_status))) {
        return false;
    }

    if (status.state >= WIFI_STATE_ASSOCIATED) {
        return true;
    }
    return false;
}

void init_wifi(void)
{
    k_work_init_delayable(&wifi_reconnect_work, wifi_reconnect_work_handler);
    /* Register net_mgmt callback for wifi events */
    net_mgmt_init_event_callback(&wifi_mgmt_cb, wifi_mgmt_event_handler, WIFI_SHELL_MGMT_EVENTS);
    net_mgmt_add_event_callback(&wifi_mgmt_cb);

    /* Try to connect initially (worker will be used for reconnects) */
    k_work_schedule(&wifi_reconnect_work, K_NO_WAIT);
}

