#include "web_status.h"

#include "esp_avb.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_status";
static const char *NVS_NS = "webnet";
static httpd_handle_t s_httpd;
static esp_netif_t *s_netif;

#define WEB_LOG_RING_SIZE 16384
#define WEB_HEARTBEAT_GPIO GPIO_NUM_23

static char s_log_ring[WEB_LOG_RING_SIZE];
static size_t s_log_head;
static size_t s_log_len;
static portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;
static bool s_log_hooked;
static volatile bool s_heartbeat_enabled;
static TaskHandle_t s_heartbeat_task;

static esp_err_t ensure_nvs(void);

static void web_log_ring_append(const char *data, size_t len) {
  if (!data || len == 0)
    return;
  portENTER_CRITICAL(&s_log_mux);
  for (size_t i = 0; i < len; i++) {
    s_log_ring[s_log_head] = data[i];
    s_log_head = (s_log_head + 1) % WEB_LOG_RING_SIZE;
    if (s_log_len < WEB_LOG_RING_SIZE)
      s_log_len++;
  }
  portEXIT_CRITICAL(&s_log_mux);
}

static int web_log_vprintf(const char *fmt, va_list ap) {
  va_list ap_copy;
  va_copy(ap_copy, ap);
  int ret = s_prev_vprintf ? s_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
  char line[256];
  int n = vsnprintf(line, sizeof(line), fmt, ap_copy);
  va_end(ap_copy);
  if (n > 0)
    web_log_ring_append(line, n < (int)sizeof(line) ? (size_t)n
                                                    : sizeof(line) - 1);
  return ret;
}

void web_status_log_init(void) {
  if (s_log_hooked)
    return;
  s_prev_vprintf = esp_log_set_vprintf(web_log_vprintf);
  s_log_hooked = true;
}

static esp_err_t load_heartbeat_config(bool *enabled) {
  if (!enabled)
    return ESP_ERR_INVALID_ARG;
  *enabled = false;
  ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs init failed");
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND)
    return ESP_OK;
  if (err != ESP_OK)
    return err;
  uint8_t value = 0;
  if (nvs_get_u8(h, "heartbeat", &value) == ESP_OK)
    *enabled = value != 0;
  nvs_close(h);
  return ESP_OK;
}

static esp_err_t save_heartbeat_config(bool enabled) {
  ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs init failed");
  nvs_handle_t h;
  ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG,
                      "nvs open failed");
  esp_err_t err = nvs_set_u8(h, "heartbeat", enabled ? 1 : 0);
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err;
}

static void heartbeat_task(void *arg) {
  (void)arg;
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << WEB_HEARTBEAT_GPIO,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
  gpio_set_level(WEB_HEARTBEAT_GPIO, 0);

  while (1) {
    if (s_heartbeat_enabled) {
      gpio_set_level(WEB_HEARTBEAT_GPIO, 1);
      vTaskDelay(pdMS_TO_TICKS(80));
      gpio_set_level(WEB_HEARTBEAT_GPIO, 0);
      vTaskDelay(pdMS_TO_TICKS(920));
    } else {
      gpio_set_level(WEB_HEARTBEAT_GPIO, 0);
      vTaskDelay(pdMS_TO_TICKS(250));
    }
  }
}

static void heartbeat_start(void) {
  bool enabled = false;
  if (load_heartbeat_config(&enabled) != ESP_OK)
    enabled = false;
  s_heartbeat_enabled = enabled;
  if (!s_heartbeat_task) {
    xTaskCreatePinnedToCore(heartbeat_task, "WEB-HB", 2048, NULL, 1,
                            &s_heartbeat_task, 0);
  }
}

typedef struct {
  bool dhcp;
  char ip[16];
  char netmask[16];
  char gateway[16];
  char dns[16];
} web_net_config_t;

static esp_err_t ensure_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  return err;
}

static void default_net_config(web_net_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->dhcp = false;
  snprintf(cfg->ip, sizeof(cfg->ip), "%s",
           CONFIG_EXAMPLE_WEB_DEFAULT_STATIC_IP);
  snprintf(cfg->netmask, sizeof(cfg->netmask), "%s",
           CONFIG_EXAMPLE_WEB_DEFAULT_NETMASK);
  snprintf(cfg->gateway, sizeof(cfg->gateway), "%s",
           CONFIG_EXAMPLE_WEB_DEFAULT_GATEWAY);
  snprintf(cfg->dns, sizeof(cfg->dns), "%s", CONFIG_EXAMPLE_WEB_DEFAULT_DNS);
}

