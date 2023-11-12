/*

Copyright (C) 2023 BIAGINI Nathan

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

*/

/*
    --- NBNET C (NATIVE) WEBRTC DRIVER ---

    WebRTC driver for the nbnet library, using a single unreliable data channel. As opposed to the other emscripten/JS
    based WebRTC driver (webrtc.h), this one is fully written in C99 and can be compiled as a native application.

    Dependencies:

        1. libdatachannel (https://github.com/paullouisageneau/libdatachannel)
        2. json.h (https://github.com/sheredom/json.h)
    
    How to use:

        1. Include this header *once* after the nbnet header in the same file where you defined the NBNET_IMPL macro
        2. Call NBN_WebRTC_C_Register in both your client and server code before calling NBN_GameClient_Start or NBN_GameServer_Start
*/

#include <string.h>
#include <rtc/rtc.h>
#include "json.h"

#define NBN_WEBRTC_C_DRIVER_ID 2
#define NBN_WEBRTC_C_DRIVER_NAME "WebRTC_C"

void NBN_WebRTC_C_Register(void);

#ifdef NBNET_IMPL

typedef struct
{
    int id;
    int channel_id;
    int ws;
    NBN_Connection *conn;
} NBN_WebRTC_C_Peer;

static void NBN_WebRTC_C_DestroyPeer(NBN_WebRTC_C_Peer *peer);

#pragma region Hashtable

#define HTABLE_DEFAULT_INITIAL_CAPACITY 32
#define HTABLE_LOAD_FACTOR_THRESHOLD 0.75

typedef struct
{
    int peer_id;
    NBN_WebRTC_C_Peer *peer;
    unsigned int slot;
} NBN_WebRTC_C_HTableEntry;

typedef struct
{
    NBN_WebRTC_C_HTableEntry **internal_array;
    unsigned int capacity;
    unsigned int count;
    float load_factor;
} NBN_WebRTC_C_HTable;

static NBN_WebRTC_C_HTable *NBN_WebRTC_C_HTable_Create(void);
static NBN_WebRTC_C_HTable *NBN_WebRTC_C_HTable_CreateWithCapacity(unsigned int);
static void NBN_WebRTC_C_HTable_Destroy(NBN_WebRTC_C_HTable *);
static void NBN_WebRTC_C_HTable_Add(NBN_WebRTC_C_HTable *, int, NBN_WebRTC_C_Peer *);
static NBN_WebRTC_C_Peer *NBN_WebRTC_C_HTable_Get(NBN_WebRTC_C_HTable *, int);
static NBN_WebRTC_C_Peer *NBN_WebRTC_C_HTable_Remove(NBN_WebRTC_C_HTable *, int);
static void NBN_WebRTC_C_HTable_InsertEntry(NBN_WebRTC_C_HTable *, NBN_WebRTC_C_HTableEntry *);
static void NBN_WebRTC_C_HTable_RemoveEntry(NBN_WebRTC_C_HTable *, NBN_WebRTC_C_HTableEntry *);
static unsigned int NBN_WebRTC_C_HTable_FindFreeSlot(NBN_WebRTC_C_HTable *, NBN_WebRTC_C_HTableEntry *, bool *);
static NBN_WebRTC_C_HTableEntry *NBN_WebRTC_C_HTable_FindEntry(NBN_WebRTC_C_HTable *, int);
static void NBN_WebRTC_C_HTable_Grow(NBN_WebRTC_C_HTable *);

static NBN_WebRTC_C_HTable *NBN_WebRTC_C_HTable_Create(void)
{
    return NBN_WebRTC_C_HTable_CreateWithCapacity(HTABLE_DEFAULT_INITIAL_CAPACITY);
}

static NBN_WebRTC_C_HTable *NBN_WebRTC_C_HTable_CreateWithCapacity(unsigned int capacity)
{
    NBN_WebRTC_C_HTable *htable = NBN_Allocator(sizeof(NBN_WebRTC_C_HTable));

    htable->internal_array = NBN_Allocator(sizeof(NBN_WebRTC_C_HTableEntry *) * capacity);
    htable->capacity = capacity;
    htable->count = 0;
    htable->load_factor = 0;

    for (unsigned int i = 0; i < htable->capacity; i++)
    {
        htable->internal_array[i] = NULL;
    }

    return htable;
}

