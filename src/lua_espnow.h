#ifndef LUA_ESPNOW_H
#define LUA_ESPNOW_H

#include <Arduino.h>

// Forward declaration for Lua
struct lua_State;

// ESP-NOW v2 max payload (ESP-IDF 5.x): 1470 bytes
// v1 legacy limit was 250 bytes
#define ESPNOW_MAX_DATA_LEN 1470

// Queue sizes (reduced for v2 since each slot is larger)
#define ESPNOW_RECV_QUEUE_SIZE 10
#define ESPNOW_SEND_QUEUE_SIZE 5

// MAC address length
#define ESPNOW_MAC_LEN 6

// Callback types for C API
typedef void (*espnow_recv_cb_t)(const uint8_t *mac, const uint8_t *data, int len);
typedef void (*espnow_send_cb_t)(const uint8_t *mac, bool success);

// ============================================================================
// C API — Core
// ============================================================================
bool espnow_init_c(uint8_t channel = 1);
void espnow_deinit_c(void);
bool espnow_is_initialized_c(void);

// ============================================================================
// C API — Peer Management
// ============================================================================
bool espnow_add_peer_c(const uint8_t *mac, uint8_t channel = 0, bool encrypt = false, const uint8_t *lmk = nullptr);
bool espnow_del_peer_c(const uint8_t *mac);
bool espnow_mod_peer_c(const uint8_t *mac, uint8_t channel, bool encrypt, const uint8_t *lmk = nullptr);
bool espnow_peer_exists_c(const uint8_t *mac);
bool espnow_peer_count_c(int *total, int *encrypted);

// ============================================================================
// C API — Send
// ============================================================================
bool espnow_send_c(const uint8_t *mac, const uint8_t *data, size_t len);
bool espnow_broadcast_c(const uint8_t *data, size_t len);

// ============================================================================
// C API — Callbacks (C-level, fires in WiFi task context)
// ============================================================================
void espnow_set_on_receive_c(espnow_recv_cb_t cb);
void espnow_set_on_send_c(espnow_send_cb_t cb);

// ============================================================================
// C API — Configuration
// ============================================================================
bool espnow_set_pmk_c(const uint8_t *pmk);
bool espnow_set_channel_c(uint8_t channel);
uint8_t espnow_get_channel_c(void);
uint32_t espnow_get_version_c(void);

// ============================================================================
// C API — MAC Utilities
// ============================================================================
bool espnow_get_mac_c(uint8_t *mac_out);
const char* espnow_get_mac_str_c(void);
bool espnow_mac_str_to_bytes_c(const char *str, uint8_t *out);
const char* espnow_mac_bytes_to_str_c(const uint8_t *mac);

// ============================================================================
// C API — Queue Processing (call from Lua task)
// ============================================================================
void espnow_poll_c(void);

// ============================================================================
// Lua Module Registration
// ============================================================================
int luaopen_espnow(lua_State *L, const char *module_name);

#endif // LUA_ESPNOW_H
