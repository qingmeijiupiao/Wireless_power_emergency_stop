#include "espnow_pairing_internal.h"

#include <cstring>

#include "HXC_NVS.h"

namespace EspNowLink::Internal {
namespace {

PeerStore default_store() {
    PeerStore store = {};
    store.magic     = STORE_MAGIC;
    store.version   = STORE_VERSION;
    store.checksum  = calculate_checksum(store);
    return store;
}

HXC::NVS_DATA<PeerStore> stored_peers("now_peers", default_store());

bool valid_store(const PeerStore& store) {
    return store.magic == STORE_MAGIC && store.version == STORE_VERSION && store.count <= MAX_SAVED_PEERS &&
           store.checksum == calculate_checksum(store);
}

} // namespace

uint32_t calculate_checksum(const PeerStore& store) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&store);
    const size_t   size = offsetof(PeerStore, checksum);
    uint32_t       hash = 2166136261U;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

PeerStore load_store() {
    PeerStore store = stored_peers.read();
    // 长度匹配不代表内容有效，magic、版本、计数和校验任一失败都恢复空表。
    if (!valid_store(store)) {
        store               = default_store();
        const esp_err_t err = stored_peers.set(store);
        if (err != ESP_OK) {
            return default_store();
        }
    }
    return store;
}

esp_err_t save_peer(const EspNowLink::PeerConfig& peer, uint8_t channel) {
    PeerStore   store = load_store();
    StoredPeer* slot  = nullptr;
    // 优先更新相同 MAC；不存在时复用第一个空槽，保持固定表可增删。
    for (auto& candidate : store.peers) {
        if (candidate.used && memcmp(candidate.mac, peer.address.bytes, sizeof(candidate.mac)) == 0) {
            slot = &candidate;
            break;
        }
        if (!candidate.used && slot == nullptr) {
            slot = &candidate;
        }
    }
    if (slot == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    if (!slot->used) {
        store.count++;
    }
    *slot      = {};
    slot->used = 1;
    memcpy(slot->mac, peer.address.bytes, sizeof(slot->mac));
    memcpy(slot->lmk, peer.lmk, sizeof(slot->lmk));
    slot->last_channel = channel;
    store.checksum     = calculate_checksum(store);
    // HXC_NVS 以完整 blob 原子更新缓存和 Flash，不为每个 peer 动态创建 key。
    return stored_peers.set(store);
}

esp_err_t update_peer_channel(const MacAddress& address, uint8_t channel) {
    PeerStore store = load_store();
    for (auto& peer : store.peers) {
        if (peer.used && memcmp(peer.mac, address.bytes, sizeof(peer.mac)) == 0) {
            peer.last_channel = channel;
            store.checksum    = calculate_checksum(store);
            esp_err_t err     = stored_peers.set(store);
            if (err != ESP_OK) {
                return err;
            }
            PeerConfig config = {};
            memcpy(config.address.bytes, peer.mac, sizeof(peer.mac));
            memcpy(config.lmk, peer.lmk, sizeof(peer.lmk));
            config.channel   = channel;
            config.encrypted = true;
            return add_peer(config);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t erase_peer(const EspNowLink::MacAddress& address) {
    PeerStore store = load_store();
    for (auto& peer : store.peers) {
        if (peer.used && memcmp(peer.mac, address.bytes, sizeof(peer.mac)) == 0) {
            peer = {};
            if (store.count > 0) {
                store.count--;
            }
            store.checksum = calculate_checksum(store);
            return stored_peers.set(store);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t erase_all_peers() {
    PeerStore store = default_store();
    return stored_peers.set(store);
}

size_t saved_peer_count() {
    return load_store().count;
}

esp_err_t read_saved_peer(size_t index, SavedPeer* output) {
    if (output == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    PeerStore store   = load_store();
    size_t    current = 0;
    for (const auto& peer : store.peers) {
        if (!peer.used) {
            continue;
        }
        if (current++ == index) {
            memcpy(output->address.bytes, peer.mac, sizeof(peer.mac));
            output->last_channel = peer.last_channel;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void restore_peers() {
    PeerStore store = load_store();
    for (const auto& stored : store.peers) {
        if (!stored.used) {
            continue;
        }
        EspNowLink::PeerConfig peer = {};
        memcpy(peer.address.bytes, stored.mac, sizeof(stored.mac));
        memcpy(peer.lmk, stored.lmk, sizeof(stored.lmk));
        peer.channel   = stored.last_channel;
        peer.encrypted = true;
        EspNowLink::add_peer(peer);
    }
}

} // namespace EspNowLink::Internal