static esp_err_t load_net_config(web_net_config_t *cfg, bool *found) {
  default_net_config(cfg);
  if (found)
    *found = false;
  ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs init failed");
  nvs_handle_t h;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
  if (err == ESP_ERR_NVS_NOT_FOUND)
    return ESP_OK;
  if (err != ESP_OK)
    return err;
  uint8_t dhcp = cfg->dhcp ? 1 : 0;
  if (nvs_get_u8(h, "dhcp", &dhcp) == ESP_OK) {
    cfg->dhcp = dhcp != 0;
    if (found)
      *found = true;
  }
  size_t len;
  len = sizeof(cfg->ip);
  nvs_get_str(h, "ip", cfg->ip, &len);
  len = sizeof(cfg->netmask);
  nvs_get_str(h, "netmask", cfg->netmask, &len);
  len = sizeof(cfg->gateway);
  nvs_get_str(h, "gateway", cfg->gateway, &len);
  len = sizeof(cfg->dns);
  nvs_get_str(h, "dns", cfg->dns, &len);
  nvs_close(h);
  return ESP_OK;
}

static esp_err_t save_net_config(const web_net_config_t *cfg) {
  ESP_RETURN_ON_ERROR(ensure_nvs(), TAG, "nvs init failed");
  nvs_handle_t h;
  ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG,
                      "nvs open failed");
  esp_err_t err = nvs_set_u8(h, "dhcp", cfg->dhcp ? 1 : 0);
  if (err == ESP_OK)
    err = nvs_set_str(h, "ip", cfg->ip);
  if (err == ESP_OK)
    err = nvs_set_str(h, "netmask", cfg->netmask);
  if (err == ESP_OK)
    err = nvs_set_str(h, "gateway", cfg->gateway);
  if (err == ESP_OK)
    err = nvs_set_str(h, "dns", cfg->dns);
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err;
}

static bool parse_ip(const char *s, esp_ip4_addr_t *out) {
  return s && ip4addr_aton(s, (ip4_addr_t *)out);
}

esp_err_t web_status_apply_network_config(esp_netif_t *netif) {
  if (!netif)
    return ESP_ERR_INVALID_ARG;
  web_net_config_t cfg;
  bool found = false;
  ESP_RETURN_ON_ERROR(load_net_config(&cfg, &found), TAG,
                      "load network config failed");
  if (cfg.dhcp) {
    ESP_LOGI(TAG, "network config: DHCP");
    return ESP_OK;
  }

  esp_netif_ip_info_t ip = {0};
  esp_netif_dns_info_t dns = {0};
  if (!parse_ip(cfg.ip, &ip.ip) || !parse_ip(cfg.netmask, &ip.netmask) ||
      !parse_ip(cfg.gateway, &ip.gw) || !parse_ip(cfg.dns, &dns.ip.u_addr.ip4)) {
    ESP_LOGW(TAG, "invalid saved static IP config, leaving DHCP enabled");
    return ESP_OK;
  }
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcpc_stop(netif));
  ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &ip), TAG,
                      "set static ip failed");
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  ESP_ERROR_CHECK_WITHOUT_ABORT(
      esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
  ESP_LOGI(TAG, "network config: static %s/%s gw %s dns %s%s", cfg.ip,
           cfg.netmask, cfg.gateway, cfg.dns, found ? "" : " (default)");
  return ESP_OK;
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} json_buf_t;

static int jb_reserve(json_buf_t *jb, size_t extra) {
  if (jb->len + extra + 1 <= jb->cap)
    return 0;
  size_t ncap = jb->cap ? jb->cap : 1024;
  while (jb->len + extra + 1 > ncap)
    ncap *= 2;
  char *nbuf = realloc(jb->buf, ncap);
  if (!nbuf)
    return -1;
  jb->buf = nbuf;
  jb->cap = ncap;
  return 0;
}

static int jb_appendf(json_buf_t *jb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0 || jb_reserve(jb, (size_t)need) != 0) {
    va_end(ap2);
    return -1;
  }
  vsnprintf(jb->buf + jb->len, jb->cap - jb->len, fmt, ap2);
  va_end(ap2);
  jb->len += (size_t)need;
  return 0;
}

