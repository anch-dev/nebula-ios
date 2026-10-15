// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// COM object helpers for the D3D12/DXGI layer on Apple/Linux hosts.

#include <d3d12.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace galaxy::d3d12 {

template <typename... Interfaces>
inline bool iid_in(REFIID riid) {
    return ((riid == __uuidof(Interfaces)) || ...);
}

// IUnknown for an implementation of `Interface`, answering QueryInterface
// for the interface and the listed ancestors (IUnknown always).
template <typename Interface, typename... Ancestors>
class Unknown : public Interface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (iid_in<Interface, Ancestors..., IUnknown>(riid)) {
            AddRef();
            *out = static_cast<Interface*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs_.fetch_add(1u, std::memory_order_relaxed) + 1u;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1u, std::memory_order_acq_rel) - 1u;
        if (remaining == 0u) delete this;
        return remaining;
    }
    virtual ~Unknown() = default;

protected:
    std::atomic<ULONG> refs_{1u};
};

// ID3D12Object (private data and names) on top of Unknown.
template <typename Interface, typename... Ancestors>
class Object : public Unknown<Interface, Ancestors...> {
public:
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        if (size == nullptr) return E_INVALIDARG;
        const std::lock_guard<std::mutex> lock(private_mutex_);
        for (const auto& entry : private_data_) {
            if (entry.guid != guid) continue;
            if (data == nullptr) {
                *size = static_cast<UINT>(entry.bytes.size());
                return S_OK;
            }
            if (*size < entry.bytes.size()) return DXGI_ERROR_MORE_DATA;
            *size = static_cast<UINT>(entry.bytes.size());
            std::memcpy(data, entry.bytes.data(), entry.bytes.size());
            return S_OK;
        }
        *size = 0u;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT size, const void* data) override {
        const std::lock_guard<std::mutex> lock(private_mutex_);
        for (auto it = private_data_.begin(); it != private_data_.end(); ++it) {
            if (it->guid == guid) {
                private_data_.erase(it);
                break;
            }
        }
        if (data != nullptr && size != 0u) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            private_data_.push_back({guid, std::vector<std::uint8_t>(bytes, bytes + size)});
        }
        return S_OK;
    }
    // Holds a reference to `object` until replaced or this object dies.
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* object) override {
        IUnknown* const held = const_cast<IUnknown*>(object);
        if (held != nullptr) held->AddRef();
        IUnknown* previous = nullptr;
        {
            const std::lock_guard<std::mutex> lock(private_mutex_);
            for (auto it = interfaces_.begin(); it != interfaces_.end(); ++it) {
                if (it->first == guid) {
                    previous = it->second;
                    interfaces_.erase(it);
                    break;
                }
            }
            if (held != nullptr) interfaces_.push_back({guid, held});
        }
        if (previous != nullptr) previous->Release();
        return S_OK;
    }
    ~Object() override {
        for (auto& [guid, held] : interfaces_) held->Release();
    }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR name) override {
        const std::lock_guard<std::mutex> lock(private_mutex_);
        name_ = name != nullptr ? std::wstring(name) : std::wstring();
        return S_OK;
    }
    [[nodiscard]] std::wstring name() const {
        const std::lock_guard<std::mutex> lock(private_mutex_);
        return name_;
    }

private:
    struct Entry {
        GUID guid;
        std::vector<std::uint8_t> bytes;
    };
    mutable std::mutex private_mutex_;
    std::vector<Entry> private_data_;
    std::vector<std::pair<GUID, IUnknown*>> interfaces_;
    std::wstring name_;
};

class Device;

// ID3D12DeviceChild::GetDevice on top of Object; holds the device alive.
template <typename Interface, typename... Ancestors>
class DeviceChild : public Object<Interface, Ancestors...> {
public:
    explicit DeviceChild(Device* device);
    ~DeviceChild() override;
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** device) override;
    [[nodiscard]] Device* device() const noexcept { return device_; }

private:
    Device* device_;
};

// Stores `object` (one reference, already held) into a QueryInterface-style
// out parameter, releasing it on mismatch.
template <typename T>
HRESULT hand_out(T* object, REFIID riid, void** out) {
    if (out == nullptr) {
        object->Release();
        return S_FALSE;  // D3D12: a null out pointer only validates
    }
    const HRESULT hr = object->QueryInterface(riid, out);
    object->Release();
    return hr;
}

}  // namespace galaxy::d3d12
