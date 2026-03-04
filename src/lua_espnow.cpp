#include "lua_espnow.h"

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_coexist.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>

extern "C" {
    #include "lua.h"
    #include "lualib.h"
    #include "lauxlib.h"
}

// ============================================================================
// Type-check macros (same pattern as lua_lcd)
// ============================================================================
#define LUA_CHECK_INT32(L, n)   ((int32_t)luaL_checknumber(L, n))
#define LUA_CHECK_UINT8(L, n)   ((uint8_t)luaL_checknumber(L, n))
#define LUA_OPT_INT32(L, n, d)  ((int32_t)luaL_optnumber(L, n, d))
#define LUA_OPT_UINT8(L, n, d)  ((uint8_t)luaL_optnumber(L, n, d))

// ============================================================================
// Internal state
// ============================================================================
static bool s_initialized = false;
static uint8_t s_channel = 1;
static char s_mac_str_buf[18]; // "AA:BB:CC:DD:EE:FF\0"

// Broadcast address
static const uint8_t BROADCAST_MAC[ESPNOW_MAC_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// C-level callbacks (fire in WiFi task context)
static espnow_recv_cb_t s_c_recv_cb = nullptr;
static espnow_send_cb_t s_c_send_cb = nullptr;

// Lua state pointer (set during luaopen_espnow)
static lua_State *s_lua_state = nullptr;

// Lua callback references
static int s_lua_recv_ref = LUA_NOREF;
static int s_lua_send_ref = LUA_NOREF;

// ============================================================================
// Queue message types
// ============================================================================

// Heap-allocated receive message (v2 payloads up to 1470 bytes)
typedef struct {
    uint8_t mac[ESPNOW_MAC_LEN];
    uint8_t *data;  // heap-allocated, freed by consumer
    int len;
    // Radio metadata from rx_ctrl
    int8_t rssi;
    int8_t noise_floor;
    uint8_t channel;
    uint32_t timestamp;     // microseconds, local RX time
} espnow_recv_msg_t;

typedef struct {
    uint8_t mac[ESPNOW_MAC_LEN];
    bool success;
} espnow_send_status_t;

// FreeRTOS queues (queue holds pointers for recv, structs for send)
static QueueHandle_t s_recv_queue = nullptr;
static QueueHandle_t s_send_queue = nullptr;

// ============================================================================
// Internal ESP-NOW callbacks (run in WiFi task context)
// ============================================================================

// ESP-IDF 5.x uses esp_now_recv_info_t
static void espnow_recv_cb_internal(const esp_now_recv_info_t *info, const uint8_t *data, int data_len) {
    if (info == nullptr || data == nullptr || data_len <= 0) return;

    // Fire C callback directly (user's responsibility for thread safety)
    if (s_c_recv_cb) {
        s_c_recv_cb(info->src_addr, data, data_len);
    }

    // Enqueue for Lua processing — heap-allocate data copy
    if (s_recv_queue != nullptr) {
        int copy_len = (data_len > ESPNOW_MAX_DATA_LEN) ? ESPNOW_MAX_DATA_LEN : data_len;
        uint8_t *data_copy = (uint8_t *)malloc(copy_len);
        if (data_copy == nullptr) return; // OOM, drop packet

        memcpy(data_copy, data, copy_len);

        espnow_recv_msg_t msg;
        memcpy(msg.mac, info->src_addr, ESPNOW_MAC_LEN);
        msg.data = data_copy;
        msg.len = copy_len;

        // Capture radio metadata from rx_ctrl
        if (info->rx_ctrl) {
            msg.rssi = info->rx_ctrl->rssi;
            msg.noise_floor = info->rx_ctrl->noise_floor;
            msg.channel = info->rx_ctrl->channel;
            msg.timestamp = info->rx_ctrl->timestamp;
        } else {
            msg.rssi = 0;
            msg.noise_floor = 0;
            msg.channel = 0;
            msg.timestamp = 0;
        }

        if (xQueueSend(s_recv_queue, &msg, 0) != pdTRUE) {
            free(data_copy); // queue full, drop packet
        }
    }
}

static void espnow_send_cb_internal(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (mac_addr == nullptr) return;

    // Fire C callback directly
    if (s_c_send_cb) {
        s_c_send_cb(mac_addr, status == ESP_NOW_SEND_SUCCESS);
    }

    // Enqueue for Lua processing
    if (s_send_queue != nullptr) {
        espnow_send_status_t msg;
        memcpy(msg.mac, mac_addr, ESPNOW_MAC_LEN);
        msg.success = (status == ESP_NOW_SEND_SUCCESS);
        xQueueSend(s_send_queue, &msg, 0);
    }
}

// ============================================================================
// C API — Core
// ============================================================================

bool espnow_init_c(uint8_t channel) {
    if (s_initialized) return true;

    // --- ESP32-S3 BLE coexistence-safe WiFi init ---
    // Use AP+STA mode: STA for ESP-NOW RX stability with BLE,
    // AP component gives us channel control without router dependency.
    wifi_mode_t currentMode;
    esp_err_t err = esp_wifi_get_mode(&currentMode);

    if (err == ESP_ERR_WIFI_NOT_INIT) {
        // WiFi not started yet — full init
        WiFi.mode(WIFI_AP_STA);
    } else if (currentMode == WIFI_MODE_NULL || currentMode == WIFI_MODE_STA) {
        // Upgrade to AP+STA for coexistence stability
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    // If already APSTA or AP, leave it alone

    // Don't persist WiFi config to flash
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    // Set channel (must match all ESP-NOW peers)
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    s_channel = channel;

    // Enable modem sleep — REQUIRED when BLE and WiFi share the radio.
    // Without this, ESP-IDF raises "Should enable WiFi modem sleep" error.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    // Disable WiFi 11b long-range mode to reduce radio contention
    esp_wifi_config_11b_rate(WIFI_IF_STA, true);

    // Initialize ESP-NOW
    err = esp_now_init();
    if (err != ESP_OK) {
        Serial.printf("[ESPNOW] esp_now_init failed: 0x%x\n", err);
        return false;
    }

    // Create queues
    s_recv_queue = xQueueCreate(ESPNOW_RECV_QUEUE_SIZE, sizeof(espnow_recv_msg_t));
    s_send_queue = xQueueCreate(ESPNOW_SEND_QUEUE_SIZE, sizeof(espnow_send_status_t));

    if (s_recv_queue == nullptr || s_send_queue == nullptr) {
        Serial.println("[ESPNOW] Failed to create queues");
        esp_now_deinit();
        if (s_recv_queue) { vQueueDelete(s_recv_queue); s_recv_queue = nullptr; }
        if (s_send_queue) { vQueueDelete(s_send_queue); s_send_queue = nullptr; }
        return false;
    }

    // Register internal callbacks
    esp_now_register_recv_cb(espnow_recv_cb_internal);
    esp_now_register_send_cb(espnow_send_cb_internal);

    // Log ESP-NOW version
    uint32_t version = 0;
    esp_now_get_version(&version);

    s_initialized = true;
    Serial.printf("[ESPNOW] Initialized on channel %d (v%lu, AP+STA, modem-sleep)\n",
                  channel, (unsigned long)version);
    return true;
}

void espnow_deinit_c(void) {
    if (!s_initialized) return;

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();

    // Drain recv queue and free heap-allocated data
    if (s_recv_queue) {
        espnow_recv_msg_t msg;
        while (xQueueReceive(s_recv_queue, &msg, 0) == pdTRUE) {
            free(msg.data);
        }
        vQueueDelete(s_recv_queue);
        s_recv_queue = nullptr;
    }
    if (s_send_queue) { vQueueDelete(s_send_queue); s_send_queue = nullptr; }

    s_c_recv_cb = nullptr;
    s_c_send_cb = nullptr;

    // Release Lua callback refs
    if (s_lua_state != nullptr) {
        if (s_lua_recv_ref != LUA_NOREF) {
            luaL_unref(s_lua_state, LUA_REGISTRYINDEX, s_lua_recv_ref);
            s_lua_recv_ref = LUA_NOREF;
        }
        if (s_lua_send_ref != LUA_NOREF) {
            luaL_unref(s_lua_state, LUA_REGISTRYINDEX, s_lua_send_ref);
            s_lua_send_ref = LUA_NOREF;
        }
    }

    s_initialized = false;
    Serial.println("[ESPNOW] Deinitialized");
}

bool espnow_is_initialized_c(void) {
    return s_initialized;
}

// ============================================================================
// C API — Peer Management
// ============================================================================

bool espnow_add_peer_c(const uint8_t *mac, uint8_t channel, bool encrypt, const uint8_t *lmk) {
    if (!s_initialized || mac == nullptr) return false;

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, ESPNOW_MAC_LEN);
    peer.channel = channel;
    peer.encrypt = encrypt;
    if (encrypt && lmk != nullptr) {
        memcpy(peer.lmk, lmk, 16);
    }

    esp_err_t err = esp_now_add_peer(&peer);
    if (err != ESP_OK) {
        Serial.printf("[ESPNOW] add_peer failed: 0x%x\n", err);
        return false;
    }
    return true;
}

bool espnow_del_peer_c(const uint8_t *mac) {
    if (!s_initialized || mac == nullptr) return false;

    esp_err_t err = esp_now_del_peer(mac);
    if (err != ESP_OK) {
        Serial.printf("[ESPNOW] del_peer failed: 0x%x\n", err);
        return false;
    }
    return true;
}

bool espnow_mod_peer_c(const uint8_t *mac, uint8_t channel, bool encrypt, const uint8_t *lmk) {
    if (!s_initialized || mac == nullptr) return false;

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, ESPNOW_MAC_LEN);
    peer.channel = channel;
    peer.encrypt = encrypt;
    if (encrypt && lmk != nullptr) {
        memcpy(peer.lmk, lmk, 16);
    }

    esp_err_t err = esp_now_mod_peer(&peer);
    if (err != ESP_OK) {
        Serial.printf("[ESPNOW] mod_peer failed: 0x%x\n", err);
        return false;
    }
    return true;
}