static int jb_append_json_str(json_buf_t *jb, const char *s) {
  if (jb_appendf(jb, "\"") != 0)
    return -1;
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') {
      if (jb_appendf(jb, "\\%c", c) != 0)
        return -1;
    } else if (c >= 0x20) {
      if (jb_reserve(jb, 1) != 0)
        return -1;
      jb->buf[jb->len++] = (char)c;
      jb->buf[jb->len] = 0;
    }
  }
  return jb_appendf(jb, "\"");
}

static bool json_bool_value(const char *body, const char *key, bool *out) {
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(body, pat);
  if (!p)
    return false;
  p = strchr(p, ':');
  if (!p)
    return false;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (strncmp(p, "true", 4) == 0) {
    *out = true;
    return true;
  }
  if (strncmp(p, "false", 5) == 0) {
    *out = false;
    return true;
  }
  return false;
}

static bool json_string_value(const char *body, const char *key, char *out,
                              size_t out_len) {
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(body, pat);
  if (!p)
    return false;
  p = strchr(p, ':');
  if (!p)
    return false;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p != '"')
    return false;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < out_len) {
    out[n++] = *p++;
  }
  out[n] = 0;
  return *p == '"';
}

static void hex_bytes(char *dst, size_t dst_len, const uint8_t *src,
                      size_t src_len, const char *sep) {
  size_t pos = 0;
  for (size_t i = 0; i < src_len && pos + 3 < dst_len; i++) {
    int n = snprintf(dst + pos, dst_len - pos, "%s%02x", i ? sep : "",
                     src[i]);
    if (n < 0)
      break;
    pos += (size_t)n;
  }
}

static const char *stream_kind(const avb_web_stream_snapshot_s *s) {
  if (s->format[0] == 0x04)
    return "CRF";
  if (s->format[0] == 0x02)
    return "AAF";
  if (s->format[0] == 0x00)
    return "IEC 61883-6";
  return "Unknown";
}

static int append_stream(json_buf_t *jb, const avb_web_stream_snapshot_s *s,
                         bool input) {
  char sid[24], mac[24], peer[24], fmt[32];
  hex_bytes(sid, sizeof(sid), s->stream_id, sizeof(s->stream_id), ":");
  hex_bytes(mac, sizeof(mac), s->stream_dest_addr, sizeof(s->stream_dest_addr),
            ":");
  hex_bytes(peer, sizeof(peer), s->peer_entity_id, sizeof(s->peer_entity_id),
            ":");
  hex_bytes(fmt, sizeof(fmt), s->format, sizeof(s->format), "");
  if (jb_appendf(jb,
                 "{\"index\":%u,\"direction\":\"%s\",\"kind\":\"%s\","
                 "\"connected\":%s,\"pending\":%s,\"streaming\":%s,"
                 "\"vlan\":%u,\"connectionCount\":%u,"
                 "\"presentationOffsetNs\":%lu,\"streamId\":\"%s\","
                 "\"destMac\":\"%s\",\"peerEntityId\":\"%s\","
                 "\"format\":\"%s\"}",
                 (unsigned)s->index, input ? "input" : "output",
                 stream_kind(s), s->connected ? "true" : "false",
                 s->pending_connection ? "true" : "false",
                 s->streaming ? "true" : "false", (unsigned)s->vlan_id,
                 (unsigned)s->connection_count,
                 (unsigned long)s->presentation_time_offset_ns, sid, mac, peer,
                 fmt) != 0)
    return -1;
  return 0;
}

static void append_network(json_buf_t *jb) {
  esp_netif_ip_info_t ip = {0};
  web_net_config_t cfg;
  bool found = false;
  load_net_config(&cfg, &found);
  bool has_ip = s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK;
  if (has_ip) {
    jb_appendf(jb, "\"network\":{\"dhcp\":%s,\"configured\":%s,"
                  "\"ip\":\"" IPSTR "\",\"netmask\":\"" IPSTR
                  "\",\"gateway\":\"" IPSTR "\",\"configIp\":\"%s\","
                  "\"configNetmask\":\"%s\",\"configGateway\":\"%s\","
                  "\"configDns\":\"%s\"}",
               cfg.dhcp ? "true" : "false", found ? "true" : "false",
               IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw), cfg.ip,
               cfg.netmask, cfg.gateway, cfg.dns);
  } else {
    jb_appendf(jb,
               "\"network\":{\"dhcp\":%s,\"configured\":%s,"
               "\"ip\":\"0.0.0.0\",\"netmask\":\"0.0.0.0\","
               "\"gateway\":\"0.0.0.0\",\"configIp\":\"%s\","
               "\"configNetmask\":\"%s\",\"configGateway\":\"%s\","
               "\"configDns\":\"%s\"}",
               cfg.dhcp ? "true" : "false", found ? "true" : "false",
               cfg.ip, cfg.netmask, cfg.gateway, cfg.dns);
  }
}

