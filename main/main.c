/**
 * Lab 7.3 - Full Weather Station Integration
 * ESP32-C3-DevKit-RUST-1
 * 
 * 1. GET location from server
 * 2. Fetch outdoor temperature from wttr.in for that location
 * 3. Read onboard SHTC3 sensor temperature
 * 4. POST both temperatures to server
 * 5. Log all information
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

// ============== CONFIGURATION ==============
// iPhone Hotspot credentials
#define WIFI_SSID      "iPhone"
#define WIFI_PASS      "your_password"

// Server configuration - your Pi's IP on the hotspot
#define SERVER_IP      "172.20.10.2"
#define SERVER_PORT    1234

// ============== CONSTANTS ==============
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

// SHTC3 I2C configuration
#define I2C_PORT            0
#define I2C_MASTER_SCL_IO   8
#define I2C_MASTER_SDA_IO   10
#define I2C_MASTER_FREQ_HZ  400000
#define SHTC3_ADDR          0x70

// SHTC3 commands
#define SHTC3_WAKEUP        0x3517
#define SHTC3_SLEEP         0xB098
#define SHTC3_READ_T_FIRST  0x7CA2

static const char *TAG = "LAB7_3";

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

// I2C handles
static i2c_master_bus_handle_t i2c_bus_handle = NULL;
static i2c_master_dev_handle_t shtc3_handle = NULL;

// CRC-8 for SHTC3: poly 0x31, init 0xFF
static uint8_t calculate_crc(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// ============== WiFi Functions ==============

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi!");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to connect to WiFi");
    return ESP_FAIL;
}

// ============== I2C / SHTC3 Functions ==============

static esp_err_t i2c_master_init(void)
{
    gpio_pullup_en(I2C_MASTER_SDA_IO);
    gpio_pullup_en(I2C_MASTER_SCL_IO);

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus_handle));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDR,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &dev_config, &shtc3_handle));

    ESP_LOGI(TAG, "I2C initialized");
    return ESP_OK;
}

static esp_err_t shtc3_send_command(uint16_t command)
{
    uint8_t cmd[2] = { (uint8_t)(command >> 8), (uint8_t)(command & 0xFF) };
    return i2c_master_transmit(shtc3_handle, cmd, sizeof(cmd), pdMS_TO_TICKS(1000));
}

static esp_err_t shtc3_read_temperature(float *temperature)
{
    // Wake up
    if (shtc3_send_command(SHTC3_WAKEUP) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to wake SHTC3");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // Start measurement
    if (shtc3_send_command(SHTC3_READ_T_FIRST) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start measurement");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(13));

    // Read temperature (3 bytes: MSB, LSB, CRC)
    uint8_t data[3];
    esp_err_t ret = i2c_master_receive(shtc3_handle, data, 3, pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read temperature data");
        return ret;
    }

    // Verify CRC
    uint8_t crc = calculate_crc(data, 2);
    if (crc != data[2]) {
        ESP_LOGE(TAG, "Temperature CRC mismatch");
        return ESP_FAIL;
    }

    // Calculate temperature
    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    *temperature = -45.0f + 175.0f * ((float)raw / 65535.0f);

    // Sleep
    shtc3_send_command(SHTC3_SLEEP);

    return ESP_OK;
}

// ============== HTTP Functions ==============

/**
 * GET /location from server
 */
