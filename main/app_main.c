/* Nim's playground.
 * BSD-2-Clause.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "nvs_flash.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_spi_flash.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "mqtt_client.h"

#include "lora.h"


#define TTGO_LED 2
#define TTGO_BTN 0
#define LORA_RECEIVER 0

#define ESP_INTR_FLAG_DEFAULT 0

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
    /*printf("LORA RECEIVER 0.\n");*/
    lora_receive();
    while (lora_received()) {
      lora_len = lora_receive_packet(lora_buf, sizeof(lora_buf));
      lora_buf[lora_len] = 0;
      ESP_LOGI(TAG, "LoRa packet received: %s", lora_buf);
      ESP_LOGI(TAG, "LoRa RSSI: %i, SNR: %f", lora_packet_rssi(), lora_packet_snr());
      esp_mqtt_client_publish(client, "ttgo/receiver", "got pkt", 0, 0, 0);
      esp_mqtt_client_publish(client, "ttgo/receiver", (char *)lora_buf, 0, 0, 0);
      lora_receive();
    }
    vTaskDelay(1);
#else // LoRa sender
    printf("LORA RECEIVER 1.\n");
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    lora_send_packet((uint8_t *)"plop", 5);
    ESP_LOGI(TAG, "LoRa packet sent");
    ESP_LOGI(TAG, "LoRa RSSI: %i, SNR: %f", lora_packet_rssi(), lora_packet_snr());
    esp_mqtt_client_publish(client, "ttgo/sender", "sent pkt", 0, 0, 0);
#endif
  }
}

static void IRAM_ATTR gpio_isr_handler(void *arg) {
  uint32_t gpio_num = (uint32_t)arg;
  xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void log_error_if_nonzero(const char * message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
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

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_publish(client, "/topic/qos1", "data_3", 0, 1, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "/topic/qos0", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_subscribe(client, "/topic/qos1", 1);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            msg_id = esp_mqtt_client_unsubscribe(client, "/topic/qos1");
            ESP_LOGI(TAG, "sent unsubscribe successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "/topic/qos0", "data", 0, 0, 0);
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
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
                log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
                log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
                ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));

            }
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void) {
  esp_mqtt_client_config_t mqtt_cfg = {
      .uri = CONFIG_BROKER_URL,
  };

  client = esp_mqtt_client_init(&mqtt_cfg);
  esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
  esp_mqtt_client_start(client);
}

void app_main(void) {
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_EXAMPLE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(example_connect());

  ESP_LOGI(TAG, "++ esp mqtt app start");
  mqtt_app_start();
  ESP_LOGI(TAG, "++ esp lora");
  lora_init();
  ESP_LOGI(TAG, "++ esp WiFi, MQTT & LoRa connected");

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