static void NBN_WebRTC_C_HTable_Destroy(NBN_WebRTC_C_HTable *htable)
{
    for (unsigned int i = 0; i < htable->capacity; i++)
    {
        NBN_WebRTC_C_HTableEntry *entry = htable->internal_array[i];

        if (entry)
        {
            NBN_WebRTC_C_DestroyPeer(entry->peer);
        }
    }

    NBN_Deallocator(htable->internal_array);
    NBN_Deallocator(htable);
}

static void NBN_WebRTC_C_HTable_Add(NBN_WebRTC_C_HTable *htable, int peer_id, NBN_WebRTC_C_Peer *peer)
{
    NBN_WebRTC_C_HTableEntry *entry = NBN_Allocator(sizeof(NBN_WebRTC_C_HTableEntry));

    entry->peer_id = peer_id;
    entry->peer = peer;

    NBN_WebRTC_C_HTable_InsertEntry(htable, entry);

    if (htable->load_factor >= HTABLE_LOAD_FACTOR_THRESHOLD)
        NBN_WebRTC_C_HTable_Grow(htable);
}

static NBN_WebRTC_C_Peer *NBN_WebRTC_C_HTable_Get(NBN_WebRTC_C_HTable *htable, int peer_id)
{
    NBN_WebRTC_C_HTableEntry *entry = NBN_WebRTC_C_HTable_FindEntry(htable, peer_id);

    return entry ? entry->peer : NULL;
}

static NBN_WebRTC_C_Peer *NBN_WebRTC_C_HTable_Remove(NBN_WebRTC_C_HTable *htable, int peer_id)
{
    NBN_WebRTC_C_HTableEntry *entry = NBN_WebRTC_C_HTable_FindEntry(htable, peer_id);

    if (entry)
    {
        NBN_WebRTC_C_Peer *peer = entry->peer;

        NBN_WebRTC_C_HTable_RemoveEntry(htable, entry);

        return peer;
    }

    return NULL;
}

static void NBN_WebRTC_C_HTable_InsertEntry(NBN_WebRTC_C_HTable *htable, NBN_WebRTC_C_HTableEntry *entry)
{
    bool use_existing_slot = false;
    unsigned int slot = NBN_WebRTC_C_HTable_FindFreeSlot(htable, entry, &use_existing_slot);

    entry->slot = slot;
    htable->internal_array[slot] = entry;

    if (!use_existing_slot)
    {
        htable->count++;
        htable->load_factor = (float)htable->count / htable->capacity;
    }
}

static void NBN_WebRTC_C_HTable_RemoveEntry(NBN_WebRTC_C_HTable *htable, NBN_WebRTC_C_HTableEntry *entry)
{
    htable->internal_array[entry->slot] = NULL;

    NBN_Deallocator(entry);

    htable->count--;
    htable->load_factor = htable->count / htable->capacity;
}

static unsigned int NBN_WebRTC_C_HTable_FindFreeSlot(NBN_WebRTC_C_HTable *htable, NBN_WebRTC_C_HTableEntry *entry, bool *use_existing_slot)
{
    unsigned long hash = entry->peer_id;
    unsigned int slot;

    // quadratic probing

    NBN_WebRTC_C_HTableEntry *current_entry;
    unsigned int i = 0;

    do
    {
        slot = (hash + (int)pow(i, 2)) % htable->capacity;
        current_entry = htable->internal_array[slot];

        i++;
    } while (current_entry != NULL && current_entry->peer_id != entry->peer_id);

    if (current_entry != NULL) // it means the current entry as the same key as the inserted entry
    {
        *use_existing_slot = true;

        NBN_Deallocator(current_entry);
    }
    
    return slot;
}