static esp_err_t get_location(char *location, size_t max_len)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return ESP_FAIL;
    }

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket connect failed to %s:%d", SERVER_IP, SERVER_PORT);
        close(sock);
        return ESP_FAIL;
    }

    char request[256];
    snprintf(request, sizeof(request),
        "GET /location HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: esp-idf/1.0 esp32 curl\r\n"
        "\r\n",
        SERVER_IP, SERVER_PORT);

    ESP_LOGI(TAG, "GET /location from %s:%d", SERVER_IP, SERVER_PORT);

    if (write(sock, request, strlen(request)) < 0) {
        ESP_LOGE(TAG, "Socket write failed");
        close(sock);
        return ESP_FAIL;
    }

    char recv_buf[512];
    char full_response[512] = {0};
    int total_len = 0;

    while (1) {
        int len = read(sock, recv_buf, sizeof(recv_buf) - 1);
        if (len <= 0) break;
        recv_buf[len] = '\0';
        if (total_len + len < sizeof(full_response) - 1) {
            strcat(full_response, recv_buf);
            total_len += len;
        }
    }
    close(sock);

    // Find body after headers
    char *body = strstr(full_response, "\r\n\r\n");
    if (body) {
        body += 4;
        // Trim whitespace
        while (*body == ' ' || *body == '\r' || *body == '\n') body++;
        char *end = body + strlen(body) - 1;
        while (end > body && (*end == ' ' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        strncpy(location, body, max_len - 1);
        location[max_len - 1] = '\0';
        ESP_LOGI(TAG, "Got location: %s", location);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to parse location response");
    return ESP_FAIL;
}

/**
 * GET weather from wttr.in
 */
static esp_err_t get_weather(const char *location, float *outdoor_temp)
{
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;

    int err = getaddrinfo("wttr.in", "80", &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed for wttr.in");
        return ESP_FAIL;
    }

    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        freeaddrinfo(res);
        return ESP_FAIL;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "Socket connect failed to wttr.in");
        close(sock);
        freeaddrinfo(res);
        return ESP_FAIL;
    }
    freeaddrinfo(res);

    // URL-encode location (replace spaces with +)
    char url_location[64];
    strncpy(url_location, location, sizeof(url_location) - 1);
    for (char *p = url_location; *p; p++) {
        if (*p == ' ') *p = '+';
    }

    char request[256];
    snprintf(request, sizeof(request),
        "GET /%s?format=%%t HTTP/1.0\r\n"
        "Host: wttr.in:80\r\n"
        "User-Agent: esp-idf/1.0 esp32 curl\r\n"
        "\r\n",
        url_location);

    ESP_LOGI(TAG, "GET weather for %s from wttr.in", location);

    if (write(sock, request, strlen(request)) < 0) {
        ESP_LOGE(TAG, "Socket write failed");
        close(sock);
        return ESP_FAIL;
    }

    char recv_buf[256];
    char full_response[512] = {0};
    int total_len = 0;

    while (1) {
        int len = read(sock, recv_buf, sizeof(recv_buf) - 1);
        if (len <= 0) break;
        recv_buf[len] = '\0';
        if (total_len + len < sizeof(full_response) - 1) {
            strcat(full_response, recv_buf);
            total_len += len;
        }
    }
    close(sock);

    char *body = strstr(full_response, "\r\n\r\n");
    if (body) {
        body += 4;
        // Skip leading whitespace and +
        while (*body == ' ' || *body == '+') body++;
        *outdoor_temp = atof(body);
        ESP_LOGI(TAG, "Outdoor temperature: %.1f°C", *outdoor_temp);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to parse weather response");
    return ESP_FAIL;
}

/**
 * POST weather data to server
 */
static esp_err_t post_weather(const char *location, float outdoor_temp, float sensor_temp)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(SERVER_PORT);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return ESP_FAIL;
    }

    if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket connect failed");
        close(sock);
        return ESP_FAIL;
    }

    char body[256];
    snprintf(body, sizeof(body), "location=%s&outdoor_temp=%.2f&sensor_temp=%.2f",
             location, outdoor_temp, sensor_temp);
    int body_len = strlen(body);

    char request[512];
    snprintf(request, sizeof(request),
        "POST /weather HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: esp-idf/1.0 esp32 curl\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s",
        SERVER_IP, SERVER_PORT, body_len, body);

    ESP_LOGI(TAG, "POST /weather to %s:%d", SERVER_IP, SERVER_PORT);

    if (write(sock, request, strlen(request)) < 0) {
        ESP_LOGE(TAG, "Socket write failed");
        close(sock);
        return ESP_FAIL;
    }

    char recv_buf[256];
    read(sock, recv_buf, sizeof(recv_buf) - 1);
    close(sock);

    return ESP_OK;
}

// ============== Main Task ==============

static void weather_task(void *pvParameters)
{
    char location[64];
    float outdoor_temp;
    float sensor_temp;

    while (1) {
        ESP_LOGI(TAG, "--- Starting weather cycle ---");

        // Step 1: Get location from server
        if (get_location(location, sizeof(location)) != ESP_OK) {
            strcpy(location, "Santa Cruz");  // Default fallback
            ESP_LOGW(TAG, "Using default location: %s", location);
        }

        // Step 2: Get outdoor temperature from wttr.in
        if (get_weather(location, &outdoor_temp) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get weather");
            outdoor_temp = 0;
        }

        // Step 3: Read sensor temperature
        if (shtc3_read_temperature(&sensor_temp) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read SHTC3");
            sensor_temp = 0;
        }

        // Step 4: Log everything on ESP32
        ESP_LOGI(TAG, "================================================");
        ESP_LOGI(TAG, "           WEATHER STATION REPORT");
        ESP_LOGI(TAG, "================================================");
        ESP_LOGI(TAG, "  Location:              %s", location);
        ESP_LOGI(TAG, "  Outdoor Temperature:   %.2f°C", outdoor_temp);
        ESP_LOGI(TAG, "  Sensor Temperature:    %.2f°C", sensor_temp);
        ESP_LOGI(TAG, "================================================");

        // Step 5: POST to server
        if (post_weather(location, outdoor_temp, sensor_temp) == ESP_OK) {
            ESP_LOGI(TAG, "Posted to server successfully");
        } else {
            ESP_LOGE(TAG, "Failed to post to server");
        }

        ESP_LOGI(TAG, "Waiting 5 seconds...\n");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ============== Main ==============

void app_main(void)
{
    ESP_LOGI(TAG, "Lab 7.3 - Full Weather Station Starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(i2c_master_init());

    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed. Halting.");
        return;
    }

    xTaskCreate(&weather_task, "weather_task", 8192, NULL, 5, NULL);
}
