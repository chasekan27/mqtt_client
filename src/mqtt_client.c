#include <errno.h>
#include <string.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
// #include <zephyr/posix/time.h>
#include <zephyr/sys/printk.h>
#include <zephyr/random/random.h>
#include <zephyr/net/net_if.h>
#include "common.h"
#include "config.h"
LOG_MODULE_REGISTER(iot_mqtt, LOG_LEVEL_DBG);

typedef struct __mqtt_client_ctx
{
    bool mqtt_connected;
} mqtt_client_ctx;

static mqtt_client_ctx mqtt_ctx;
/* The mqtt client struct */
static struct mqtt_client client_ctx;

#ifdef CONFIG_DNS_RESOLVER
static struct zsock_addrinfo hints;
static struct zsock_addrinfo *haddr;
#endif

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* Socket Poll */
static struct zsock_pollfd fds[1];
static int nfds;

/* Buffers for MQTT client. */
static uint8_t rx_buffer[1024];
static uint8_t tx_buffer[1024];

#define TOPIC_PRE_STR "zephyr_demo/devices/"

static void clear_fds(void);
static uint8_t devbound_topic[] = TOPIC_PRE_STR CONFIG_MOSQUITTO_CLIENT_ID "/temperature/config/#";
static struct mqtt_topic subs_topic;
static struct mqtt_subscription_list subs_list;
static struct k_work_delayable pub_message;

#define MQTT_CLIENT_STACK 7168
#define MQTT_CLIENT_PRIORITY 5
#define MQTT_CLIENT_FREQ_MS 15000

K_THREAD_STACK_DEFINE(mqtt_client_stack, MQTT_CLIENT_STACK);
static struct k_thread mqtt_client_thread_data;
static k_tid_t mqtt_client_tid;

int get_random_temp_data()
{
    int minTemp = 20;
    int maxTemp = 25;
    int temperature = minTemp + (sys_rand32_get() % (maxTemp - minTemp + 1));
    return temperature;
}

static int publish(struct mqtt_client *client, enum mqtt_qos qos)
{
    double temp = 25.5;
    // read_temperature(&temp);
    char payload[15] = {0};
    snprintf(payload, sizeof(payload), "%f", temp);
    char evt_topic[] = TOPIC_PRE_STR CONFIG_MOSQUITTO_CLIENT_ID "/temperature/data/";
    uint8_t len = strlen(evt_topic);
    struct mqtt_publish_param param;

    param.message.topic.qos = qos;
    param.message.topic.topic.utf8 = (uint8_t *)evt_topic;
    param.message.topic.topic.size = len;
    param.message.payload.data = payload;
    param.message.payload.len = strlen(payload);
    param.message_id = sys_rand16_get();
    param.dup_flag = 0U;
    param.retain_flag = 0U;
    LOG_INF("Publishing Data on %s\n", evt_topic);

    return mqtt_publish(client, &param);
}

/* Random time between 10 - 15 seconds
 * If you prefer to have this value more than CONFIG_MQTT_KEEPALIVE,
 * then keep the application connection live by calling mqtt_live()
 * in regular intervals.
 */
static uint8_t timeout_for_publish(void)
{
    return (10 + sys_rand8_get() % 5);
}

static void publish_timeout(struct k_work *work)
{
    int rc;

    if (!mqtt_ctx.mqtt_connected) {
        return;
    }

    rc = publish(&client_ctx, MQTT_QOS_0_AT_MOST_ONCE);
    if (rc) {
        LOG_ERR("mqtt_publish ERROR");
    } else {
        LOG_DBG("mqtt_publish OK");
    }
    k_work_reschedule(&pub_message, K_SECONDS(timeout_for_publish()));
}