static NBN_WebRTC_C_HTableEntry *NBN_WebRTC_C_HTable_FindEntry(NBN_WebRTC_C_HTable *htable, int peer_id)
{
    unsigned long hash = peer_id;
    unsigned int slot;

    //quadratic probing

    NBN_WebRTC_C_HTableEntry *current_entry;
    unsigned int i = 0;

    do
    {
        slot = (hash + (int)pow(i, 2)) % htable->capacity;
        current_entry = htable->internal_array[slot];

        if (current_entry != NULL && current_entry->peer_id == peer_id)
        {
            return current_entry;
        }

        i++;
    } while (i < htable->capacity);
    
    return NULL;
}

static void NBN_WebRTC_C_HTable_Grow(NBN_WebRTC_C_HTable *htable)
{
    unsigned int old_capacity = htable->capacity;
    unsigned int new_capacity = old_capacity * 2;
    NBN_WebRTC_C_HTableEntry** old_internal_array = htable->internal_array;
    NBN_WebRTC_C_HTableEntry** new_internal_array = NBN_Allocator(sizeof(NBN_WebRTC_C_HTableEntry*) * new_capacity);

    for (unsigned int i = 0; i < new_capacity; i++)
    {
        new_internal_array[i] = NULL;
    }

    htable->internal_array = new_internal_array;
    htable->capacity = new_capacity;
    htable->count = 0;
    htable->load_factor = 0;

    // rehash

    for (unsigned int i = 0; i < old_capacity; i++)
    {
        if (old_internal_array[i])
            NBN_WebRTC_C_HTable_InsertEntry(htable, old_internal_array[i]);
    }

    NBN_Deallocator(old_internal_array);
}

#pragma endregion // Hashtable

#pragma region String utils

// IMPORTANT: res needs to be pre allocated and big enough to old the resulting string
static void NBN_WebRTC_C_StringReplaceAll(char *res, const char *str, const char *a, const char *b)
{
    char *substr = strstr(str, a);
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);

    if (substr)
    {
        int pos = substr - str;

        strncpy(res, str, pos);
        strncpy(res + pos, b, len_b);

        NBN_WebRTC_C_StringReplaceAll(res + pos + len_b, str + pos + len_a, a, b);
    }
    else
    {
        strncpy(res, str, strlen(str) + 1);
    }
}

#pragma endregion /* String utils */

#pragma region Signaling

typedef enum
{
    NBN_WEBRTC_C_UNDEFINED,
    NBN_WEBRTC_C_OFFER
} NBN_WebRTC_C_SignalingPayloadType;

typedef struct
{
    NBN_WebRTC_C_SignalingPayloadType type;
    char *sdp;
} NBN_WebRTC_C_SignalingPayload;

#define DEFAULT_ICE_SERVER "stun:stun01.sipphone.com"

#ifndef ICE_SERVERS
#define ICE_SERVERS DEFAULT_ICE_SERVER
#endif

static const char *ice_servers[] = {
   DEFAULT_ICE_SERVER
};

static bool NBN_WebRTC_C_ParseSignalingMessage(const char *msg, size_t msg_len, NBN_WebRTC_C_SignalingPayload *payload)
{
    struct json_value_s* root = json_parse(msg, msg_len); // this has to be freed

    if (root->type != json_type_object)
    {
        free(root);
        return false;
    }

    struct json_object_s* object = (struct json_object_s*)root->payload;
    struct json_object_element_s *curr = object->start;

    payload->type = NBN_WEBRTC_C_UNDEFINED;
    payload->sdp = NULL;

    while (curr != NULL)
    {
        if (strncmp(curr->name->string, "type", 4) == 0)
        {
            payload->type = NBN_WEBRTC_C_OFFER;
        }
        else if (strncmp(curr->name->string, "sdp", 3) == 0)
        {
            struct json_string_s *str = json_value_as_string(curr->value);

            if (str)
            {
                // strdup equivalent using NBN_Allocator, make sure this is freed
                size_t len = strlen(str->string);

                payload->sdp = NBN_Allocator(len + 1);
                memcpy(payload->sdp, str->string, len + 1);
            }
        }

        curr = curr->next;
    }

    free(root);

    if (payload->type == NBN_WEBRTC_C_UNDEFINED)
    {
        return false;
    }

    return true;
}