static esp_err_t send_json(httpd_req_t *req, json_buf_t *jb) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  esp_err_t ret = httpd_resp_send(req, jb->buf ? jb->buf : "{}", jb->len);
  free(jb->buf);
  return ret;
}

static esp_err_t read_body(httpd_req_t *req, char *body, size_t body_len) {
  if (!body || body_len == 0)
    return ESP_ERR_INVALID_ARG;
  int total = 0;
  while (total < req->content_len && total < (int)body_len - 1) {
    int room = (int)body_len - 1 - total;
    int wanted = req->content_len - total;
    if (wanted > room)
      wanted = room;
    int r = httpd_req_recv(req, body + total, wanted);
    if (r <= 0)
      return ESP_FAIL;
    total += r;
  }
  if (total < req->content_len)
    return ESP_ERR_INVALID_SIZE;
  body[total] = 0;
  return ESP_OK;
}

static esp_err_t send_unavailable(httpd_req_t *req) {
  httpd_resp_set_status(req, "503 Service Unavailable");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, "AVB is not running", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req) {
  avb_web_status_snapshot_s snap;
  if (avb_web_status_snapshot(&snap) != ESP_OK)
    return send_unavailable(req);
  json_buf_t jb = {0};
  char eid[24], gm[24];
  hex_bytes(eid, sizeof(eid), snap.entity_id, sizeof(snap.entity_id), ":");
  hex_bytes(gm, sizeof(gm), snap.gm_id, sizeof(snap.gm_id), ":");
  int32_t pll_cppm = (int32_t)(((int64_t)snap.pll_applied_ppm_q16 * 100) >> 16);
  int32_t pll_abs = pll_cppm < 0 ? -pll_cppm : pll_cppm;
  jb_appendf(&jb, "{\"running\":%s,\"entityName\":",
             snap.running ? "true" : "false");
  jb_append_json_str(&jb, snap.entity_name);
  jb_appendf(&jb,
             ",\"entityId\":\"%s\",\"talkerEnabled\":%s,"
             "\"listenerEnabled\":%s,\"clockSourceValid\":%s,"
             "\"avbLite\":%s,\"heartbeatEnabled\":%s,"
             "\"heartbeatGpio\":%d,\"activeClockSource\":%u,"
             "\"pllAppliedPpm\":\"%s%ld.%02ld\",\"ptp\":{\"gmId\":\"%s\","
             "\"stepsRemoved\":%d,\"peerDelayNs\":%ld,\"pathDelayNs\":%ld,"
             "\"driftPpb\":%ld},",
             eid, snap.talker_enabled ? "true" : "false",
             snap.listener_enabled ? "true" : "false",
             snap.clock_source_valid ? "true" : "false",
             snap.avb_lite ? "true" : "false",
             s_heartbeat_enabled ? "true" : "false", WEB_HEARTBEAT_GPIO,
             (unsigned)snap.active_clock_source_index, pll_cppm < 0 ? "-" : "",
             (long)(pll_abs / 100), (long)(pll_abs % 100), gm,
             snap.gm_steps_removed, snap.peer_delay_ns, snap.path_delay_ns,
             snap.drift_ppb);
  append_network(&jb);
  jb_appendf(&jb, "}");
  return send_json(req, &jb);
}

