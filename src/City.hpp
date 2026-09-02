// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "CityMath.hpp"
#include "RoadNetwork.hpp"
#include "Rng.hpp"

namespace CnaCity
{
    /**
     * @brief What a district is used for.
     *
     * Zoning is the single input that makes a procedural city read as a city rather than as a
     * grid: it decides street spacing, building height and footprint, how many people live and
     * work per square metre, and therefore where everybody is at nine in the morning.
     */
    enum class ZoneType : std::uint8_t
    {
        Downtown = 0,    ///< Towers, almost no residents, most of the jobs.
        Commercial,      ///< Mid-rise offices and shops with flats above.
        Residential,     ///< Dense perimeter blocks; the bulk of the population.
        Suburb,          ///< Detached houses on their own plots.
        Industrial,      ///< Big sheds, few but real jobs, no residents.
        Park             ///< No buildings; trees, paths and the city's lungs.
    };

    inline constexpr int kZoneCount = 6;

    [[nodiscard]] const char* ZoneName(ZoneType zone);

    /** @brief One cell of the arterial grid, with the character its zoning gives it. */
    struct District
    {
        std::uint16_t id = 0;
        Bounds2 rect;
        Vec2 center{0.0f, 0.0f};
        ZoneType zone = ZoneType::Residential;
        float gridAngle = 0.0f;      ///< The local street grid's rotation; this is what stops the city looking like graph paper.
        float streetSpacing = 95.0f;
        std::string name;
    };

    /** @brief The shape of a building, which is also how the renderer groups it into a draw call. */
    enum class BuildingKind : std::uint8_t
    {
        Tower = 0,       ///< Downtown high-rise, usually with a setback tier and a roof mast.
        Office,          ///< Mid-rise commercial slab.
        Apartment,       ///< Perimeter-block residential.
        House,           ///< Suburban detached, with a pitched roof.
        Shop,            ///< Ground-floor retail unit, often part of a longer terrace.
        Warehouse        ///< Industrial shed.
    };

    inline constexpr int kBuildingKindCount = 6;

    /**
     * @brief One building, as a rotated box with a height and an occupancy.
     *
     * A box is enough. The realism in a skyline comes from the *distribution* of heights and
     * footprints and from the facade material, not from the silhouette of any one building -- and
     * a box is what lets forty thousand of them be four dozen instanced draw calls.
     */
    struct Building
    {
        Vec2 center{0.0f, 0.0f};
        Vec2 halfExtent{6.0f, 6.0f};  ///< Along the building's own axes.
        float rotation = 0.0f;        ///< Yaw, radians, of the local +X axis.
        float height = 12.0f;
        float podiumHeight = 0.0f;    ///< >0 when a tower steps back above a wider base.
        Vec2 podiumHalfExtent{0.0f, 0.0f};
        BuildingKind kind = BuildingKind::Apartment;
        std::uint8_t floors = 4;
        std::uint8_t variant = 0;     ///< Selects the facade material within the kind.
        std::uint16_t district = 0;
        std::uint32_t block = 0;
        std::uint32_t residents = 0;  ///< How many agents call this home.
        std::uint32_t jobs = 0;       ///< How many agents work here.
        /// The point on the pavement this building's door opens onto -- the hand-off between the
        /// building and the street network, and the only part of a building the simulation uses.
        Vec2 doorway{0.0f, 0.0f};
        std::uint32_t doorNode = 0;   ///< The road node nearest the doorway.
    };

    /** @brief Street furniture and planting. Purely visual, but it is most of what sells the scale. */
    enum class PropKind : std::uint8_t
    {
        StreetLamp = 0,
        TrafficSignal,
        TreeRound,       ///< Broadleaf; parks and residential streets.
        TreeConifer,     ///< Parks and the industrial fringe.
        Bench,
        Bin,
        BusShelter,
        MetroEntrance
    };

    inline constexpr int kPropKindCount = 8;

    struct Prop
    {
        Vec2 position{0.0f, 0.0f};
        float rotation = 0.0f;
        float scale = 1.0f;
        PropKind kind = PropKind::StreetLamp;
        std::uint16_t district = 0;
    };

    /** @brief Everything the generator needs to be told; everything else follows from the seed. */
    struct CityConfig
    {
        std::uint64_t seed = 20260902ULL;
        float halfSize = 1650.0f;        ///< Half the side of the square the city occupies, in metres.
        float arterialSpacing = 470.0f;  ///< The district grid; also the arterial road spacing.
        int   diagonalAvenues = 3;
        float blockSetback = 1.2f;       ///< Extra inset past the pavement before anything is built.
        int   metroLines = 4;
        float parkFraction = 0.10f;      ///< Share of districts turned over to parkland.
    };