static char *NBN_WebRTC_C_EscapeSDP(const char *sdp)
{
    size_t len = strlen(sdp) * 2; // TODO: kinda lame way of making sure it's going to be big enough, find a better way
    char *escaped_sdp = NBN_Allocator(len);

    NBN_WebRTC_C_StringReplaceAll(escaped_sdp, sdp, "\r\n", "\\r\\n");

    return escaped_sdp;
}

static void NBN_WebRTC_C_OnLocalDescriptionCallback(int pc, const char *sdp, const char *type, void *user_ptr)
{
    if (strncmp(type, "answer", 5) != 0)
    {
        NBN_LogWarning("Received a local description with an unknown type: %s", type);
        return;
    }

    NBN_WebRTC_C_Peer *peer = user_ptr;
    char signaling_json[1024];
    char *escaped_sdp = NBN_WebRTC_C_EscapeSDP(sdp);

    snprintf(signaling_json, sizeof(signaling_json), "{\"type\":\"answer\", \"sdp\":\"%s\"}", escaped_sdp);
    rtcSendMessage(peer->ws, signaling_json, -1); // assume signaling_json to be a null-terminated string
    NBN_Deallocator(escaped_sdp);
}

#pragma endregion /* Signaling */

#pragma region Game server

typedef struct NBN_WebRTC_C_Server
{
    int wsserver;
    NBN_WebRTC_C_HTable *peers;
    bool is_encrypted;
    uint16_t ws_port;
    uint32_t protocol_id;
    char packet_buffer[NBN_PACKET_MAX_SIZE];
} NBN_WebRTC_C_Server;

static NBN_WebRTC_C_Server nbn_wrtc_c_serv = {0, NULL, false, 0, 0, {0}};

static void NBN_WebRTC_C_DestroyPeer(NBN_WebRTC_C_Peer *peer)
{
    NBN_LogDebug("Destroying peer %d", peer->id);

    rtcDeleteDataChannel(peer->channel_id);
    rtcDeletePeerConnection(peer->id);
    rtcDelete(peer->ws);
    NBN_Deallocator(peer);
}

static void NBN_WebRTC_C_OnPeerStateChanged(int pc, rtcState state, void *user_ptr)
{
    NBN_LogDebug("Peer state changed to %d", state);

    if (state == RTC_CONNECTED)
    {
        NBN_WebRTC_C_Peer *peer = (NBN_WebRTC_C_Peer *)user_ptr;

        NBN_Driver_RaiseEvent(NBN_DRIVER_SERV_CLIENT_CONNECTED, peer->conn);
        NBN_LogDebug("Peer %d is connected !", pc);
    }
}

static void NBN_WebRTC_C_OnWsOpen(int ws, void *user_ptr)
{
    NBN_LogDebug("WS %d is open", ws);

    int peer_id = rtcCreatePeerConnection(&(rtcConfiguration){
            .iceServers = ice_servers,
            .iceServersCount = 1,
            .disableAutoNegotiation = false
            });

    if (peer_id < 0)
    {
        NBN_LogError("Failed to create peer: %d", peer_id);
        rtcClose(ws);
        return;
    }

    int ret = rtcSetLocalDescriptionCallback(peer_id, NBN_WebRTC_C_OnLocalDescriptionCallback);

    if (ret < 0)
    {
        NBN_LogError("Failed to register local description callback for peer %d: %d", peer_id, ret);
        rtcDeletePeerConnection(peer_id);
        rtcClose(ws);
        return;
    }

    ret = rtcSetStateChangeCallback(peer_id, NBN_WebRTC_C_OnPeerStateChanged);

    if (ret < 0)
    {
        NBN_LogError("Failed to register state change callback for peer %d: %d", peer_id, ret);
        rtcDeletePeerConnection(peer_id);
        rtcClose(ws);
        return;
    }

    int channel_id = rtcCreateDataChannelEx(peer_id, "unreliable", &(rtcDataChannelInit){
            .reliability = {.unordered = true, .unreliable = true, .maxPacketLifeTime = 1000, .maxRetransmits = 0},
            .negotiated = true,
            .manualStream = true,
            .stream = 0
            });

    if (channel_id < 0)
    {
        NBN_LogError("Failed to create data channel for peer %d: %d", peer_id, channel_id);
        rtcDeletePeerConnection(peer_id);
        rtcClose(ws);
        return;
    }

    NBN_LogDebug("Successfully created data channel for peer %d: %d", peer_id, channel_id);

    NBN_WebRTC_C_Peer *peer = (NBN_WebRTC_C_Peer *)NBN_Allocator(sizeof(NBN_WebRTC_C_Peer));

    peer->id = peer_id;
    peer->channel_id = channel_id;
    peer->ws = ws;
    peer->conn = NBN_GameServer_CreateClientConnection(
            NBN_WEBRTC_C_DRIVER_ID,
            peer,
            nbn_wrtc_c_serv.protocol_id,
            peer_id,
            nbn_wrtc_c_serv.is_encrypted);

    rtcSetUserPointer(ws, peer);
    rtcSetUserPointer(peer_id, peer);
    NBN_WebRTC_C_HTable_Add(nbn_wrtc_c_serv.peers, peer_id, peer);
}

