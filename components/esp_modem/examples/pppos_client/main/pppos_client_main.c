/*
 * SPDX-FileCopyrightText: 2022-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* PPPoS Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "mqtt_client.h"
#include "esp_modem_api.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "driver/gpio.h"


#if defined(CONFIG_EXAMPLE_FLOW_CONTROL_NONE)
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_NONE
#elif defined(CONFIG_EXAMPLE_FLOW_CONTROL_SW)
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_SW
#elif defined(CONFIG_EXAMPLE_FLOW_CONTROL_HW)
#define EXAMPLE_FLOW_CONTROL ESP_MODEM_FLOW_CONTROL_HW
#endif


static const char *TAG = "pppos_example";
static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT = BIT0;
static const int GOT_DATA_BIT = BIT2;
static const int USB_DISCONNECTED_BIT = BIT3; // Used only with USB DTE but we define it unconditionally, to avoid too many #ifdefs in the code

#if defined(CONFIG_EXAMPLE_SERIAL_CONFIG_USB)
#include "esp_modem_usb_c_api.h"
#include "esp_modem_usb_config.h"
#include "freertos/task.h"
static void usb_terminal_error_handler(esp_modem_terminal_error_t err)
{
    if (err == ESP_MODEM_TERMINAL_DEVICE_GONE) {
        ESP_LOGI(TAG, "USB modem disconnected");
        assert(event_group);
        xEventGroupSetBits(event_group, USB_DISCONNECTED_BIT);
    }
}
#define CHECK_USB_DISCONNECTION(event_group) \
if ((xEventGroupGetBits(event_group) & USB_DISCONNECTED_BIT) == USB_DISCONNECTED_BIT) { \
    esp_modem_destroy(dce); \
    continue; \
}
#else
#define CHECK_USB_DISCONNECTION(event_group)
#endif

// GPIO config for powering the modem on
#define GPIO_PWRKEY    ((gpio_num_t) 15)
#define GPIO_MODEM_SUPPLY_EN ((gpio_num_t) 21)
#define GPIO_OUTPUT_PIN_SEL  (1ULL<<GPIO_PWRKEY | 1ULL<<GPIO_MODEM_SUPPLY_EN)

void config_gpio(void)
{
    gpio_config_t io_conf = {};                     //zero-initialize the config structure.

    io_conf.intr_type = GPIO_INTR_DISABLE;          //disable interrupt
    io_conf.mode = GPIO_MODE_OUTPUT;                //set as output mode
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;     //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;   //disable pull-down mode
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;       //disable pull-up mode

    ESP_ERROR_CHECK(gpio_config(&io_conf));                          //configure GPIO with the given settings
}

void wakeup_modem(void)
{
    /* Power on the modem */
    ESP_LOGI(TAG, "Modem supply power off");  // Ensure hard reset of GPS between system reboots
    gpio_set_level(GPIO_MODEM_SUPPLY_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Modem supply power on");
    gpio_set_level(GPIO_MODEM_SUPPLY_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Press modem power button");
    gpio_set_level(GPIO_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    gpio_set_level(GPIO_PWRKEY, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, CONFIG_EXAMPLE_MQTT_TEST_TOPIC, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, CONFIG_EXAMPLE_MQTT_TEST_TOPIC, CONFIG_EXAMPLE_MQTT_TEST_DATA, 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        xEventGroupSetBits(event_group, GOT_DATA_BIT);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "MQTT other event id: %d", event->event_id);
        break;
    }
}

static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "PPP state changed event %d", event_id);
    if (event_id == NETIF_PPP_ERRORUSER) {
        /* User interrupted event from esp-netif */
        esp_netif_t *netif = event_data;
        ESP_LOGI(TAG, "User interrupted event from netif:%p", netif);
    }
}


