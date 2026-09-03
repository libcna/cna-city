// SPDX-License-Identifier: MIT
#include "Snapshot.hpp"

#include <cstdio>

#include "Archive.hpp"
#include "Checksum.hpp"

namespace CnaCity
{
    namespace
    {
        constexpr std::uint64_t kMagic = 0x594349434E43ULL;   // "CNCITY"
        constexpr std::uint32_t kVersion = 1;

        /// Written into the header and checked on load. A snapshot is a memory image of a struct
        /// layout, so a build whose `Vehicle` grew a field must refuse a file from one whose did
        /// not -- and the version alone does not catch that, because nobody remembers to bump it.
        struct LayoutFingerprint
        {
            std::uint32_t vehicle = static_cast<std::uint32_t>(sizeof(Vehicle));
            std::uint32_t train = static_cast<std::uint32_t>(sizeof(MetroTrain));
            std::uint32_t bus = static_cast<std::uint32_t>(sizeof(Bus));
            std::uint32_t vec2 = static_cast<std::uint32_t>(sizeof(Vec2));

            [[nodiscard]] bool operator==(const LayoutFingerprint&) const = default;
        };

        void SerializeConfig(Archive& archive, SimConfig& config)
        {
            archive.Pod(config.city.seed);
            archive.Pod(config.city.halfSize);
            archive.Pod(config.city.arterialSpacing);
            archive.Pod(config.city.diagonalAvenues);
            archive.Pod(config.city.blockSetback);
            archive.Pod(config.city.metroLines);
            archive.Pod(config.city.parkFraction);
            archive.Pod(config.city.kerbOccupancy);
            archive.Pod(config.agentCount);
            archive.Pod(config.startHour);
            archive.Pod(config.timeScale);
            archive.Pod(config.weather);
            archive.Pod(config.randomWeather);
            archive.Pod(config.carOwnership);
            archive.Pod(config.metroLines);
            archive.Pod(config.busRoutes);
            archive.Pod(config.threads);
        }

        struct Header
        {
            std::uint64_t magic = kMagic;
            std::uint32_t version = kVersion;
            LayoutFingerprint layout;
            std::uint64_t cityChecksum = 0;
            std::uint64_t tick = 0;
            float hour = 0.0f;
            int day = 0;
            std::uint32_t travelling = 0;
        };

        void SerializeHeader(Archive& archive, Header& header, SimConfig& config, std::string& note)
        {
            archive.Pod(header.magic);
            archive.Pod(header.version);
            archive.Pod(header.layout);
            archive.Pod(header.cityChecksum);
            archive.Pod(header.tick);
            archive.Pod(header.hour);
            archive.Pod(header.day);
            archive.Pod(header.travelling);
            archive.Text(note);
            SerializeConfig(archive, config);
            archive.Fence(0x48454144u);   // "HEAD"
        }

        bool ReadWholeFile(const std::string& path, std::vector<std::uint8_t>& out,
                           std::string& error)
        {
            std::FILE* handle = std::fopen(path.c_str(), "rb");
            if (handle == nullptr)
            {
                error = "cannot read " + path;
                return false;
            }
            std::uint8_t buffer[65536];
            std::size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof(buffer), handle)) > 0)
                out.insert(out.end(), buffer, buffer + got);
            std::fclose(handle);
            return true;
        }

        bool CheckHeader(const Header& header, const std::string& path, std::string& error)
        {
            if (header.magic != kMagic)
            {
                error = path + " is not a cna-city snapshot";
                return false;
            }
            if (header.version != kVersion)
            {
                error = path + " is snapshot version " + std::to_string(header.version) +
                        "; this build reads " + std::to_string(kVersion);
                return false;
            }
            if (!(header.layout == LayoutFingerprint{}))
            {
                error = path +
                        " was written by a build with different structure sizes; a snapshot is a "
                        "memory image and cannot be read across that";
                return false;
            }
            return true;
        }
    }

    bool SaveSnapshot(const std::string& path, const Simulation& sim, const std::string& note,
                      std::string& error)
    {
        std::vector<std::uint8_t> buffer;
        Archive archive(buffer);

        Header header;
        header.cityChecksum = ComputeCityChecksum(sim);
        header.tick = sim.tick();
        header.hour = sim.clock().hour();
        header.day = sim.clock().day();
        header.travelling = static_cast<std::uint32_t>(sim.agents().size()) - sim.stats().indoors;
        SimConfig config = sim.config();
        std::string text = note;
        SerializeHeader(archive, header, config, text);

        // Simulation::Serialize is not const, and neither is the archive traversal -- one function
        // does both directions, which is what stops the reader and the writer drifting apart. The
        // cast is the price of that and it is contained here: nothing in the write direction
        // touches the simulation at all.
        const_cast<Simulation&>(sim).Serialize(archive);
        if (!archive.ok())
        {
            error = "internal error while writing the snapshot";
            return false;
        }

        std::FILE* handle = std::fopen(path.c_str(), "wb");
        if (handle == nullptr)
        {
            error = "cannot write " + path;
            return false;
        }
        const std::size_t written = std::fwrite(buffer.data(), 1, buffer.size(), handle);
        std::fclose(handle);
        if (written != buffer.size())
        {
            error = "short write to " + path;
            return false;
        }
        return true;
    }

    bool ReadSnapshotInfo(const std::string& path, SnapshotInfo& info, std::string& error)
    {
        std::vector<std::uint8_t> buffer;
        if (!ReadWholeFile(path, buffer, error)) return false;

        Archive archive(buffer.data(), buffer.size());
        Header header;
        std::string note;
        SimConfig config;
        SerializeHeader(archive, header, config, note);
        if (!archive.ok())
        {
            error = path + " is truncated";
            return false;
        }
        if (!CheckHeader(header, path, error)) return false;

        info.config = config;
        info.cityChecksum = header.cityChecksum;
        info.tick = header.tick;
        info.hour = header.hour;
        info.day = header.day;
        info.travelling = header.travelling;
        info.note = note;
        return true;
    }

    bool LoadSnapshot(const std::string& path, Simulation& sim, std::string& error)
    {
        std::vector<std::uint8_t> buffer;
        if (!ReadWholeFile(path, buffer, error)) return false;

        Archive archive(buffer.data(), buffer.size());
        Header header;
        std::string note;
        SimConfig config;
        SerializeHeader(archive, header, config, note);
        if (!archive.ok())
        {
            error = path + " is truncated";
            return false;
        }
        if (!CheckHeader(header, path, error)) return false;

        // The city is regenerated rather than stored, and then checked. A generator that has moved
        // since the snapshot was taken produces roads that the saved traffic is no longer on, and
        // the failure would be a slow one -- vehicles on segments that mean something else now --
        // so it is caught here instead.
        sim.Initialize(config);
        if (ComputeCityChecksum(sim) != header.cityChecksum)
        {
            error = path +
                    " was taken from a different city: same seed, different generator. The "
                    "snapshot cannot be loaded into this build.";
            return false;
        }

        sim.Serialize(archive);
        if (!archive.ok())
        {
            error = path + " is corrupt after the header";
            return false;
        }
        return true;
    }
}