bool espnow_peer_exists_c(const uint8_t *mac) {
    if (!s_initialized || mac == nullptr) return false;
    return esp_now_is_peer_exist(mac);
}

bool espnow_peer_count_c(int *total, int *encrypted) {
    if (!s_initialized) return false;

    esp_now_peer_num_t num;
    esp_err_t err = esp_now_get_peer_num(&num);
    if (err != ESP_OK) return false;

    if (total) *total = num.total_num;
    if (encrypted) *encrypted = num.encrypt_num;
    return true;
}

// ============================================================================
// C API — Send
// ============================================================================

bool espnow_send_c(const uint8_t *mac, const uint8_t *data, size_t len) {
    if (!s_initialized || data == nullptr || len == 0) return false;
    if (len > ESPNOW_MAX_DATA_LEN) return false;

    // mac == nullptr means broadcast
    esp_err_t err = esp_now_send(mac, data, len);
    if (err != ESP_OK) {
        Serial.printf("[ESPNOW] send failed: 0x%x\n", err);
        return false;
    }
    return true;
}

bool espnow_broadcast_c(const uint8_t *data, size_t len) {
    if (!s_initialized || data == nullptr || len == 0) return false;
    if (len > ESPNOW_MAX_DATA_LEN) return false;

    // Ensure broadcast peer exists
    if (!esp_now_is_peer_exist(BROADCAST_MAC)) {
        esp_now_peer_info_t peer;
        memset(&peer, 0, sizeof(peer));
        memcpy(peer.peer_addr, BROADCAST_MAC, ESPNOW_MAC_LEN);
        peer.channel = 0; // use current channel
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    return espnow_send_c(BROADCAST_MAC, data, len);
}

// ============================================================================
// C API — Callbacks
// ============================================================================

void espnow_set_on_receive_c(espnow_recv_cb_t cb) {
    s_c_recv_cb = cb;
}

void espnow_set_on_send_c(espnow_send_cb_t cb) {
    s_c_send_cb = cb;
}

// ============================================================================
// C API — Configuration
// ============================================================================

bool espnow_set_pmk_c(const uint8_t *pmk) {
    if (!s_initialized || pmk == nullptr) return false;
    esp_err_t err = esp_now_set_pmk(pmk);
    return (err == ESP_OK);
}

bool espnow_set_channel_c(uint8_t channel) {
    if (channel < 1 || channel > 14) return false;
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err == ESP_OK) {
        s_channel = channel;
        return true;
    }
    return false;
}