static void NBN_WebRTC_C_OnWsClosed(int ws, void *user_ptr)
{
    NBN_LogDebug("WS %d has closed", ws);

    if (user_ptr)
    {
        NBN_WebRTC_C_Peer *peer = (NBN_WebRTC_C_Peer *)user_ptr;

        NBN_LogDebug("Closing WebRTC peer and channel (peer: %d, channel: %d)", peer->id, peer->channel_id);

        rtcClose(peer->channel_id);
        rtcClosePeerConnection(peer->id);
    }
}

static void NBN_WebRTC_C_OnWsError(int ws, const char *err_msg, void *user_ptr)
{
    (void)user_ptr;

    NBN_LogError("Error on WS %d: %s", ws, err_msg);
}

static void NBN_WebRTC_C_OnWsMessage(int ws, const char *msg, int size, void *user_ptr)
{
    NBN_LogDebug("Received signaling data on WS %d (size: %d): %s", ws, size, msg);

    // FIXME: the size parameter is wrong for some reason
    // that's why I need to use strlen on the msg (I'm assuming it's always null-terminated...)
    size = strlen(msg);

    NBN_WebRTC_C_SignalingPayload payload;
    bool ret = NBN_WebRTC_C_ParseSignalingMessage(msg, size, &payload);

    if (!ret)
    {
        NBN_LogWarning("Failed to parse signaling data for WS %d", ws);
        return;
    }

    NBN_LogDebug("Successfully parsed signaling payload (type: %d, sdp: %s)", payload.type, payload.sdp);

    if (payload.type == NBN_WEBRTC_C_OFFER && payload.sdp)
    {
        NBN_WebRTC_C_Peer *peer = (NBN_WebRTC_C_Peer *)user_ptr;

        int ret = rtcSetRemoteDescription(peer->id, payload.sdp, "offer");

        if (ret < 0)
        {
            NBN_LogError("Failed to set remote description for peer %d (WS: %d): %d", peer->id, ws, ret);
            rtcClose(ws);
        }
    }

    // IMPORTANT: not sure I can free this because it's passed to rtcSetRemoteDescription
    NBN_Deallocator(payload.sdp);
}

static void NBN_WebRTC_C_OnWsConnection(int wsserver, int ws, void *user_ptr)
{
    NBN_LogDebug("New WS connection %d (user_ptr: %p)", ws, user_ptr);

    rtcSetOpenCallback(ws, NBN_WebRTC_C_OnWsOpen);
    rtcSetClosedCallback(ws, NBN_WebRTC_C_OnWsClosed);
    rtcSetErrorCallback(ws, NBN_WebRTC_C_OnWsError);
    rtcSetMessageCallback(ws, NBN_WebRTC_C_OnWsMessage);
}

