#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

// ===== CONFIGURACION AUTOMATICA =====
#define WIFI_SSID      "iva"
#define WIFI_PASS      "ivasalinas"
#define GITHUB_USER    "NathanCanS"

// URLs de GitHub
#define FIRMWARE_URL   "https://github.com/" GITHUB_USER "/esp32-firmware/raw/main/firmware/latest/firmware.bin"
#define VERSION_URL    "https://github.com/" GITHUB_USER "/esp32-firmware/raw/main/version.txt"

// Configuracion de reintentos
#define WIFI_MAXIMUM_RETRY  10
#define OTA_CHECK_INTERVAL  30

static const char *TAG = "OTA_APP";
static int s_retry_num = 0;
static bool wifi_connected = false;
static esp_netif_t *sta_netif = NULL;

// Event handler para WiFi con DNS
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_connected = false;
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Reconectando WiFi... intento %d/%d", s_retry_num, WIFI_MAXIMUM_RETRY);
        } else {
            ESP_LOGE(TAG, "Fallo al conectar WiFi despues de %d intentos", WIFI_MAXIMUM_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "WiFi conectado! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&event->ip_info.gw));
        ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&event->ip_info.netmask));

        // Configurar DNS servers
        esp_netif_dns_info_t dns_info;

        // DNS Primario: Google
        dns_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info));

        // DNS Secundario: Google
        dns_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 4, 4);
        ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_BACKUP, &dns_info));

        // DNS Tercero: Cloudflare
        dns_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
        ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_FALLBACK, &dns_info));

        ESP_LOGI(TAG, "DNS configurado: 8.8.8.8, 8.8.4.4, 1.1.1.1");

        s_retry_num = 0;
        wifi_connected = true;

        // Verificar DNS inmediatamente
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        struct hostent *res = gethostbyname("github.com");
        if (res) {
            ESP_LOGI(TAG, "DNS funcionando! GitHub IP: %s",
                     inet_ntoa(*(struct in_addr*)res->h_addr_list[0]));
        } else {
            ESP_LOGE(TAG, "DNS no puede resolver github.com");
        }
    }
}

// Inicializar WiFi con configuracion DNS mejorada
void wifi_init_sta(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Crear interfaz con configuracion especifica
    sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Registrar event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi iniciado. Conectando a SSID: %s", WIFI_SSID);
}

// Verificar conexion a internet
bool check_internet_connection(void)
{
    ESP_LOGI(TAG, "Verificando conexion a internet...");

    // Primero verificar que podemos resolver DNS
    struct hostent *host_entry = gethostbyname("google.com");
    if (!host_entry) {
        ESP_LOGE(TAG, "No se puede resolver DNS");
        return false;
    }

    ESP_LOGI(TAG, "DNS OK, Google IP: %s", inet_ntoa(*(struct in_addr*)host_entry->h_addr_list[0]));

    // Crear socket para verificar conectividad
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = ((struct in_addr*)host_entry->h_addr_list[0])->s_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(80);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "No se pudo crear socket");
        return false;
    }

    // Configurar timeout
    struct timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Intentar conectar
    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    close(sock);

    if (err != 0) {
        ESP_LOGE(TAG, "No hay conexion a internet ^(error: %d^)", err);
        return false;
    }

    ESP_LOGI(TAG, "Conexion a internet OK!");
    return true;
}

// Realizar actualizacion OTA
void perform_ota_update(void)
{
    ESP_LOGI(TAG, "Iniciando actualizacion OTA...");
    ESP_LOGI(TAG, "URL: %s", FIRMWARE_URL);

    esp_http_client_config_t config = {
        .url = FIRMWARE_URL,
        .timeout_ms = 60000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .bulk_flash_erase = false,
        .partial_http_download = true,
        .max_http_request_size = 0,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin fallo: %s", esp_err_to_name(err));
        return;
    }

    int image_len = 0;
    int last_percent = -1;

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }

        // Mostrar progreso
        image_len = esp_https_ota_get_image_len_read(https_ota_handle);
        int image_total = esp_https_ota_get_image_size(https_ota_handle);
        if (image_total > 0) {
            int percent = (image_len * 100) / image_total;
            if (percent != last_percent) {
                ESP_LOGI(TAG, "Descargando: %d%%", percent);
                last_percent = percent;
            }
        }
    }

    if (err == ESP_OK) {
        err = esp_https_ota_finish(https_ota_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "OTA EXITOSO! Reiniciando en 3 segundos...");
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            esp_restart();
        } else {
            ESP_LOGE(TAG, "esp_https_ota_finish fallo: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "esp_https_ota_perform fallo: %s", esp_err_to_name(err));
        esp_https_ota_abort(https_ota_handle);
    }
}

// Funcion principal de verificacion y actualizacion
void check_and_update(void)
{
    if (!wifi_connected) {
        ESP_LOGW(TAG, "WiFi no conectado");
        return;
    }

    // Verificar conexion a internet
    if (!check_internet_connection()) {
        ESP_LOGE(TAG, "No hay acceso a internet");
        return;
    }

    // Pequena pausa para estabilizar
    vTaskDelay(2000 / portTICK_PERIOD_MS);

    // Realizar OTA directamente
    perform_ota_update();
}

void app_main(void)
{
    // Informacion del firmware
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "======================================");
    ESP_LOGI(TAG, "ESP32 OTA GitHub Demo");
    ESP_LOGI(TAG, "Version: %s", app_desc->version);
    ESP_LOGI(TAG, "Proyecto: %s", app_desc->project_name);
    ESP_LOGI(TAG, "Compilado: %s %s", app_desc->date, app_desc->time);
    ESP_LOGI(TAG, "======================================");

    // Inicializar NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Informacion de la particion
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Ejecutando desde: %s", running->label);

    // Validar firmware si es necesario
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Firmware pendiente de verificacion...");
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            ESP_LOGI(TAG, "Firmware funcionando correctamente, validando...");
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Firmware validado");
        }
    }

    // Inicializar WiFi
    wifi_init_sta();

    // Aplicacion principal
    int contador = 0;
    bool first_check_done = false;

    while (1) {
        ESP_LOGI(TAG, "Aplicacion ejecutandose... [%d]", contador);

        // Primera verificacion despues de 10 segundos
        if (!first_check_done && contador == 5 && wifi_connected) {
            first_check_done = true;
            ESP_LOGI(TAG, "Primera verificacion de actualizaciones...");
            check_and_update();
        }

        // Verificaciones periodicas cada 60 segundos
        if (contador > 0 && contador % OTA_CHECK_INTERVAL == 0 && wifi_connected) {
            check_and_update();
        }

        contador++;
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}