static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "IP event! %d", event_id);
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        esp_netif_dns_info_t dns_info;

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_t *netif = event->esp_netif;

        ESP_LOGI(TAG, "Modem Connect to PPP Server");
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        esp_netif_get_dns_info(netif, 0, &dns_info);
        ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        esp_netif_get_dns_info(netif, 1, &dns_info);
        ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        xEventGroupSetBits(event_group, CONNECT_BIT);

        ESP_LOGI(TAG, "GOT ip event!!!");
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "Modem Disconnect from PPP Server");
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ESP_LOGI(TAG, "GOT IPv6 event!");

        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        ESP_LOGI(TAG, "Got IPv6 address " IPV6STR, IPV62STR(event->ip6_info.ip));
    }
}


void app_main(void)
{
    /* Init and register system/core components */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    /* Configure the PPP netif */
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(CONFIG_EXAMPLE_MODEM_PPP_APN);
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *esp_netif = esp_netif_new(&netif_ppp_config);
    assert(esp_netif);

    event_group = xEventGroupCreate();

    // Supply power to the GSM module
    config_gpio();
    wakeup_modem();

    /* Configure the DTE */
#if defined(CONFIG_EXAMPLE_SERIAL_CONFIG_UART)
    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    /* setup UART specific configuration based on kconfig options */
    dte_config.uart_config.tx_io_num = CONFIG_EXAMPLE_MODEM_UART_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_EXAMPLE_MODEM_UART_RX_PIN;
    dte_config.uart_config.rts_io_num = CONFIG_EXAMPLE_MODEM_UART_RTS_PIN;
    dte_config.uart_config.cts_io_num = CONFIG_EXAMPLE_MODEM_UART_CTS_PIN;
    dte_config.uart_config.flow_control = EXAMPLE_FLOW_CONTROL;
    dte_config.uart_config.rx_buffer_size = CONFIG_EXAMPLE_MODEM_UART_RX_BUFFER_SIZE;
    dte_config.uart_config.tx_buffer_size = CONFIG_EXAMPLE_MODEM_UART_TX_BUFFER_SIZE;
    dte_config.uart_config.event_queue_size = CONFIG_EXAMPLE_MODEM_UART_EVENT_QUEUE_SIZE;
    dte_config.task_stack_size = CONFIG_EXAMPLE_MODEM_UART_EVENT_TASK_STACK_SIZE;
    dte_config.task_priority = CONFIG_EXAMPLE_MODEM_UART_EVENT_TASK_PRIORITY;
    dte_config.dte_buffer_size = CONFIG_EXAMPLE_MODEM_UART_RX_BUFFER_SIZE / 2;

#if CONFIG_EXAMPLE_MODEM_DEVICE_BG96 == 1
    ESP_LOGI(TAG, "Initializing esp_modem for the BG96 module...");
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_BG96, &dte_config, &dce_config, esp_netif);
#elif CONFIG_EXAMPLE_MODEM_DEVICE_SIM800 == 1
    ESP_LOGI(TAG, "Initializing esp_modem for the SIM800 module...");
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM800, &dte_config, &dce_config, esp_netif);
#elif CONFIG_EXAMPLE_MODEM_DEVICE_SIM7000 == 1
    ESP_LOGI(TAG, "Initializing esp_modem for the SIM7000 module...");
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7000, &dte_config, &dce_config, esp_netif);
#elif CONFIG_EXAMPLE_MODEM_DEVICE_SIM7070 == 1
    ESP_LOGI(TAG, "Initializing esp_modem for the SIM7070 module...");
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7070, &dte_config, &dce_config, esp_netif);
#elif CONFIG_EXAMPLE_MODEM_DEVICE_SIM7600 == 1
    ESP_LOGI(TAG, "Initializing esp_modem for the SIM7600 module...");
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_config, &dce_config, esp_netif);
#else
    ESP_LOGI(TAG, "Initializing esp_modem for a generic module...");
    esp_modem_dce_t *dce = esp_modem_new(&dte_config, &dce_config, esp_netif);
#endif
    assert(dce);
    if (dte_config.uart_config.flow_control == ESP_MODEM_FLOW_CONTROL_HW) {
        esp_err_t err = esp_modem_set_flow_control(dce, 2, 2);  //2/2 means HW Flow Control.
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set the set_flow_control mode");
            return;
        }
        ESP_LOGI(TAG, "HW set_flow_control OK");
    }