uint8_t espnow_get_channel_c(void) {
    uint8_t primary;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    return primary;
}

uint32_t espnow_get_version_c(void) {
    uint32_t version = 0;
    esp_now_get_version(&version);
    return version;
}

// ============================================================================
// C API — MAC Utilities
// ============================================================================

bool espnow_get_mac_c(uint8_t *mac_out) {
    if (mac_out == nullptr) return false;
    return (esp_wifi_get_mac(WIFI_IF_STA, mac_out) == ESP_OK);
}

const char* espnow_get_mac_str_c(void) {
    uint8_t mac[ESPNOW_MAC_LEN];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) {
        return "00:00:00:00:00:00";
    }
    snprintf(s_mac_str_buf, sizeof(s_mac_str_buf),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return s_mac_str_buf;
}

bool espnow_mac_str_to_bytes_c(const char *str, uint8_t *out) {
    if (str == nullptr || out == nullptr) return false;
    int vals[6];
    int matched = sscanf(str, "%x:%x:%x:%x:%x:%x",
                         &vals[0], &vals[1], &vals[2],
                         &vals[3], &vals[4], &vals[5]);
    if (matched != 6) return false;
    for (int i = 0; i < 6; i++) {
        out[i] = (uint8_t)vals[i];
    }
    return true;
}

