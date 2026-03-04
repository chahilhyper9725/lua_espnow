---@meta lua_espnow
--- ESP-NOW Lua Type Definitions
--- Standalone ESP-NOW wireless communication for ESP32-S3
--- Supports peer-to-peer and broadcast messaging with BLE coexistence

-- =============================================================================
-- ESP-NOW Module
-- =============================================================================

---@class espnow
local espnow = {}

-- =============================================================================
-- Constants
-- =============================================================================

---Maximum ESP-NOW v2 payload size in bytes
---@type integer
espnow.MAX_DATA_LEN = 1470

-- =============================================================================
-- Radio Info Table (returned by on_receive callback and receive())
-- =============================================================================

---Radio metadata from the received packet
---@class EspnowRadioInfo
---@field rssi integer Received Signal Strength Indicator in dBm (e.g. -40)
---@field noise_floor integer RF noise floor in dBm (ESP32-S3 only)
---@field channel integer WiFi channel the packet was received on (1-14)
---@field timestamp integer Local RX time in microseconds

-- =============================================================================
-- Peer Info Table (returned by get_peers())
-- =============================================================================

---Peer information entry
---@class EspnowPeerInfo
---@field mac string MAC address as "AA:BB:CC:DD:EE:FF"
---@field channel integer WiFi channel (0 = current)
---@field encrypt boolean True if encryption is enabled

-- =============================================================================
-- Core Functions
-- =============================================================================

---Initialize ESP-NOW (AP+STA mode, modem sleep, BLE-coexistence safe)
---Called automatically from C setup, but can be called from Lua to reinit
---@param channel? integer WiFi channel 1-14 (default: 1)
---@return boolean success True if initialization succeeded
function espnow.init(channel) end

---Deinitialize ESP-NOW and free all resources
---Drains queues, unregisters callbacks, releases Lua refs
function espnow.deinit() end

---Check if ESP-NOW is initialized
---@return boolean initialized True if ESP-NOW is active
function espnow.is_initialized() end

-- =============================================================================
-- Peer Management
-- =============================================================================

---Add an ESP-NOW peer
---@param mac string MAC address "AA:BB:CC:DD:EE:FF" (case-insensitive)
---@param channel? integer WiFi channel 0-14 (default: 0 = current)
---@param encrypt? boolean Enable encryption (default: false)
---@param lmk? string Local Master Key, exactly 16 bytes (default: nil)
---@return boolean success True if peer was added
function espnow.add_peer(mac, channel, encrypt, lmk) end

---Remove an ESP-NOW peer
---@param mac string MAC address "AA:BB:CC:DD:EE:FF"
---@return boolean success True if peer was removed
function espnow.del_peer(mac) end

---Modify an existing ESP-NOW peer
---@param mac string MAC address "AA:BB:CC:DD:EE:FF"
---@param channel? integer WiFi channel 0-14 (default: 0 = current)
---@param encrypt? boolean Enable encryption (default: false)
---@param lmk? string Local Master Key, exactly 16 bytes (default: nil)
---@return boolean success True if peer was modified
function espnow.mod_peer(mac, channel, encrypt, lmk) end

---Check if a peer is registered
---@param mac string MAC address "AA:BB:CC:DD:EE:FF"
---@return boolean exists True if peer exists in peer list
function espnow.peer_exists(mac) end

---Get the number of registered peers
---@return integer total Total number of peers
---@return integer encrypted Number of encrypted peers
function espnow.peer_count() end

---Get list of all registered unicast peers (excludes broadcast)
---@return EspnowPeerInfo[] peers Array of peer info tables
function espnow.get_peers() end

-- =============================================================================
-- Send Functions
-- =============================================================================

---Send data to a specific peer or broadcast
---@param mac string|nil MAC address "AA:BB:CC:DD:EE:FF", or nil for broadcast
---@param data string Data to send (max 1470 bytes for ESP-NOW v2)
---@return boolean success True if send was queued successfully
function espnow.send(mac, data) end

---Broadcast data to all peers
---Automatically adds broadcast peer (FF:FF:FF:FF:FF:FF) if not present
---@param data string Data to broadcast (max 1470 bytes)
---@return boolean success True if broadcast was queued
function espnow.broadcast(data) end

-- =============================================================================
-- Receive Functions
-- =============================================================================

---Non-blocking dequeue of one received packet
---@return string|nil mac Sender MAC address, or nil if queue empty
---@return string|nil data Received data, or nil if queue empty
---@return EspnowRadioInfo|nil info Radio metadata table, or nil if queue empty
function espnow.receive() end

-- =============================================================================
-- Callback Registration
-- =============================================================================

---Register a Lua receive callback (fired by poll())
---@param callback fun(mac: string, data: string, info: EspnowRadioInfo) Receive handler
function espnow.on_receive(callback) end

---Register a Lua send-status callback (fired by poll())
---@param callback fun(mac: string, success: boolean) Send status handler
function espnow.on_send(callback) end

---Process receive/send queues and fire registered Lua callbacks
---Call this regularly in your main loop
function espnow.poll() end

-- =============================================================================
-- Info / Config Functions
-- =============================================================================

---Get own WiFi STA MAC address
---@return string mac MAC address as "AA:BB:CC:DD:EE:FF"
function espnow.get_mac() end

---Get ESP-NOW protocol version
---@return integer version Protocol version number (1 or 2)
function espnow.get_version() end

---Get current WiFi channel
---@return integer channel Channel number 1-14
function espnow.get_channel() end

---Set WiFi channel (must match all peers)
---@param channel integer Channel number 1-14
---@return boolean success True if channel was set
function espnow.set_channel(channel) end

---Set primary master key for encryption
---@param key string Exactly 16 bytes
---@return boolean success True if PMK was set
function espnow.set_pmk(key) end