static int NBN_WebRTC_C_ServStart(uint32_t protocol_id, uint16_t port, bool enable_encryption)
{
    nbn_wrtc_c_serv.ws_port = port;
    nbn_wrtc_c_serv.peers = NBN_WebRTC_C_HTable_Create();
    nbn_wrtc_c_serv.is_encrypted = enable_encryption;
    nbn_wrtc_c_serv.protocol_id = protocol_id;
    nbn_wrtc_c_serv.wsserver = -1;

#ifdef NBN_USE_HTTPS

// TODO
#error "HTTPS is not supported yet"

#endif // NBN_USE_HTTPS

    rtcInitLogger(RTC_LOG_VERBOSE, NULL); // will print on stdout
    rtcPreload();

    rtcWsServerConfiguration cfg = {
        .port = port,

        // TODO: https
        .enableTls = false,
        .certificatePemFile = NULL,
        .keyPemFile = NULL,
        .keyPemPass = NULL
    };

    int wsserver = rtcCreateWebSocketServer(&cfg, NBN_WebRTC_C_OnWsConnection);

    if (wsserver < 0)
    {
        NBN_LogError("Failed to start WS server (code: %d)", wsserver);
        return -1;
    }

    nbn_wrtc_c_serv.wsserver = wsserver;

    return 0;
}

static void NBN_WebRTC_C_ServStop(void)
{
    NBN_WebRTC_C_HTable_Destroy(nbn_wrtc_c_serv.peers);

    if (nbn_wrtc_c_serv.wsserver >= 0)
    {
        rtcDeleteWebSocketServer(nbn_wrtc_c_serv.wsserver);
    }

    rtcCleanup();
}

static int NBN_WebRTC_C_ServRecvPackets(void)
{
    int size;

    for (unsigned int i = 0; i < nbn_wrtc_c_serv.peers->capacity; i++)
    {
        NBN_WebRTC_C_HTableEntry *entry = nbn_wrtc_c_serv.peers->internal_array[i];

        if (entry)
        {
            size = NBN_PACKET_MAX_SIZE;

            while (rtcReceiveMessage(entry->peer->channel_id, nbn_wrtc_c_serv.packet_buffer, &size) != RTC_ERR_NOT_AVAIL)
            {
                NBN_Packet packet;

                if (NBN_Packet_InitRead(&packet, entry->peer->conn, (uint8_t *)nbn_wrtc_c_serv.packet_buffer, size) < 0)
                    continue;

                packet.sender = entry->peer->conn;
                NBN_Driver_RaiseEvent(NBN_DRIVER_SERV_CLIENT_PACKET_RECEIVED, &packet);
            }
        }
    }
    return 0;
}

static void NBN_WebRTC_C_ServRemoveClientConnection(NBN_Connection *conn)
{
    NBN_WebRTC_C_Peer *peer = conn->driver_data;

    NBN_WebRTC_C_HTable_Remove(nbn_wrtc_c_serv.peers, peer->id);
    NBN_WebRTC_C_DestroyPeer(peer);
}

static int NBN_WebRTC_C_ServSendPacketTo(NBN_Packet *packet, NBN_Connection *conn)
{
    if (!conn->is_accepted) return 0;

    NBN_WebRTC_C_Peer *peer = conn->driver_data;

    if (rtcSendMessage(peer->channel_id, (char *)packet->buffer, packet->size) < 0)
    {
        NBN_LogDebug("rtcSendMessage failed for peer %d", peer->id);
    }

    return 0;
}

#pragma endregion /* Game server */

void NBN_WebRTC_C_Register(void)
{
    NBN_DriverImplementation driver_impl = {
        // Client implementation
        NULL,
        NULL,
        NULL,
        NULL,

        // Server implementation
        NBN_WebRTC_C_ServStart,
        NBN_WebRTC_C_ServStop,
        NBN_WebRTC_C_ServRecvPackets,
        NBN_WebRTC_C_ServSendPacketTo,
        NBN_WebRTC_C_ServRemoveClientConnection
    };

    NBN_Driver_Register(
        NBN_WEBRTC_C_DRIVER_ID,
        NBN_WEBRTC_C_DRIVER_NAME,
        driver_impl
    );
}

#endif // NBNET_IMPL