const char* espnow_mac_bytes_to_str_c(const uint8_t *mac) {
    if (mac == nullptr) return "00:00:00:00:00:00";
    snprintf(s_mac_str_buf, sizeof(s_mac_str_buf),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return s_mac_str_buf;
}

// ============================================================================
// C API — Queue Processing
// ============================================================================

void espnow_poll_c(void) {
    if (!s_initialized || s_lua_state == nullptr) return;

    // Process receive queue
    espnow_recv_msg_t recv_msg;
    while (xQueueReceive(s_recv_queue, &recv_msg, 0) == pdTRUE) {
        if (s_lua_recv_ref != LUA_NOREF) {
            lua_rawgeti(s_lua_state, LUA_REGISTRYINDEX, s_lua_recv_ref);
            lua_pushstring(s_lua_state, espnow_mac_bytes_to_str_c(recv_msg.mac));
            lua_pushlstring(s_lua_state, (const char *)recv_msg.data, recv_msg.len);

            // 3rd arg: radio info table {rssi, noise_floor, channel, timestamp}
            lua_createtable(s_lua_state, 0, 4);
            lua_pushinteger(s_lua_state, recv_msg.rssi);
            lua_setfield(s_lua_state, -2, "rssi");
            lua_pushinteger(s_lua_state, recv_msg.noise_floor);
            lua_setfield(s_lua_state, -2, "noise_floor");
            lua_pushinteger(s_lua_state, recv_msg.channel);
            lua_setfield(s_lua_state, -2, "channel");
            lua_pushinteger(s_lua_state, recv_msg.timestamp);
            lua_setfield(s_lua_state, -2, "timestamp");

            if (lua_pcall(s_lua_state, 3, 0, 0) != LUA_OK) {
                Serial.printf("[ESPNOW] Lua recv callback error: %s\n",
                              lua_tostring(s_lua_state, -1));
                lua_pop(s_lua_state, 1);
            }
        }
        free(recv_msg.data);
    }

    // Process send status queue
    espnow_send_status_t send_msg;
    while (xQueueReceive(s_send_queue, &send_msg, 0) == pdTRUE) {
        if (s_lua_send_ref != LUA_NOREF) {
            lua_rawgeti(s_lua_state, LUA_REGISTRYINDEX, s_lua_send_ref);
            lua_pushstring(s_lua_state, espnow_mac_bytes_to_str_c(send_msg.mac));
            lua_pushboolean(s_lua_state, send_msg.success);
            if (lua_pcall(s_lua_state, 2, 0, 0) != LUA_OK) {
                Serial.printf("[ESPNOW] Lua send callback error: %s\n",
                              lua_tostring(s_lua_state, -1));
                lua_pop(s_lua_state, 1);
            }
        }
    }
}

// ============================================================================
// Lua Wrappers
// ============================================================================

// -- Core --

static int l_init(lua_State *L) {
    uint8_t channel = LUA_OPT_UINT8(L, 1, 1);
    lua_pushboolean(L, espnow_init_c(channel));
    return 1;
}

static int l_deinit(lua_State *L) {
    espnow_deinit_c();
    return 0;
}

static int l_is_initialized(lua_State *L) {
    lua_pushboolean(L, espnow_is_initialized_c());
    return 1;
}

// -- Peer Management --

// espnow.add_peer(mac)
// espnow.add_peer(mac, channel)
// espnow.add_peer(mac, channel, encrypt)
// espnow.add_peer(mac, channel, encrypt, lmk)
//
// Defaults: channel=0 (current), encrypt=false, lmk=nil
static int l_add_peer(lua_State *L) {
    const char *mac_str = luaL_checkstring(L, 1);
    uint8_t channel = LUA_OPT_UINT8(L, 2, 0);
    bool encrypt = lua_isnoneornil(L, 3) ? false : lua_toboolean(L, 3);
    const char *lmk_str = luaL_optstring(L, 4, nullptr);

    uint8_t mac[ESPNOW_MAC_LEN];
    if (!espnow_mac_str_to_bytes_c(mac_str, mac)) {
        luaL_error(L, "invalid MAC format, expected AA:BB:CC:DD:EE:FF");
        return 0;
    }

    const uint8_t *lmk = nullptr;
    if (lmk_str != nullptr) {
        size_t lmk_len = strlen(lmk_str);
        if (lmk_len != 16) {
            luaL_error(L, "LMK must be exactly 16 bytes (got %d)", (int)lmk_len);
            return 0;
        }
        lmk = (const uint8_t *)lmk_str;
    }

    lua_pushboolean(L, espnow_add_peer_c(mac, channel, encrypt, lmk));
    return 1;
}

static int l_del_peer(lua_State *L) {
    const char *mac_str = luaL_checkstring(L, 1);
    uint8_t mac[ESPNOW_MAC_LEN];
    if (!espnow_mac_str_to_bytes_c(mac_str, mac)) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, espnow_del_peer_c(mac));
    return 1;
}

