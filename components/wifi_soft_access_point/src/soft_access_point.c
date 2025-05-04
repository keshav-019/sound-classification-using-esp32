/**
 * @file soft_access_point.c
 * @brief WiFi Soft Access Point (AP) and Station (STA) implementation
 * 
 * This file provides functionality to:
 * - Create a WiFi Access Point
 * - Handle client connection events
 * - Configure DNS settings
 * - Manage both AP and STA modes simultaneously
 */

 #include "soft_access_point.h"

 // Logging tags
 static const char *TAG_AP = "WiFi SoftAP";  ///< Tag for Access Point related logs
 static const char *TAG_STA = "WiFi Sta";    ///< Tag for Station related logs
 
 // Connection retry counter
 static int s_retry_num = 0;  ///< Tracks WiFi connection retry attempts
 
 // Event group for WiFi connection status
 static EventGroupHandle_t s_wifi_event_group;  ///< FreeRTOS event group for WiFi status
 
 /**
  * @brief WiFi event handler for both AP and STA modes
  * @param arg User data (unused)
  * @param event_base Type of event (WIFI_EVENT/IP_EVENT)
  * @param event_id Specific event ID
  * @param event_data Event-specific data
  * 
  * Handles:
  * - Client connection/disconnection to AP
  * - Station mode startup
  * - IP address acquisition
  * 
  * @note This is a static callback function registered with ESP event loop
  */
 static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
 {
     // Handle AP client connections
     if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
         wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
         ESP_LOGI(TAG_AP, "Station "MACSTR" joined, AID=%d",
                  MAC2STR(event->mac), event->aid);
     } 
     // Handle AP client disconnections
     else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
         wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
         ESP_LOGI(TAG_AP, "Station "MACSTR" left, AID=%d, reason:%d",
                  MAC2STR(event->mac), event->aid, event->reason);
     } 
     // Handle Station mode startup
     else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
         esp_wifi_connect();
         ESP_LOGI(TAG_STA, "Station started");
     } 
     // Handle IP address acquisition
     else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
         ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
         ESP_LOGI(TAG_STA, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
         s_retry_num = 0;
         xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
     }
 }
 
 /**
  * @brief Initializes WiFi in SoftAP mode
  * @return Pointer to created AP network interface
  * 
  * Steps:
  * 1. Creates default AP network interface
  * 2. Configures AP settings (SSID, password, channel)
  * 3. Sets authentication mode (WPA2 or Open)
  * 4. Applies the configuration
  * 
  * @note Uses EXAMPLE_ESP_WIFI_* defines for configuration
  */
 esp_netif_t *wifi_init_softap(void)
 {
     // Step 1: Create default AP interface
     esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();
 
     // Step 2: Configure AP settings
     wifi_config_t wifi_ap_config = {
         .ap = {
             .ssid = EXAMPLE_ESP_WIFI_AP_SSID,
             .ssid_len = strlen(EXAMPLE_ESP_WIFI_AP_SSID),
             .channel = EXAMPLE_ESP_WIFI_CHANNEL,
             .password = EXAMPLE_ESP_WIFI_AP_PASSWD,
             .max_connection = EXAMPLE_MAX_STA_CONN,
             .authmode = WIFI_AUTH_WPA2_PSK,
             .pmf_cfg = {
                 .required = false,
             },
         },
     };
 
     // Step 3: Handle open network case
     if (strlen(EXAMPLE_ESP_WIFI_AP_PASSWD) == 0) {
         wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
     }
 
     // Step 4: Apply configuration
     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
 
     ESP_LOGI(TAG_AP, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
              EXAMPLE_ESP_WIFI_AP_SSID, EXAMPLE_ESP_WIFI_AP_PASSWD, EXAMPLE_ESP_WIFI_CHANNEL);
 
     return esp_netif_ap;
 }
 
 /**
  * @brief Configures DNS server for SoftAP using STA's DNS
  * @param esp_netif_ap AP network interface
  * @param esp_netif_sta STA network interface
  * 
  * Steps:
  * 1. Gets DNS info from STA interface
  * 2. Stops DHCP server on AP
  * 3. Sets DNS offer option
  * 4. Applies DNS settings
  * 5. Restarts DHCP server
  */
 void softap_set_dns_addr(esp_netif_t *esp_netif_ap, esp_netif_t *esp_netif_sta)
 {
     // Step 1: Get DNS info from STA
     esp_netif_dns_info_t dns;
     esp_netif_get_dns_info(esp_netif_sta, ESP_NETIF_DNS_MAIN, &dns);
     
     // Step 2: Configure DHCP options
     uint8_t dhcps_offer_option = DHCPS_OFFER_DNS;
     
     // Step 3: Update AP DNS settings
     ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(esp_netif_ap));
     ESP_ERROR_CHECK(esp_netif_dhcps_option(esp_netif_ap, ESP_NETIF_OP_SET, 
                  ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_offer_option, sizeof(dhcps_offer_option)));
     ESP_ERROR_CHECK(esp_netif_set_dns_info(esp_netif_ap, ESP_NETIF_DNS_MAIN, &dns));
     ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(esp_netif_ap));
 }
 
 /**
  * @brief Creates and configures a WiFi Soft Access Point
  * 
  * Complete initialization sequence:
  * 1. Initialize network stack
  * 2. Setup WiFi with default config
  * 3. Set AP+STA mode
  * 4. Configure AP interface with static IP
  * 5. Set AP parameters
  * 6. Optional STA configuration
  * 7. Start WiFi
  * 8. Log configuration
  * 
  * @note Uses EXAMPLE_ESP_WIFI_* defines for SSID/password
  */
 void soft_access_create(void)
 {
     // Step 1: Initialize TCP/IP stack
     ESP_ERROR_CHECK(esp_netif_init());
 
     // Step 2: Initialize WiFi with default config
     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
     ESP_ERROR_CHECK(esp_wifi_init(&cfg));
 
     // Step 3: Set WiFi mode to AP+STA
     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
 
     // Step 4: Create and configure AP interface
     esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();
     
     // Configure static IP for AP (important for web server)
     esp_netif_ip_info_t ip_info;
     IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
     IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
     IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
     
     // Apply IP configuration
     esp_netif_dhcps_stop(esp_netif_ap);
     esp_netif_set_ip_info(esp_netif_ap, &ip_info);
     esp_netif_dhcps_start(esp_netif_ap);
 
     // Step 5: Configure AP parameters
     wifi_config_t wifi_ap_config = {
         .ap = {
             .ssid = EXAMPLE_ESP_WIFI_AP_SSID,
             .ssid_len = strlen(EXAMPLE_ESP_WIFI_AP_SSID),
             .channel = EXAMPLE_ESP_WIFI_CHANNEL,
             .password = EXAMPLE_ESP_WIFI_AP_PASSWD,
             .max_connection = EXAMPLE_MAX_STA_CONN,
             .authmode = WIFI_AUTH_WPA2_PSK,
             .pmf_cfg = {
                 .required = false,
             },
         },
     };
     
     // Handle open network case
     if (strlen(EXAMPLE_ESP_WIFI_AP_PASSWD) == 0) {
         wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
     }
 
     ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));
 
     // Step 6: Optional STA configuration
     esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();
     
     // Step 7: Start WiFi
     ESP_ERROR_CHECK(esp_wifi_start());
 
     // Step 8: Log configuration
     ESP_LOGI(TAG_AP, "WiFi AP initialized");
     ESP_LOGI(TAG_AP, "SSID: %s", EXAMPLE_ESP_WIFI_AP_SSID);
     ESP_LOGI(TAG_AP, "Password: %s", EXAMPLE_ESP_WIFI_AP_PASSWD);
     ESP_LOGI(TAG_AP, "IP Address: 192.168.4.1");
 }