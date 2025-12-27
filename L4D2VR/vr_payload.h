#pragma once

#include <array>
#include <atomic>
#include <chrono>

#include "sdk/vector.h"

struct VrViewPayload
{
    uint64_t sequence = 0;

    Vector hmdPositionAbs = { 0.0f, 0.0f, 0.0f };
    QAngle hmdAnglesAbs = { 0.0f, 0.0f, 0.0f };

    Vector leftEyePosition = { 0.0f, 0.0f, 0.0f };
    Vector rightEyePosition = { 0.0f, 0.0f, 0.0f };

    VMatrix leftViewMatrix{};
    VMatrix rightViewMatrix{};

    VMatrix leftProjectionMatrix{};
    VMatrix rightProjectionMatrix{};

    std::chrono::steady_clock::time_point capturedAt{};
};

class VrPayloadRingBuffer
{
public:
    static constexpr size_t kCapacity = 32;

    void Push(const VrViewPayload& payload)
    {
        uint64_t nextSequence = m_publishedSequence.load(std::memory_order_relaxed) + 1;

        VrViewPayload copy = payload;
        copy.sequence = nextSequence;

        const size_t slot = (nextSequence - 1) % kCapacity;
        m_buffer[slot] = copy;

        std::atomic_thread_fence(std::memory_order_release);
        m_publishedSequence.store(nextSequence, std::memory_order_release);
    }

    bool TryReadLatest(VrViewPayload& payloadOut, uint64_t& sequenceOut) const
    {
        const uint64_t sequence = m_publishedSequence.load(std::memory_order_acquire);

        if (sequence == 0)
            return false;

        const size_t slot = (sequence - 1) % kCapacity;
        const VrViewPayload& payload = m_buffer[slot];

        if (payload.sequence != sequence)
            return false;

        payloadOut = payload;
        sequenceOut = sequence;
        return true;
    }

    uint64_t LatestSequence() const
    {
        return m_publishedSequence.load(std::memory_order_acquire);
    }

private:
    std::array<VrViewPayload, kCapacity> m_buffer{};
    std::atomic<uint64_t> m_publishedSequence{ 0 };
};

inline VrPayloadRingBuffer g_VrViewPayloadQueue{};