// espnow.mod_peer(mac)
// espnow.mod_peer(mac, channel)
// espnow.mod_peer(mac, channel, encrypt)
// espnow.mod_peer(mac, channel, encrypt, lmk)
//
// Defaults: channel=0 (current), encrypt=false, lmk=nil
static int l_mod_peer(lua_State *L) {
    const char *mac_str = luaL_checkstring(L, 1);
    uint8_t channel = LUA_OPT_UINT8(L, 2, 0);
    bool encrypt = lua_isnoneornil(L, 3) ? false : lua_toboolean(L, 3);
    const char *lmk_str = luaL_optstring(L, 4, nullptr);

    uint8_t mac[ESPNOW_MAC_LEN];
    if (!espnow_mac_str_to_bytes_c(mac_str, mac)) {
        luaL_error(L, "invalid MAC format, expected AA:BB:CC:DD:EE:FF");
        return 0;
    }

    const uint8_t *lmk = nullptr;
    if (lmk_str != nullptr) {
        size_t lmk_len = strlen(lmk_str);
        if (lmk_len != 16) {
            luaL_error(L, "LMK must be exactly 16 bytes (got %d)", (int)lmk_len);
            return 0;
        }
        lmk = (const uint8_t *)lmk_str;
    }

    lua_pushboolean(L, espnow_mod_peer_c(mac, channel, encrypt, lmk));
    return 1;
}

