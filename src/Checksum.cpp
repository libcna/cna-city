// SPDX-License-Identifier: MIT
#include "Checksum.hpp"

#include <cmath>

#include "Simulation.hpp"

namespace CnaCity
{
    namespace
    {
        /// FNV-1a, 64-bit. Not cryptographic and does not need to be: this is a comparison between
        /// two runs of the same program, not a signature anybody has an interest in forging.
        constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
        constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

        struct Hasher
        {
            std::uint64_t value = kFnvOffset;

            void Mix(std::uint64_t word)
            {
                value ^= word;
                value *= kFnvPrime;
            }

            /// Quantised to a centimetre, and to a *signed integer* rather than to a float: the
            /// point is to compare two runs, and two runs that agree to a centimetre agree.
            void MixFloat(float f)
            {
                if (!std::isfinite(f))
                {
                    Mix(0xDEADBEEFDEADBEEFULL);
                    return;
                }
                Mix(static_cast<std::uint64_t>(std::llround(static_cast<double>(f) * 100.0)));
            }

            void MixVec(Vec2 v)
            {
                MixFloat(v.X);
                MixFloat(v.Y);
            }
        };
    }

    std::string ToHex(std::uint64_t value)
    {
        static const char* const kDigits = "0123456789abcdef";
        std::string text(16, '0');
        for (int i = 15; i >= 0; --i)
        {
            text[static_cast<std::size_t>(i)] = kDigits[value & 0xFu];
            value >>= 4;
        }
        return text;
    }

    bool FromHex(const std::string& text, std::uint64_t& value)
    {
        if (text.size() != 16) return false;
        std::uint64_t parsed = 0;
        for (const char c : text)
        {
            parsed <<= 4;
            if (c >= '0' && c <= '9') parsed |= static_cast<std::uint64_t>(c - '0');
            else if (c >= 'a' && c <= 'f') parsed |= static_cast<std::uint64_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') parsed |= static_cast<std::uint64_t>(c - 'A' + 10);
            else return false;
        }
        value = parsed;
        return true;
    }

    std::uint64_t ComputeCityChecksum(const Simulation& sim)
    {
        Hasher h;
        const City& city = sim.city();

        // The road graph, which everything else is built on. Node positions and the segment table,
        // in index order -- the order is part of the answer, because a generator that produced the
        // same set of roads in a different order would give every downstream index a different
        // meaning.
        h.Mix(city.roads().nodes().size());
        for (const RoadNode& node : city.roads().nodes())
        {
            h.MixVec(node.position);
            h.Mix(node.incidentCount);
            h.Mix(static_cast<std::uint64_t>(node.highestClass));
            h.Mix(node.signalised ? 1u : 0u);
        }
        h.Mix(city.roads().segments().size());
        for (const RoadSegment& segment : city.roads().segments())
        {
            h.Mix(segment.nodeA);
            h.Mix(segment.nodeB);
            h.Mix(static_cast<std::uint64_t>(segment.roadClass));
            h.MixFloat(segment.length);
        }

        h.Mix(city.roads().blocks().size());
        h.Mix(city.districts().size());
        for (const District& district : city.districts())
        {
            h.MixVec(district.center);
            h.Mix(static_cast<std::uint64_t>(district.zone));
        }

        h.Mix(city.buildings().size());
        for (const Building& building : city.buildings())
        {
            h.MixVec(building.center);
            h.MixVec(building.halfExtent);
            h.MixFloat(building.height);
            h.MixFloat(building.rotation);
            h.Mix(static_cast<std::uint64_t>(building.kind));
            h.Mix(building.residents);
            h.Mix(building.jobs);
            h.MixVec(building.doorway);
        }

        h.Mix(city.props().size());
        h.Mix(city.parkedVehicles().size());

        // Both transit layouts, which are generated from the same seed and are as much part of the
        // static city as the streets are.
        h.Mix(sim.metro().stations().size());
        for (const MetroStation& station : sim.metro().stations())
        {
            h.MixVec(station.position);
            h.MixVec(station.axis);
            h.Mix(station.doorNode);
        }
        h.Mix(sim.metro().lines().size());
        for (const MetroLine& line : sim.metro().lines()) h.MixFloat(line.length);

        h.Mix(sim.buses().stops().size());
        for (const BusStop& stop : sim.buses().stops())
        {
            h.MixVec(stop.position);
            h.Mix(stop.node);
        }
        h.Mix(sim.buses().routes().size());
        for (const BusRoute& route : sim.buses().routes())
        {
            h.MixFloat(route.length);
            h.Mix(route.stops.size());
        }
        return h.value;
    }

    WorldChecksum ComputeChecksum(const Simulation& sim)
    {
        WorldChecksum result;
        result.city = ComputeCityChecksum(sim);

        {
            Hasher h;
            const Agents& agents = sim.agents();
            h.Mix(agents.size());
            for (std::size_t i = 0; i < agents.size(); ++i)
            {
                h.MixVec(agents.position[i]);
                h.Mix(agents.mode[i]);
                h.Mix(agents.activity[i]);
            }
            result.agents = h.value;
        }

        {
            Hasher h;
            for (const Vehicle& vehicle : sim.traffic().vehicles())
            {
                h.Mix(vehicle.active);
                if (!vehicle.active) continue;
                h.Mix(vehicle.driver);
                h.Mix(vehicle.segment);
                h.Mix(vehicle.lane);
                h.Mix(vehicle.forward);
                h.MixFloat(vehicle.s);
                h.MixFloat(vehicle.speed);
            }
            result.traffic = h.value;
        }

        {
            Hasher h;
            for (const MetroTrain& train : sim.metro().trains())
            {
                h.Mix(train.line);
                h.MixFloat(train.position);
                h.MixFloat(train.speed);
                h.Mix(train.onboard);
            }
            for (const Bus& bus : sim.buses().buses())
            {
                h.Mix(bus.route);
                h.MixFloat(bus.position);
                h.MixFloat(bus.speed);
                h.Mix(bus.onboard);
            }
            result.transit = h.value;
        }

        {
            Hasher h;
            h.MixFloat(sim.clock().hour());
            h.Mix(static_cast<std::uint64_t>(sim.clock().day()));
            h.Mix(static_cast<std::uint64_t>(sim.weather().kind()));
            h.MixFloat(sim.weather().cloudiness());
            h.MixFloat(sim.weather().precipitation());
            h.MixFloat(sim.weather().wetness());
            h.MixFloat(sim.weather().snowCover());
            h.MixFloat(sim.weather().fogDensity());
            result.world = h.value;
        }

        Hasher total;
        total.Mix(result.city);
        total.Mix(result.agents);
        total.Mix(result.traffic);
        total.Mix(result.transit);
        total.Mix(result.world);
        result.total = total.value;
        return result;
    }
}