static esp_err_t streams_handler(httpd_req_t *req) {
  avb_web_status_snapshot_s snap;
  if (avb_web_status_snapshot(&snap) != ESP_OK)
    return send_unavailable(req);
  json_buf_t jb = {0};
  jb_appendf(&jb, "{\"inputs\":[");
  for (size_t i = 0; i < snap.num_input_streams; i++) {
    if (i)
      jb_appendf(&jb, ",");
    append_stream(&jb, &snap.input_streams[i], true);
  }
  jb_appendf(&jb, "],\"outputs\":[");
  for (size_t i = 0; i < snap.num_output_streams; i++) {
    if (i)
      jb_appendf(&jb, ",");
    append_stream(&jb, &snap.output_streams[i], false);
  }
  jb_appendf(&jb, "]}");
  return send_json(req, &jb);
}

static esp_err_t matrix_handler(httpd_req_t *req) {
  avb_web_status_snapshot_s snap;
  if (avb_web_status_snapshot(&snap) != ESP_OK)
    return send_unavailable(req);
  json_buf_t jb = {0};
  jb_appendf(&jb, "{\"connections\":[");
  bool first = true;
  for (size_t i = 0; i < snap.num_input_streams; i++) {
    avb_web_stream_snapshot_s *s = &snap.input_streams[i];
    if (!s->connected)
      continue;
    char sid[24], peer[24];
    hex_bytes(sid, sizeof(sid), s->stream_id, sizeof(s->stream_id), ":");
    hex_bytes(peer, sizeof(peer), s->peer_entity_id, sizeof(s->peer_entity_id),
              ":");
    jb_appendf(&jb, "%s{\"sink\":\"input-%u\",\"sourceEntity\":\"%s\","
                  "\"streamId\":\"%s\",\"kind\":\"%s\"}",
               first ? "" : ",", (unsigned)s->index, peer, sid,
               stream_kind(s));
    first = false;
  }
  for (size_t i = 0; i < snap.num_output_streams; i++) {
    avb_web_stream_snapshot_s *s = &snap.output_streams[i];
    if (!s->connected)
      continue;
    char sid[24], mac[24];
    hex_bytes(sid, sizeof(sid), s->stream_id, sizeof(s->stream_id), ":");
    hex_bytes(mac, sizeof(mac), s->stream_dest_addr,
              sizeof(s->stream_dest_addr), ":");
    jb_appendf(&jb, "%s{\"source\":\"output-%u\",\"listeners\":%u,"
                  "\"destMac\":\"%s\",\"streamId\":\"%s\",\"kind\":\"%s\"}",
               first ? "" : ",", (unsigned)s->index,
               (unsigned)s->connection_count, mac, sid, stream_kind(s));
    first = false;
  }
  jb_appendf(&jb, "]}");
  return send_json(req, &jb);
}

static esp_err_t network_get_handler(httpd_req_t *req) {
  json_buf_t jb = {0};
  jb_appendf(&jb, "{");
  append_network(&jb);
  jb_appendf(&jb, "}");
  return send_json(req, &jb);
}

static esp_err_t network_post_handler(httpd_req_t *req) {
  char body[256];
  esp_err_t read_err = read_body(req, body, sizeof(body));
  if (read_err == ESP_ERR_INVALID_SIZE) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    return httpd_resp_send(req, "payload too large", HTTPD_RESP_USE_STRLEN);
  }
  if (read_err != ESP_OK)
    return read_err;

  web_net_config_t cfg;
  default_net_config(&cfg);
  json_bool_value(body, "dhcp", &cfg.dhcp);
  json_string_value(body, "ip", cfg.ip, sizeof(cfg.ip));
  json_string_value(body, "netmask", cfg.netmask, sizeof(cfg.netmask));
  json_string_value(body, "gateway", cfg.gateway, sizeof(cfg.gateway));
  json_string_value(body, "dns", cfg.dns, sizeof(cfg.dns));
  esp_ip4_addr_t tmp;
  if (!cfg.dhcp &&
      (!parse_ip(cfg.ip, &tmp) || !parse_ip(cfg.netmask, &tmp) ||
       !parse_ip(cfg.gateway, &tmp) || !parse_ip(cfg.dns, &tmp))) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "invalid IPv4 config", HTTPD_RESP_USE_STRLEN);
  }
  esp_err_t err = save_net_config(&cfg);
  if (err != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, esp_err_to_name(err), HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(
      req, "{\"ok\":true,\"rebootRequired\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t entity_get_handler(httpd_req_t *req) {
  avb_web_status_snapshot_s snap;
  if (avb_web_status_snapshot(&snap) != ESP_OK)
    return send_unavailable(req);
  json_buf_t jb = {0};
  jb_appendf(&jb, "{\"entityName\":");
  jb_append_json_str(&jb, snap.entity_name);
  jb_appendf(&jb, "}");
  return send_json(req, &jb);
}

static esp_err_t entity_post_handler(httpd_req_t *req) {
  char body[160];
  char name[64];
  esp_err_t read_err = read_body(req, body, sizeof(body));
  if (read_err == ESP_ERR_INVALID_SIZE) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    return httpd_resp_send(req, "payload too large", HTTPD_RESP_USE_STRLEN);
  }
  if (read_err != ESP_OK)
    return read_err;
  if (!json_string_value(body, "entityName", name, sizeof(name)) ||
      name[0] == 0) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "invalid entity name", HTTPD_RESP_USE_STRLEN);
  }
  if (avb_set_entity_name(name) != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "failed to set entity name",
                           HTTPD_RESP_USE_STRLEN);
  }
  json_buf_t jb = {0};
  jb_appendf(&jb, "{\"ok\":true,\"entityName\":");
  jb_append_json_str(&jb, name);
  jb_appendf(&jb, "}");
  return send_json(req, &jb);
}