static int l_peer_exists(lua_State *L) {
    const char *mac_str = luaL_checkstring(L, 1);
    uint8_t mac[ESPNOW_MAC_LEN];
    if (!espnow_mac_str_to_bytes_c(mac_str, mac)) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_pushboolean(L, espnow_peer_exists_c(mac));
    return 1;
}

static int l_peer_count(lua_State *L) {
    int total = 0, encrypted = 0;
    espnow_peer_count_c(&total, &encrypted);
    lua_pushinteger(L, total);
    lua_pushinteger(L, encrypted);
    return 2;
}

// espnow.get_peers() -> {  {mac="AA:BB:...", channel=1, encrypt=false}, ... }
static int l_get_peers(lua_State *L) {
    if (!s_initialized) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L); // result array
    int idx = 1;

    esp_now_peer_info_t peer;
    // fetch_peer skips broadcast MAC, returns only unicast peers
    bool from_head = true;
    while (esp_now_fetch_peer(from_head, &peer) == ESP_OK) {
        from_head = false; // subsequent calls fetch next

        lua_createtable(L, 0, 3);

        // mac string
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 peer.peer_addr[0], peer.peer_addr[1], peer.peer_addr[2],
                 peer.peer_addr[3], peer.peer_addr[4], peer.peer_addr[5]);
        lua_pushstring(L, mac_str);
        lua_setfield(L, -2, "mac");

        lua_pushinteger(L, peer.channel);
        lua_setfield(L, -2, "channel");

        lua_pushboolean(L, peer.encrypt);
        lua_setfield(L, -2, "encrypt");

        lua_rawseti(L, -2, idx++);
    }

    return 1;
}

// -- Send --

static int l_send(lua_State *L) {
    size_t data_len;
    const uint8_t *mac_ptr = nullptr;
    uint8_t mac[ESPNOW_MAC_LEN];

    // First arg: mac string or nil (broadcast)
    if (!lua_isnil(L, 1) && !lua_isnoneornil(L, 1)) {
        const char *mac_str = luaL_checkstring(L, 1);
        if (!espnow_mac_str_to_bytes_c(mac_str, mac)) {
            lua_pushboolean(L, false);
            return 1;
        }
        mac_ptr = mac;
    }

    const char *data = luaL_checklstring(L, 2, &data_len);
    lua_pushboolean(L, espnow_send_c(mac_ptr, (const uint8_t *)data, data_len));
    return 1;
}

static int l_broadcast(lua_State *L) {
    size_t data_len;
    const char *data = luaL_checklstring(L, 1, &data_len);
    lua_pushboolean(L, espnow_broadcast_c((const uint8_t *)data, data_len));
    return 1;
}

// -- Receive (non-blocking dequeue) --

