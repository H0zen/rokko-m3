#pragma once

// What coordinates are measured against: a map instance, or a vessel's deck. A SHIP IS
// ITSELF A MAP -- no arithmetic crosses between frames, and Placement fails closed across.

#include <cstdint>

namespace Geometry
{
    class Frame
    {
        public:
            Frame() : m_id(0), m_kind(Kind::Nowhere) {}

            static Frame World(uint32_t mapId, uint32_t instanceId)
            {
                return Frame(Kind::World, (uint64_t(mapId) << 32) | uint64_t(instanceId));
            }

            static Frame Deck(uint64_t vesselGuid)
            {
                return Frame(Kind::Deck, vesselGuid);
            }

            bool IsPlaced() const { return m_kind != Kind::Nowhere; }
            bool IsDeck() const { return m_kind == Kind::Deck; }
            bool IsWorld() const { return m_kind == Kind::World; }
            uint64_t Id() const { return m_id; }

            /// The map a World frame names, and the instance of it. Zero for any other kind:
            /// a deck's key is a vessel's guid and has no map in it, and Nowhere has nothing.
            uint32_t MapId() const { return IsWorld() ? uint32_t(m_id >> 32) : 0; }
            uint32_t InstanceId() const { return IsWorld() ? uint32_t(m_id) : 0; }

            bool operator==(const Frame& other) const
            {
                return m_kind == other.m_kind && m_id == other.m_id;
            }

            bool operator!=(const Frame& other) const { return !(*this == other); }

        private:
            enum class Kind : uint8_t
            {
                Nowhere,
                World,
                Deck
            };

            Frame(Kind kind, uint64_t id) : m_id(id), m_kind(kind) {}

            uint64_t m_id;
            Kind m_kind;
    };
}