    /**
     * @brief The static city: streets, districts, blocks, buildings and props.
     *
     * Generated once from the seed and `const` for the rest of the run. Nothing in here changes
     * with the time of day or with what the agents are doing -- that separation is what lets the
     * renderer keep every building in a vertex buffer it never touches again.
     */
    class City
    {
    public:
        void Generate(const CityConfig& config);

        [[nodiscard]] const CityConfig& config() const { return config_; }
        [[nodiscard]] const RoadNetwork& roads() const { return roads_; }
        [[nodiscard]] const std::vector<District>& districts() const { return districts_; }
        [[nodiscard]] const std::vector<Building>& buildings() const { return buildings_; }
        [[nodiscard]] const std::vector<Prop>& props() const { return props_; }
        [[nodiscard]] const std::vector<std::uint32_t>& homes() const { return homes_; }
        [[nodiscard]] const std::vector<std::uint32_t>& workplaces() const { return workplaces_; }
        [[nodiscard]] const std::vector<std::uint32_t>& leisureVenues() const { return leisure_; }

        [[nodiscard]] std::uint32_t totalResidentCapacity() const { return residentCapacity_; }
        [[nodiscard]] std::uint32_t totalJobCapacity() const { return jobCapacity_; }

        /** @brief The district containing @p point, or 0 when it is outside the city. */
        [[nodiscard]] std::uint16_t DistrictAt(Vec2 point) const;

        [[nodiscard]] int districtGridSide() const { return districtSide_; }

        /**
         * @brief The height of the tallest building covering @p point, or 0 where there is none.
         *
         * A coarse occupancy grid, two metres to a cell and one byte to a height. It exists for
         * the cameras: a chase camera placed a fixed distance behind a pedestrian walking along a
         * pavement is inside the building behind them about half the time, and the alternative to
         * this is a camera that spends its life looking at brickwork from within.
         */
        [[nodiscard]] float BuildingHeightAt(Vec2 point) const;

    private:
        void GenerateDistricts(Rng& rng);
        void GenerateRoads(Rng& rng);
        void GenerateBuildings(Rng& rng);
        void GenerateProps(Rng& rng);
        void BuildOccupancy();
        [[nodiscard]] int CellIndex(float world) const;
        void AssignDistrictsToNetwork();
        void PlaceBlockBuildings(std::uint32_t blockIndex, Rng& rng);
        void PlacePerimeterBuildings(const CityBlock& block, std::uint32_t blockIndex,
                                     const District& district, Rng& rng, float depthScale,
                                     float minWidth, float maxWidth, BuildingKind kind);
        void PlaceTower(const CityBlock& block, std::uint32_t blockIndex, const District& district,
                        Rng& rng);
        void PlaceSuburbHouses(const CityBlock& block, std::uint32_t blockIndex,
                               const District& district, Rng& rng);
        void PlaceWarehouses(const CityBlock& block, std::uint32_t blockIndex,
                             const District& district, Rng& rng);
        void PlaceParkPlanting(const CityBlock& block, const District& district, Rng& rng);
        void FinishBuilding(Building& building, const District& district, Rng& rng);

        CityConfig config_;
        RoadNetwork roads_;
        std::vector<District> districts_;
        std::vector<Building> buildings_;
        std::vector<Prop> props_;
        /// Index lists over @ref buildings_, so the population generator can draw a home or a job
        /// without rejection-sampling the whole city.
        std::vector<std::uint32_t> homes_;
        std::vector<std::uint32_t> workplaces_;
        std::vector<std::uint32_t> leisure_;
        std::uint32_t residentCapacity_ = 0;
        std::uint32_t jobCapacity_ = 0;
        /// The jittered coordinates of the arterial grid lines. District (i, j) is the cell
        /// between gridX_[i]..gridX_[i+1] and gridZ_[j]..gridZ_[j+1], so a point's district is two
        /// binary searches rather than a spatial query.
        std::vector<float> gridX_;
        std::vector<float> gridZ_;
        int districtSide_ = 0;

        /// Two metres per cell, one byte per cell holding the height in half-metre steps.
        std::vector<std::uint8_t> occupancy_;
        int occupancySide_ = 0;
        static constexpr float kOccupancyCell = 2.0f;
    };
}