static int l_receive(lua_State *L) {
    if (!s_initialized || s_recv_queue == nullptr) {
        lua_pushnil(L);
        return 1;
    }

    espnow_recv_msg_t msg;
    if (xQueueReceive(s_recv_queue, &msg, 0) == pdTRUE) {
        lua_pushstring(L, espnow_mac_bytes_to_str_c(msg.mac));
        lua_pushlstring(L, (const char *)msg.data, msg.len);
        free(msg.data);

        // 3rd return: radio info table
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, msg.rssi);
        lua_setfield(L, -2, "rssi");
        lua_pushinteger(L, msg.noise_floor);
        lua_setfield(L, -2, "noise_floor");
        lua_pushinteger(L, msg.channel);
        lua_setfield(L, -2, "channel");
        lua_pushinteger(L, msg.timestamp);
        lua_setfield(L, -2, "timestamp");
        return 3;
    }

    lua_pushnil(L);
    return 1;
}

// -- Callback registration --

static int l_on_receive(lua_State *L) {
    // Release previous ref
    if (s_lua_recv_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_lua_recv_ref);
        s_lua_recv_ref = LUA_NOREF;
    }

    if (lua_isfunction(L, 1)) {
        lua_pushvalue(L, 1);
        s_lua_recv_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int l_on_send(lua_State *L) {
    // Release previous ref
    if (s_lua_send_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_lua_send_ref);
        s_lua_send_ref = LUA_NOREF;
    }

    if (lua_isfunction(L, 1)) {
        lua_pushvalue(L, 1);
        s_lua_send_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

static int l_poll(lua_State *L) {
    espnow_poll_c();
    return 0;
}

// -- Info / Config --

static int l_get_mac(lua_State *L) {
    lua_pushstring(L, espnow_get_mac_str_c());
    return 1;
}

static int l_get_version(lua_State *L) {
    lua_pushinteger(L, espnow_get_version_c());
    return 1;
}

static int l_get_channel(lua_State *L) {
    lua_pushinteger(L, espnow_get_channel_c());
    return 1;
}

static int l_set_channel(lua_State *L) {
    uint8_t ch = LUA_CHECK_UINT8(L, 1);
    lua_pushboolean(L, espnow_set_channel_c(ch));
    return 1;
}

static int l_set_pmk(lua_State *L) {
    size_t len;
    const char *pmk = luaL_checklstring(L, 1, &len);
    if (len != 16) {
        return luaL_error(L, "PMK must be exactly 16 bytes");
    }
    lua_pushboolean(L, espnow_set_pmk_c((const uint8_t *)pmk));
    return 1;
}

// ============================================================================
// Lua function table
// ============================================================================

static const luaL_Reg espnow_functions[] = {
    // Core
    {"init",            l_init},
    {"deinit",          l_deinit},
    {"is_initialized",  l_is_initialized},

    // Peer management
    {"add_peer",        l_add_peer},
    {"del_peer",        l_del_peer},
    {"mod_peer",        l_mod_peer},
    {"peer_exists",     l_peer_exists},
    {"peer_count",      l_peer_count},
    {"get_peers",       l_get_peers},

    // Send
    {"send",            l_send},
    {"broadcast",       l_broadcast},

    // Receive
    {"receive",         l_receive},

    // Callbacks
    {"on_receive",      l_on_receive},
    {"on_send",         l_on_send},
    {"poll",            l_poll},

    // Info / Config
    {"get_mac",         l_get_mac},
    {"get_version",     l_get_version},
    {"get_channel",     l_get_channel},
    {"set_channel",     l_set_channel},
    {"set_pmk",         l_set_pmk},

    {NULL, NULL}
};

// ============================================================================
// Module registration
// ============================================================================

int luaopen_espnow(lua_State *L, const char *module_name) {
    // Store Lua state for callback dispatch
    s_lua_state = L;

    luaL_newlib(L, espnow_functions);

    // Add constants
    lua_pushinteger(L, ESPNOW_MAX_DATA_LEN);
    lua_setfield(L, -2, "MAX_DATA_LEN");

    lua_setglobal(L, module_name);
    return 0;
}
