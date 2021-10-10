/* Nim's playground.
 * BSD-2-Clause.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "lora.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#define TTGO_LED 2
#define TTGO_BTN 0
#define EXAMPLE_BROKER_URL "mqtt://nim:ju6ZaeGhnooSh4Na@192.168.8.111:1884"
#define EXAMPLE_ESP_WIFI_SSID "azv"
#define EXAMPLE_ESP_WIFI_PASSWORD "PetitElephantDeviendraGrand8D"
#define EXAMPLE_ESP_MAXIMUM_RETRY 2
#define LORA_RECEIVER 0

#define ESP_INTR_FLAG_DEFAULT 0

static EventGroupHandle_t s_wifi_event_group;
static const int WIFI_CONNECTED_BIT = BIT0;
static int s_retry_num = 0;
static xQueueHandle gpio_evt_queue = NULL;
static esp_mqtt_client_handle_t client = NULL;

#if LORA_RECEIVER == 0
static int lora_len;
static uint8_t lora_buf[32];
static const char *TAG = "ttgo_receiver";
#else
static const char *TAG = "ttgo_sender";
#endif

static void lora_task(void *p) {
  for (;;) {
#if LORA_RECEIVER == 0 // LoRa Receiver
    lora_receive();
    while (lora_received()) {
      lora_len = lora_receive_packet(lora_buf, sizeof(lora_buf));
      lora_buf[lora_len] = 0;
      ESP_LOGI(TAG, "LoRa packet received: %s", lora_buf);
      ESP_LOGI(TAG, "LoRa RSSI: %i, SNR: %f", lora_packet_rssi(), lora_packet_snr());
      lora_receive();
    }
    vTaskDelay(1);
#else // LoRa sender
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    lora_send_packet((uint8_t *)"plop", 5);
    ESP_LOGI(TAG, "LoRa packet sent");
    ESP_LOGI(TAG, "LoRa RSSI: %i, SNR: %f", lora_packet_rssi(), lora_packet_snr());
#endif
  }
}

static void IRAM_ATTR gpio_isr_handler(void *arg) {
  uint32_t gpio_num = (uint32_t)arg;
  xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task(void *arg) {
  uint32_t io_num;
  for (;;) {
    if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
      printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
      if (client != NULL)
        esp_mqtt_client_publish(client, "ttgo/pub", "btn", 0, 1, 0);
    }
  }
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
  int msg_id;
  // your_context_t *context = event->context;
  switch (event->event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    msg_id = esp_mqtt_client_publish(client, "ttgo/pub", "hop", 0, 1, 0);
    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

    msg_id = esp_mqtt_client_subscribe(client, "ttgo/sub", 1);
    ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    break;

  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
    msg_id = esp_mqtt_client_publish(client, "ttgo/pub", "oke", 0, 1, 0);
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
    msg_id = esp_mqtt_client_publish(client, "ttgo/pub", event->data,
                                     event->data_len, 1, 0);
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
    break;
  default:
    ESP_LOGI(TAG, "Other event id:%d", event->event_id);
    break;
  }
  return ESP_OK;
}

static void mqtt_app_start(void) {
  esp_mqtt_client_config_t mqtt_cfg = {
      .uri = EXAMPLE_BROKER_URL, .event_handle = mqtt_event_handler,
      // .user_context = (void *)your_context
  };

  client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_start(client);
}

static esp_err_t event_handler(void *ctx, system_event_t *event) {
  switch (event->event_id) {
  case SYSTEM_EVENT_STA_START:
    esp_wifi_connect();
    break;
  case SYSTEM_EVENT_STA_GOT_IP:
    ESP_LOGI(TAG, "got ip:%s",
             ip4addr_ntoa(&event->event_info.got_ip.ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    break;
  case SYSTEM_EVENT_STA_DISCONNECTED: {
    if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
      esp_wifi_connect();
      xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    }
    ESP_LOGI(TAG, "connect to the AP fail\n");
    break;
  }
  default:
    break;
  }
  return ESP_OK;
}

void wifi_init_sta() {
  s_wifi_event_group = xEventGroupCreate();

  tcpip_adapter_init();
  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  wifi_config_t wifi_config = {
      .sta = {.ssid = EXAMPLE_ESP_WIFI_SSID, .password = EXAMPLE_ESP_WIFI_PASSWORD},
  };

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_ERROR_CHECK(tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_STA, TAG));

  ESP_LOGI(TAG, "wifi_init_sta finished.");
  ESP_LOGI(TAG, "connect to ap SSID:%s password:%s", EXAMPLE_ESP_WIFI_SSID,
           EXAMPLE_ESP_WIFI_PASSWORD);
}

void app_main(void) {
  printf("Hello Baroustan blink!\n");

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
  wifi_init_sta();
  mqtt_app_start();
  lora_init();
  ESP_LOGI(TAG, "WiFi, MQTT & LoRa connected");

  lora_set_frequency(868e6);
  lora_enable_crc();
  xTaskCreate(lora_task, "lora_task", 2048, NULL, 9, NULL);

  ESP_ERROR_CHECK(gpio_set_direction(TTGO_LED, GPIO_MODE_OUTPUT));
  ESP_ERROR_CHECK(gpio_set_direction(TTGO_BTN, GPIO_MODE_INPUT));
  ESP_ERROR_CHECK(gpio_set_level(TTGO_LED, 0));
  ESP_ERROR_CHECK(gpio_set_intr_type(TTGO_BTN, GPIO_INTR_NEGEDGE));
  gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
  xTaskCreate(gpio_task, "gpio_task", 2048, NULL, 10, NULL);
  ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT));
  ESP_ERROR_CHECK(
      gpio_isr_handler_add(TTGO_BTN, gpio_isr_handler, (void *)TTGO_BTN));

  for (int i = 60; i >= 0; i--) {
    for (int j = 0; j <= i; j++) {
      gpio_set_level(TTGO_LED, 1);
      vTaskDelay(100 / portTICK_PERIOD_MS);
      gpio_set_level(TTGO_LED, 0);
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    printf("Restarting in %d...\n", i);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  printf("Restarting now.\n");
  fflush(stdout);
  esp_restart();
}