static void mqtt_event_handler(struct mqtt_client *const client, const struct mqtt_evt *evt)
{
    struct mqtt_puback_param puback;
    uint8_t data[129];
    int len;

    switch (evt->type)
    {
        case MQTT_EVT_SUBACK:
            LOG_INF("SUBACK packet id: %u", evt->param.suback.message_id);
            break;

        case MQTT_EVT_UNSUBACK:
            LOG_INF("UNSUBACK packet id: %u", evt->param.suback.message_id);
            break;

        case MQTT_EVT_CONNACK:
            if (evt->result)
            {
                LOG_ERR("MQTT connect failed %d", evt->result);
                break;
            }

            mqtt_ctx.mqtt_connected = true;
            LOG_DBG("MQTT client connected!");
            break;

        case MQTT_EVT_DISCONNECT:
            LOG_DBG("MQTT client disconnected %d", evt->result);

            mqtt_ctx.mqtt_connected = false;
            clear_fds();
            break;

        case MQTT_EVT_PUBACK:
            if (evt->result)
            {
                LOG_ERR("MQTT PUBACK error %d", evt->result);
                break;
            }

            LOG_DBG("PUBACK packet id: %u\n", evt->param.puback.message_id);
            break;

        case MQTT_EVT_PUBLISH:
            len = evt->param.publish.message.payload.len;

            LOG_INF("MQTT publish received %d, %d bytes", evt->result, len);
            LOG_INF(" id: %d, qos: %d", evt->param.publish.message_id, evt->param.publish.message.topic.qos);

            while (len)
            {
                int bytes_read =
                    mqtt_read_publish_payload(&client_ctx, data, len >= sizeof(data) - 1 ? sizeof(data) - 1 : len);
                if (bytes_read < 0 && bytes_read != -EAGAIN)
                {
                    LOG_ERR("Fail to read payload");
                    break;
                }

                data[bytes_read] = '\0';
                LOG_INF("Payload: %s", data);
                len -= bytes_read;
            }

            puback.message_id = evt->param.publish.message_id;
            mqtt_publish_qos1_ack(&client_ctx, &puback);
            break;

        default:
            LOG_DBG("Unhandled MQTT event %d", evt->type);
            break;
    }
}

static void prepare_fds(struct mqtt_client *client)
{
    if (client->transport.type == MQTT_TRANSPORT_NON_SECURE)
    {
        fds[0].fd = client->transport.tcp.sock;
    }
    fds[0].events = ZSOCK_POLLIN;
    nfds = 1;
}

static void clear_fds(void)
{
    nfds = 0;
}

static int wait(int timeout)
{
    int rc = -EINVAL;

    if (nfds <= 0)
    {
        return rc;
    }

    rc = zsock_poll(fds, nfds, timeout);
    if (rc < 0)
    {
        LOG_ERR("poll error: %d", errno);
        return -errno;
    }

    return rc;
}

static void broker_init(void)
{
    struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;

    broker4->sin_family = AF_INET;
    broker4->sin_port = htons(CONFIG_MOSQUITTO_SERVER_PORT);

#if defined(CONFIG_DNS_RESOLVER)
    net_ipaddr_copy(&broker4->sin_addr, &net_sin(haddr->ai_addr)->sin_addr);
#else
    zsock_inet_pton(AF_INET, CONFIG_MOSQUITTO_SERVER_IP, &broker4->sin_addr);
#endif
}

static void client_init(struct mqtt_client *client)
{

    mqtt_client_init(client);

    broker_init();

    /* MQTT client configuration */
    client->broker = &broker;
    client->evt_cb = mqtt_event_handler;

    client->client_id.utf8 = (uint8_t *)CONFIG_MOSQUITTO_CLIENT_ID;
    client->client_id.size = strlen(CONFIG_MOSQUITTO_CLIENT_ID);

    client->password = NULL;
    client->user_name = NULL;
    client->keepalive = 60;
    client->clean_session = 1;

    client->protocol_version = MQTT_VERSION_3_1_1;

    /* MQTT buffers configuration */
    client->rx_buf = rx_buffer;
    client->rx_buf_size = sizeof(rx_buffer);
    client->tx_buf = tx_buffer;
    client->tx_buf_size = sizeof(tx_buffer);
    client->transport.type = MQTT_TRANSPORT_NON_SECURE;
}

static void poll_mqtt(void)
{
    int rc;

    while (mqtt_ctx.mqtt_connected)
    {
        rc = wait(SYS_FOREVER_MS);
        if (rc > 0)
        {
            mqtt_input(&client_ctx);
        }
    }
}