static esp_err_t heartbeat_get_handler(httpd_req_t *req) {
  json_buf_t jb = {0};
  jb_appendf(&jb, "{\"enabled\":%s,\"gpio\":%d}",
             s_heartbeat_enabled ? "true" : "false", WEB_HEARTBEAT_GPIO);
  return send_json(req, &jb);
}

static esp_err_t heartbeat_post_handler(httpd_req_t *req) {
  char body[64];
  bool enabled = false;
  esp_err_t read_err = read_body(req, body, sizeof(body));
  if (read_err == ESP_ERR_INVALID_SIZE) {
    httpd_resp_set_status(req, "413 Payload Too Large");
    return httpd_resp_send(req, "payload too large", HTTPD_RESP_USE_STRLEN);
  }
  if (read_err != ESP_OK)
    return read_err;
  if (!json_bool_value(body, "enabled", &enabled)) {
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_send(req, "invalid heartbeat config",
                           HTTPD_RESP_USE_STRLEN);
  }
  esp_err_t err = save_heartbeat_config(enabled);
  if (err != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, esp_err_to_name(err), HTTPD_RESP_USE_STRLEN);
  }
  s_heartbeat_enabled = enabled;
  if (!enabled)
    gpio_set_level(WEB_HEARTBEAT_GPIO, 0);

  json_buf_t jb = {0};
  jb_appendf(&jb, "{\"ok\":true,\"enabled\":%s,\"gpio\":%d}",
             enabled ? "true" : "false", WEB_HEARTBEAT_GPIO);
  return send_json(req, &jb);
}

