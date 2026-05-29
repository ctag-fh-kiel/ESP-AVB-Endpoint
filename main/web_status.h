#pragma once

#include "esp_err.h"
#include "esp_netif.h"

esp_err_t web_status_apply_network_config(esp_netif_t *netif);
void web_status_log_init(void);
esp_err_t web_status_start(esp_netif_t *netif);