static void subscribe(struct mqtt_client *client)
{
    int err;

    /* subscribe */
    subs_topic.topic.utf8 = devbound_topic;
    subs_topic.topic.size = strlen(devbound_topic);
    subs_list.list = &subs_topic;
    subs_list.list_count = 1U;
    subs_list.message_id = 1U;

    err = mqtt_subscribe(client, &subs_list);
    if (err) {
        LOG_ERR("Failed on topic %s", devbound_topic);
    } else {
        LOG_INF("Subscribed to %s\n", devbound_topic);
    }
}

static int try_to_connect(struct mqtt_client *client)
{
    uint8_t retries = 3U;
    int rc;

    LOG_DBG("attempting to connect...");

    while (retries--)
    {
        client_init(client);

        rc = mqtt_connect(client);
        if (rc)
        {
            LOG_ERR("mqtt_connect failed %d", rc);
            k_msleep(MQTT_CLIENT_FREQ_MS);
            continue;
        }

        prepare_fds(client);

        rc = wait(APP_SLEEP_MSECS);
        if (rc < 0)
        {
            mqtt_abort(client);
            return rc;
        }

        mqtt_input(client);

        if (mqtt_ctx.mqtt_connected)
        {
            subscribe(client);
            k_work_reschedule(&pub_message, K_SECONDS(timeout_for_publish()));
            return 0;
        }

        mqtt_abort(client);

        wait(10 * MSEC_PER_SEC);
    }

    return -EINVAL;
}

#ifdef CONFIG_DNS_RESOLVER
static int get_mqtt_broker_addrinfo(void)
{
    int retries = 3;
    int rc = -EINVAL;

    while (retries--)
    {
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;

        rc = zsock_getaddrinfo(CONFIG_MOSQUITTO_HOSTNAME, "1883", &hints, &haddr);
        if (rc == 0)
        {
            LOG_INF("DNS resolved for %s:%d", CONFIG_MOSQUITTO_HOSTNAME, CONFIG_MOSQUITTO_SERVER_PORT);

            return 0;
        }

        LOG_ERR("DNS not resolved for %s:%d, retrying", CONFIG_MOSQUITTO_HOSTNAME, CONFIG_MOSQUITTO_SERVER_PORT);
    }

    return rc;
}
#endif
static void connect_to_cloud_and_publish(void)
{
    int rc = -EINVAL;

    do
    {
#ifdef CONFIG_DNS_RESOLVER
        rc = get_mqtt_broker_addrinfo();
        if (rc)
        {
            return;
        }
#endif
        rc = try_to_connect(&client_ctx);
        if (rc)
        {
            return;
        }
        poll_mqtt();
    } while (0);
}

void abort_mqtt_connection(void)
{
    if (mqtt_ctx.mqtt_connected)
    {
        mqtt_ctx.mqtt_connected = false;
        mqtt_abort(&client_ctx);
        k_work_cancel_delayable(&pub_message);
    }
}

static void mqtt_client(void *dummy1, void *dummy2, void *dummy3)
{
    ARG_UNUSED(dummy1);
    ARG_UNUSED(dummy2);
    ARG_UNUSED(dummy3);

    while (1)
    {
        do
        {
            if (false == check_wifi_status())
            {
                LOG_WRN("WiFi is not connected\n");
                break;
            }
            if (mqtt_ctx.mqtt_connected)
            {
                LOG_INF("MQTT Client is already connected");
                break;
            }
            connect_to_cloud_and_publish();
        } while (0);

        k_msleep(MQTT_CLIENT_FREQ_MS);
    }
}

int start_mqtt_client()
{
    mqtt_client_tid =
        k_thread_create(&mqtt_client_thread_data, mqtt_client_stack, K_THREAD_STACK_SIZEOF(mqtt_client_stack),
                mqtt_client, NULL, NULL, NULL, MQTT_CLIENT_PRIORITY, 0, K_NO_WAIT);
    k_thread_name_set(&mqtt_client_thread_data, "mqtt_client");
    k_thread_start(&mqtt_client_thread_data);
    k_work_init_delayable(&pub_message, publish_timeout);
    return 0;
}