static esp_err_t logs_handler(httpd_req_t *req) {
  char *copy = malloc(WEB_LOG_RING_SIZE + 1);
  if (!copy) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "out of memory", HTTPD_RESP_USE_STRLEN);
  }
  size_t len;
  portENTER_CRITICAL(&s_log_mux);
  len = s_log_len;
  size_t start = (s_log_head + WEB_LOG_RING_SIZE - s_log_len) %
                 WEB_LOG_RING_SIZE;
  for (size_t i = 0; i < len; i++)
    copy[i] = s_log_ring[(start + i) % WEB_LOG_RING_SIZE];
  portEXIT_CRITICAL(&s_log_mux);
  copy[len] = 0;
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  esp_err_t ret = httpd_resp_send(req, copy, len);
  free(copy);
  return ret;
}

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ESP AVB Node</title><style>"
    "body{margin:0;font:14px system-ui,-apple-system,Segoe UI,sans-serif;"
    "background:#f5f6f8;color:#17202a}header{padding:16px 20px;background:#1f2937;"
    "color:white}h1{margin:0;font-size:20px}.sub{opacity:.75;margin-top:4px}"
    "main{padding:16px;display:grid;gap:16px}.grid{display:grid;gap:16px;"
    "grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}.panel{background:white;"
    "border:1px solid #d8dde3;border-radius:8px;padding:14px;box-shadow:0 1px 2px #0001}"
    "h2{font-size:15px;margin:0 0 10px}table{width:100%;border-collapse:collapse}"
    "td,th{padding:7px;border-top:1px solid #e8ebef;text-align:left;vertical-align:top}"
    "th{color:#53606d;font-weight:600}.pill{display:inline-block;padding:2px 7px;"
    "border-radius:999px;background:#e9eef5}.ok{background:#dff5e5}.bad{background:#fde3e3}"
    "code{font-size:12px;word-break:break-all}.muted{color:#65727f}"
    "label{display:grid;gap:4px;margin:8px 0}input{font:inherit;padding:7px;border:1px solid #cbd3dc;border-radius:6px}"
    "button{font:inherit;padding:8px 10px;border:1px solid #9aa6b2;border-radius:6px;background:#fff;cursor:pointer}"
    "pre{margin:0;max-height:360px;overflow:auto;white-space:pre-wrap;background:#111827;color:#e5e7eb;padding:10px;border-radius:6px;font-size:12px}</style></head>"
    "<body><header><h1 id=name>ESP AVB Node</h1><div class=sub id=summary></div></header>"
    "<main><div class=grid><section class=panel><h2>Status</h2><table id=status></table>"
    "</section><section class=panel><h2>Connection Matrix</h2><table id=matrix></table>"
    "</section></div><section class=panel><h2>Talker Streams</h2><table id=outputs></table>"
    "</section><section class=panel><h2>Listener Streams</h2><table id=inputs></table>"
    "</section><div class=grid><section class=panel><h2>Entity</h2><form id=entityform>"
    "<label>Name<input id=entityName maxlength=63></label>"
    "<button>Save entity name</button> <span id=entitymsg class=muted></span></form>"
    "<form id=heartbeatform><label><span><input type=checkbox id=heartbeat> GPIO23 heartbeat</span></label>"
    "<button>Save LED setting</button> <span id=heartbeatmsg class=muted></span></form>"
    "</section><section class=panel><h2>Network</h2><form id=netform>"
    "<label><span><input type=checkbox id=dhcp> DHCP</span></label>"
    "<label>IP<input id=ip></label><label>Netmask<input id=netmask></label>"
    "<label>Gateway<input id=gateway></label><label>DNS<input id=dns></label>"
    "<button>Save network config</button> <span id=netmsg class=muted></span></form>"
    "</section></div><section class=panel><h2>Log Output</h2><pre id=logs></pre>"
    "</section></main><script>"
    "const q=id=>document.getElementById(id);"
    "const get=p=>fetch(p,{cache:'no-store'}).then(r=>{if(!r.ok)throw Error(r.status+' '+p);return r.json()});"
    "const getText=p=>fetch(p,{cache:'no-store'}).then(r=>r.text());"
    "function pill(v){return `<span class='pill ${v?'ok':'bad'}'>${v?'yes':'no'}</span>`}"
    "function rows(o){return Object.entries(o).map(([k,v])=>`<tr><th>${k}</th><td>${v}</td></tr>`).join('')}"
    "function streamTable(a){let body=a.length?a.map(s=>`<tr><td>${s.index}</td><td>${s.kind}</td><td>${pill(s.connected||s.streaming)} ${s.pending?'pending':''}</td><td>${s.vlan}</td><td><code>${s.streamId}</code></td><td><code>${s.format}</code></td></tr>`).join(''):'<tr><td colspan=6>No streams</td></tr>';return '<tr><th>#</th><th>Kind</th><th>State</th><th>VLAN</th><th>Stream ID</th><th>Format</th></tr>'+body}"
    "let netLoaded=false,entityLoaded=false,heartbeatLoaded=false;async function refresh(){try{let st=await get('/api/status');let ss=await get('/api/streams');let mx=await get('/api/matrix');"
    "q('name').textContent=st.entityName||'ESP AVB Node';q('summary').textContent=`${st.network.ip} · entity ${st.entityId}`;"
    "q('status').innerHTML=rows({'Talker':pill(st.talkerEnabled),'Listener':pill(st.listenerEnabled),'PTP locked':pill(st.clockSourceValid),'AVB Lite':pill(st.avbLite),'Heartbeat':pill(st.heartbeatEnabled),'GM':`<code>${st.ptp.gmId}</code>`,'Peer delay':`${st.ptp.peerDelayNs} ns`,'Drift':`${st.ptp.driftPpb} ppb`,'Clock source':st.activeClockSource,'PLL applied':`${st.pllAppliedPpm} ppm`});"
    "q('inputs').innerHTML=streamTable(ss.inputs);q('outputs').innerHTML=streamTable(ss.outputs);"
    "let mr=mx.connections.length?mx.connections.map(c=>`<tr><td>${c.sink||c.source}</td><td>${c.sourceEntity||c.destMac||''}</td><td><code>${c.streamId}</code></td></tr>`).join(''):'<tr><td colspan=3>No active connections</td></tr>';q('matrix').innerHTML='<tr><th>Endpoint</th><th>Peer</th><th>Stream</th></tr>'+mr;"
    "if(!entityLoaded){q('entityName').value=st.entityName||'';entityLoaded=true;}"
    "if(!heartbeatLoaded){q('heartbeat').checked=!!st.heartbeatEnabled;heartbeatLoaded=true;}"
    "if(!netLoaded){let n=st.network;q('dhcp').checked=n.dhcp;q('ip').value=n.configIp;q('netmask').value=n.configNetmask;q('gateway').value=n.configGateway;q('dns').value=n.configDns;netLoaded=true;}}catch(e){q('summary').textContent='refresh failed: '+e.message;}}"
    "async function refreshLogs(){try{let t=await getText('/api/logs');let p=q('logs');let nearBottom=p.scrollTop+p.clientHeight+20>=p.scrollHeight;p.textContent=t;if(nearBottom)p.scrollTop=p.scrollHeight;}catch(e){}}"
    "q('entityform').onsubmit=async e=>{e.preventDefault();let body={entityName:q('entityName').value};let r=await fetch('/api/entity',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body),cache:'no-store'});q('entitymsg').textContent=r.ok?'saved':await r.text();entityLoaded=false;refresh();};"
    "q('heartbeatform').onsubmit=async e=>{e.preventDefault();let body={enabled:q('heartbeat').checked};let r=await fetch('/api/heartbeat',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body),cache:'no-store'});q('heartbeatmsg').textContent=r.ok?'saved':await r.text();heartbeatLoaded=false;refresh();};"
    "q('netform').onsubmit=async e=>{e.preventDefault();let body={dhcp:q('dhcp').checked,ip:q('ip').value,netmask:q('netmask').value,gateway:q('gateway').value,dns:q('dns').value};let r=await fetch('/api/network',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});q('netmsg').textContent=r.ok?'saved, reboot required':await r.text();};"
    "refresh();refreshLogs();setInterval(refresh,1000);setInterval(refreshLogs,2000);</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_status_start(esp_netif_t *netif) {
  if (s_httpd)
    return ESP_OK;
  s_netif = netif;
  heartbeat_start();
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = 80;
  cfg.ctrl_port = 32768;
  cfg.stack_size = 6144;
  cfg.core_id = 0;
  cfg.max_uri_handlers = 14;
  esp_err_t ret = httpd_start(&s_httpd, &cfg);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "failed to start HTTP server: %s", esp_err_to_name(ret));
    return ret;
  }
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/", .method = HTTP_GET, .handler = index_handler}), fail, TAG, "register / failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/status", .method = HTTP_GET, .handler = status_handler}), fail, TAG, "register status failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/streams", .method = HTTP_GET, .handler = streams_handler}), fail, TAG, "register streams failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/matrix", .method = HTTP_GET, .handler = matrix_handler}), fail, TAG, "register matrix failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/network", .method = HTTP_GET, .handler = network_get_handler}), fail, TAG, "register network get failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/network", .method = HTTP_POST, .handler = network_post_handler}), fail, TAG, "register network post failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/entity", .method = HTTP_GET, .handler = entity_get_handler}), fail, TAG, "register entity get failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/entity", .method = HTTP_POST, .handler = entity_post_handler}), fail, TAG, "register entity post failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/heartbeat", .method = HTTP_GET, .handler = heartbeat_get_handler}), fail, TAG, "register heartbeat get failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/heartbeat", .method = HTTP_POST, .handler = heartbeat_post_handler}), fail, TAG, "register heartbeat post failed");
  ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_httpd, &(httpd_uri_t){.uri = "/api/logs", .method = HTTP_GET, .handler = logs_handler}), fail, TAG, "register logs failed");
  ESP_LOGI(TAG, "HTTP status server started on port %u", cfg.server_port);
  return ESP_OK;
fail:
  httpd_stop(s_httpd);
  s_httpd = NULL;
  return ret;
}