#elif defined(CONFIG_EXAMPLE_SERIAL_CONFIG_USB)
    while (1) {
        ESP_LOGI(TAG, "Initializing esp_modem for the SIM7070 module...");
        struct esp_modem_usb_term_config usb_config = ESP_MODEM_DEFAULT_USB_CONFIG(0x1e0e, 0x9206, 2); // VID, PID and interface num of SIM7070 modem
        const esp_modem_dte_config_t dte_usb_config = ESP_MODEM_DTE_DEFAULT_USB_CONFIG(usb_config);
        ESP_LOGI(TAG, "Waiting for USB device connection...");
        esp_modem_dce_t *dce = esp_modem_new_dev_usb(ESP_MODEM_DCE_SIM7070, &dte_usb_config, &dce_config, esp_netif);
        assert(dce);
        esp_modem_set_error_cb(dce, usb_terminal_error_handler);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Although the DTE should be ready after USB enumeration, sometimes it fails to respond without this delay

#else
#error Invalid serial connection to modem.
#endif

    xEventGroupClearBits(event_group, CONNECT_BIT | GOT_DATA_BIT | USB_DISCONNECTED_BIT);

    /* Run the modem demo app */
#if CONFIG_EXAMPLE_NEED_SIM_PIN == 1
    // check if PIN needed
    bool pin_ok = false;
    if (esp_modem_read_pin(dce, &pin_ok) == ESP_OK && pin_ok == false) {
        if (esp_modem_set_pin(dce, CONFIG_EXAMPLE_SIM_PIN) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            abort();
        }
    }
#endif

    int rssi, ber;
    esp_err_t err = esp_modem_get_signal_quality(dce, &rssi, &ber);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_get_signal_quality failed with %d %s", err, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Signal quality: rssi=%d, ber=%d", rssi, ber);

#if CONFIG_EXAMPLE_SEND_MSG
    if (esp_modem_sms_txt_mode(dce, true) != ESP_OK || esp_modem_sms_character_set(dce) != ESP_OK) {
        ESP_LOGE(TAG, "Setting text mode or GSM character set failed");
        return;
    }

    err = esp_modem_send_sms(dce, CONFIG_EXAMPLE_SEND_MSG_PEER_PHONE_NUMBER, "Text message from esp-modem");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_send_sms() failed with %d", err);
        return;
    }
#endif

    err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_DATA) failed with %d", err);
        return;
    }
    /* Wait for IP address */
    ESP_LOGI(TAG, "Waiting for IP address");
    xEventGroupWaitBits(event_group, CONNECT_BIT | USB_DISCONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    CHECK_USB_DISCONNECTION(event_group);

    /* Config MQTT */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
    };
#else
    esp_mqtt_client_config_t mqtt_config = {
        .uri = CONFIG_EXAMPLE_MQTT_BROKER_URI,
    };
#endif
    esp_mqtt_client_handle_t mqtt_client = esp_mqtt_client_init(&mqtt_config);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    ESP_LOGI(TAG, "Waiting for MQTT data");
    xEventGroupWaitBits(event_group, GOT_DATA_BIT | USB_DISCONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    CHECK_USB_DISCONNECTION(event_group);

    esp_mqtt_client_destroy(mqtt_client);
    err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_set_mode(ESP_MODEM_MODE_COMMAND) failed with %d", err);
        return;
    }
    char imsi[32];
    err = esp_modem_get_imsi(dce, imsi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_modem_get_imsi failed with %d", err);
        return;
    }
    ESP_LOGI(TAG, "IMSI=%s", imsi);

#if defined(CONFIG_EXAMPLE_SERIAL_CONFIG_USB)
    // USB example runs in a loop to demonstrate hot-plugging and sudden disconnection features.
    ESP_LOGI(TAG, "USB demo finished. Disconnect and connect the modem to run it again");
    xEventGroupWaitBits(event_group, USB_DISCONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    CHECK_USB_DISCONNECTION(event_group); // dce will be destroyed here
} // while (1)
#else
    // UART DTE clean-up
    esp_modem_destroy(dce);
    esp_netif_destroy(esp_netif);
#endif
}
