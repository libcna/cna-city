// SPDX-License-Identifier: MIT
#include "City.hpp"

#include <algorithm>
#include <cmath>

namespace CnaCity
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        /// Sub-stream labels. Each generation stage draws from its own PCG stream so that changing
        /// one of them -- adding a tree species, say -- does not move every building in the city.
        enum Stream : std::uint64_t
        {
            StreamDistricts = 1,
            StreamRoads     = 2,
            StreamBuildings = 3,
            StreamProps     = 4,
        };

        struct ZoneRule
        {
            float streetSpacing;
            float blockAspect;      ///< The second street family's spacing, as a multiple of the first.
            float heightMean;
            float heightSigma;
            float heightMin;
            float heightMax;
            float floorHeight;
            float depthMin;
            float depthMax;
            float widthMin;
            float widthMax;
            BuildingKind kind;
        };

        /// The table the whole skyline comes out of. Heights are metres to the parapet.
        constexpr ZoneRule kZoneRules[kZoneCount] = {
            /* Downtown    */ { 82.0f, 1.25f, 74.0f, 40.0f, 24.0f, 195.0f, 3.95f, 17.0f, 27.0f, 15.0f, 32.0f, BuildingKind::Tower},
            /* Commercial  */ { 92.0f, 1.40f, 25.0f, 10.0f, 11.0f,  58.0f, 3.80f, 13.0f, 21.0f, 11.0f, 26.0f, BuildingKind::Office},
            /* Residential */ { 98.0f, 1.45f, 17.5f,  6.0f,  8.5f,  36.0f, 3.20f, 10.5f, 16.5f,  8.5f, 19.0f, BuildingKind::Apartment},
            /* Suburb      */ {126.0f, 1.20f,  7.4f,  1.5f,  5.0f,  11.5f, 3.05f,  8.0f, 12.0f,  8.0f, 13.0f, BuildingKind::House},
            /* Industrial  */ {168.0f, 1.15f, 11.5f,  3.2f,  6.5f,  19.0f, 5.60f, 22.0f, 46.0f, 24.0f, 60.0f, BuildingKind::Warehouse},
            /* Park        */ {  0.0f, 1.00f,  0.0f,  0.0f,  0.0f,   0.0f, 3.00f,  0.0f,  0.0f,  0.0f,  0.0f, BuildingKind::House},
        };

        const char* const kDistrictPrefix[] = {
            "North", "South", "East", "West", "Upper", "Lower", "Old", "New", "Little", "Great",
            "Kings", "Queens", "Saint", "Free", "High", "Green"};
        const char* const kDistrictStem[] = {
            "haven", "field", "bridge", "port", "gate", "market", "mill", "wood", "hill", "cross",
            "bourne", "stead", "ford", "quay", "moor", "row"};

        /**
         * @brief Clips the infinite line through @p point along @p dir to an axis-aligned box.
         *
         * Liang-Barsky, in the form that returns the parameter interval rather than the endpoints,
         * because the caller wants to drop a whole family of parallel streets and only the ones
         * with a usable interval are worth emitting.
         */
        bool ClipLineToRect(Vec2 point, Vec2 dir, const Bounds2& rect, float& t0, float& t1)
        {
            t0 = -1e9f;
            t1 = 1e9f;
            const float p[4] = {-dir.X, dir.X, -dir.Y, dir.Y};
            const float q[4] = {point.X - rect.min.X, rect.max.X - point.X,
                                point.Y - rect.min.Y, rect.max.Y - point.Y};
            for (int i = 0; i < 4; ++i)
            {
                if (std::fabs(p[i]) < 1e-9f)
                {
                    if (q[i] < 0.0f) return false;   // parallel and outside
                    continue;
                }
                const float r = q[i] / p[i];
                if (p[i] < 0.0f) { if (r > t1) return false; if (r > t0) t0 = r; }
                else             { if (r < t0) return false; if (r < t1) t1 = r; }
            }
            return t1 - t0 > 1.0f;
        }

        /**
         * @brief True when every corner of the footprint lies inside the buildable polygon.
         *
         * The corners are pulled in by a few centimetres before the test. A building placed
         * against a street frontage has its two front corners exactly *on* the boundary, and a
         * point-in-polygon test on the boundary answers whichever way the arithmetic falls -- which
         * is how the first version of this rejected three quarters of every street wall it tried
         * to build.
         */
        bool FootprintFits(const std::vector<Vec2>& polygon, Vec2 center, Vec2 axisU, Vec2 axisV,
                           float halfWidth, float halfDepth)
        {
            constexpr float kInside = 0.06f;
            const Vec2 u = axisU * std::max(0.05f, halfWidth - kInside);
            const Vec2 v = axisV * std::max(0.05f, halfDepth - kInside);
            return PointInPolygon(polygon, center + u + v) &&
                   PointInPolygon(polygon, center + u - v) &&
                   PointInPolygon(polygon, center - u + v) &&
                   PointInPolygon(polygon, center - u - v);
        }
    }

    const char* ZoneName(ZoneType zone)
    {
        switch (zone)
        {
            case ZoneType::Downtown:    return "Downtown";
            case ZoneType::Commercial:  return "Commercial";
            case ZoneType::Residential: return "Residential";
            case ZoneType::Suburb:      return "Suburb";
            case ZoneType::Industrial:  return "Industrial";
            case ZoneType::Park:        return "Park";
        }
        return "?";
    }

    void City::Generate(const CityConfig& config)
    {
        // Everything the generator owns, cleared in one place before any of it runs.
        //
        // The stages used to clear their own outputs, each of them except one: park planting
        // appends to `props_` while the blocks are being built, so `GenerateProps` deliberately
        // does not clear it -- and nothing else did either. Generating twice into the same City
        // therefore produced a city with two of every lamp post, and after the road graph had been
        // rebuilt around them, two of every tree in places there was no longer a park. It was
        // invisible for as long as nothing generated twice, and `Simulation::Initialize` began
        // doing exactly that the day it grew a caller that wanted a second world to compare
        // against.
        config_ = config;
        districts_.clear();
        buildings_.clear();
        props_.clear();
        parked_.clear();
        homes_.clear();
        workplaces_.clear();
        leisure_.clear();
        residentCapacity_ = 0;
        jobCapacity_ = 0;

        Rng root(config.seed);

        Rng districtRng = root.Split(StreamDistricts);
        GenerateDistricts(districtRng);

        Rng roadRng = root.Split(StreamRoads);
        GenerateRoads(roadRng);
        AssignDistrictsToNetwork();

        Rng buildingRng = root.Split(StreamBuildings);
        GenerateBuildings(buildingRng);

        Rng propRng = root.Split(StreamProps);
        GenerateProps(propRng);
        GenerateParking(propRng);

        BuildOccupancy();
    }

    void City::GenerateDistricts(Rng& rng)
    {
        const float span = config_.halfSize * 2.0f;
        districtSide_ = std::max(2, static_cast<int>(std::lround(span / config_.arterialSpacing)));

        // The arterial grid is jittered so that blocks vary in size. The two outer lines are not:
        // they are the city limit, and a ragged edge there reads as a bug rather than as character.
        gridX_.resize(districtSide_ + 1);
        gridZ_.resize(districtSide_ + 1);
        for (int i = 0; i <= districtSide_; ++i)
        {
            const float base = -config_.halfSize + span * static_cast<float>(i) /
                                                   static_cast<float>(districtSide_);
            const bool edge = i == 0 || i == districtSide_;
            gridX_[i] = edge ? base : base + rng.NextFloat(-0.09f, 0.09f) * config_.arterialSpacing;
            gridZ_[i] = edge ? base : base + rng.NextFloat(-0.09f, 0.09f) * config_.arterialSpacing;
        }

        districts_.clear();
        districts_.reserve(static_cast<std::size_t>(districtSide_) * districtSide_);
        for (int j = 0; j < districtSide_; ++j)
            for (int i = 0; i < districtSide_; ++i)
            {
                District d;
                d.id = static_cast<std::uint16_t>(districts_.size());
                d.rect.min = Vec2(gridX_[i], gridZ_[j]);
                d.rect.max = Vec2(gridX_[i + 1], gridZ_[j + 1]);
                d.center = d.rect.Center();

                // Zoning is a function of distance from the centre plus enough noise that the
                // rings are not concentric. Without the noise the city looks like a dartboard;
                // with too much of it, the skyline stops making sense.
                const float radius = Length(d.center) / config_.halfSize;
                const float noisy = radius + rng.NextFloat(-0.13f, 0.13f);
                if (noisy < 0.17f)      d.zone = ZoneType::Downtown;
                else if (noisy < 0.37f) d.zone = ZoneType::Commercial;
                else if (noisy < 0.62f) d.zone = ZoneType::Residential;
                else if (noisy < 0.84f) d.zone = ZoneType::Suburb;
                else                    d.zone = ZoneType::Industrial;

                // Parkland anywhere but the very centre: a city keeps its most valuable land built.
                if (d.zone != ZoneType::Downtown && rng.Chance(config_.parkFraction))
                    d.zone = ZoneType::Park;

                // Downtown keeps the arterials' own orientation -- a rotated grid there would
                // fight the towers. Everywhere else takes a district-wide rotation, which is what
                // gives a procedural city the patchwork look real ones have.
                const ZoneRule& rule = kZoneRules[static_cast<int>(d.zone)];
                if (d.zone == ZoneType::Downtown)
                    d.gridAngle = rng.NextFloat(-0.04f, 0.04f);
                else
                {
                    const float choices[6] = {0.0f, 0.0f, 0.13f, -0.21f, 0.52f, -0.44f};
                    d.gridAngle = choices[rng.NextUInt(6)] + rng.NextFloat(-0.03f, 0.03f);
                }
                d.streetSpacing = rule.streetSpacing * rng.NextFloat(0.9f, 1.12f);
                d.name = std::string(kDistrictPrefix[rng.NextUInt(16)]) + kDistrictStem[rng.NextUInt(16)];
                districts_.push_back(std::move(d));
            }
    }

    void City::GenerateRoads(Rng& rng)
    {
        const float half = config_.halfSize;

        // ---- The arterial grid -----------------------------------------------------------------
        for (int i = 0; i <= districtSide_; ++i)
        {
            roads_.AddSegment(Vec2(gridX_[i], -half), Vec2(gridX_[i], half),
                              i == 0 || i == districtSide_ ? RoadClass::Highway : RoadClass::Arterial);
            roads_.AddSegment(Vec2(-half, gridZ_[i]), Vec2(half, gridZ_[i]),
                              i == 0 || i == districtSide_ ? RoadClass::Highway : RoadClass::Arterial);
        }

        // ---- The boulevard ring ----------------------------------------------------------------
        //
        // A ring road at roughly half the city's radius, drawn as a jittered 56-gon. It is what
        // stops every cross-town trip going through the centre, which shows up in the traffic
        // model as a genuinely different congestion pattern rather than as decoration.
        {
            constexpr int kRingSteps = 56;
            const float ringRadius = half * 0.54f;
            std::vector<Vec2> ring;
            ring.reserve(kRingSteps + 1);
            for (int i = 0; i <= kRingSteps; ++i)
            {
                const float angle = kPi * 2.0f * static_cast<float>(i % kRingSteps) /
                                    static_cast<float>(kRingSteps);
                const float r = ringRadius * (1.0f + 0.055f * std::sin(angle * 3.0f) +
                                              0.03f * std::cos(angle * 5.0f + 1.1f));
                ring.push_back(FromHeading(angle) * r);
            }
            roads_.AddPolyline(ring, RoadClass::Arterial);
        }

        // ---- Diagonal avenues ------------------------------------------------------------------
        //
        // The single most effective realism trick in the file. A diagonal across an orthogonal
        // grid produces triangular blocks and five- and six-way junctions, and both of those are
        // what a real city has and a generated grid does not.
        for (int i = 0; i < config_.diagonalAvenues; ++i)
        {
            const float angle = kPi * static_cast<float>(i) /
                                std::max(1.0f, static_cast<float>(config_.diagonalAvenues)) +
                                rng.NextFloat(0.22f, 0.55f);
            const Vec2 dir = FromHeading(angle);
            const Vec2 through(rng.NextFloat(-0.22f, 0.22f) * half, rng.NextFloat(-0.22f, 0.22f) * half);
            Bounds2 city;
            city.min = Vec2(-half, -half);
            city.max = Vec2(half, half);
            float t0 = 0.0f, t1 = 0.0f;
            if (ClipLineToRect(through, dir, city, t0, t1))
                roads_.AddSegment(through + dir * t0, through + dir * t1, RoadClass::Arterial);
        }

        // ---- District street grids --------------------------------------------------------------
        for (const District& d : districts_)
        {
            if (d.zone == ZoneType::Park) continue;   // parkland gets no streets cut through it
            const ZoneRule& rule = kZoneRules[static_cast<int>(d.zone)];
            const Vec2 u = FromHeading(d.gridAngle);
            const Vec2 v = Perp(u);

            // A rotated grid needs to be swept far enough to cover the cell's diagonal, which is
            // why the loop bound is the half-diagonal rather than the half-width.
            const Vec2 extent = d.rect.Extent();
            const float reach = Length(extent) + d.streetSpacing;
            const float spacingU = d.streetSpacing;
            const float spacingV = d.streetSpacing * rule.blockAspect;

            // The streets are clipped to the cell *exactly*, so that each one ends on the arterial
            // it is meant to meet rather than a hair short of it. Stopping short was the first
            // version, and it was wrong in a way that is worth recording: every district's street
            // grid became an island, the face between it and the surrounding arterials was an
            // annulus rather than a polygon, and the block extractor silently returned a handful
            // of enormous self-touching "blocks" instead of a thousand real ones.
            const Bounds2& inner = d.rect;

            const float offsetU = rng.NextFloat(0.0f, spacingU);
            const float offsetV = rng.NextFloat(0.0f, spacingV);
            const int stepsU = static_cast<int>(reach / spacingU) + 1;
            const int stepsV = static_cast<int>(reach / spacingV) + 1;

            for (int k = -stepsV; k <= stepsV; ++k)
            {
                const Vec2 point = d.center + v * (static_cast<float>(k) * spacingV + offsetV);
                float t0 = 0.0f, t1 = 0.0f;
                if (!ClipLineToRect(point, u, inner, t0, t1)) continue;
                // Every third street in a family is a collector: two lanes each way, and the
                // route planner prefers it, which is what gives a district a spine.
                const RoadClass cls = (k % 3 == 0) ? RoadClass::Collector : RoadClass::Local;
                roads_.AddSegment(point + u * t0, point + u * t1, cls);
            }
            for (int k = -stepsU; k <= stepsU; ++k)
            {
                const Vec2 point = d.center + u * (static_cast<float>(k) * spacingU + offsetU);
                float t0 = 0.0f, t1 = 0.0f;
                if (!ClipLineToRect(point, v, inner, t0, t1)) continue;
                const RoadClass cls = (k % 3 == 0) ? RoadClass::Collector : RoadClass::Local;
                roads_.AddSegment(point + v * t0, point + v * t1, cls);
            }
        }

        roads_.Build();
        roads_.ExtractBlocks(config_.blockSetback);
    }

    void City::AssignDistrictsToNetwork()
    {
        for (RoadNode& node : roads_.mutableNodes())
            node.district = DistrictAt(node.position);
        for (RoadSegment& segment : roads_.mutableSegments())
        {
            const Vec2 mid = (roads_.nodes()[segment.nodeA].position +
                              roads_.nodes()[segment.nodeB].position) * 0.5f;
            segment.district = DistrictAt(mid);
        }
    }

    int City::CellIndex(float world) const
    {
        return static_cast<int>(std::floor((world + config_.halfSize + 60.0f) / kOccupancyCell));
    }

    std::uint16_t City::DistrictAt(Vec2 point) const
    {
        if (gridX_.size() < 2) return 0;
        const auto findCell = [](const std::vector<float>& grid, float value) {
            const auto it = std::upper_bound(grid.begin(), grid.end(), value);
            const int index = static_cast<int>(it - grid.begin()) - 1;
            return Clamp(index, 0, static_cast<int>(grid.size()) - 2);
        };
        const int i = findCell(gridX_, point.X);
        const int j = findCell(gridZ_, point.Y);
        return static_cast<std::uint16_t>(j * districtSide_ + i);
    }

    void City::GenerateBuildings(Rng& rng)
    {
        buildings_.clear();
        buildings_.reserve(roads_.blocks().size() * 8);
        for (std::uint32_t b = 0; b < roads_.blocks().size(); ++b)
            PlaceBlockBuildings(b, rng);

        // Doorways were placed on the pavement during layout; binding each to a road node is what
        // turns a building into a place the route planner can reach.
        for (Building& building : buildings_)
            building.doorNode = roads_.FindNearestNode(building.doorway);

        homes_.clear();
        workplaces_.clear();
        leisure_.clear();
        residentCapacity_ = 0;
        jobCapacity_ = 0;
        for (std::uint32_t i = 0; i < buildings_.size(); ++i)
        {
            const Building& building = buildings_[i];
            if (building.residents > 0)
            {
                homes_.push_back(i);
                residentCapacity_ += building.residents;
            }
            if (building.jobs > 0)
            {
                workplaces_.push_back(i);
                jobCapacity_ += building.jobs;
            }
            if (building.kind == BuildingKind::Shop || building.kind == BuildingKind::Office)
                leisure_.push_back(i);
        }
    }

    void City::PlaceBlockBuildings(std::uint32_t blockIndex, Rng& rng)
    {
        const CityBlock& block = roads_.blocks()[blockIndex];
        if (block.buildable.size() < 3 || block.area < 60.0f) return;

        const District& district = districts_[DistrictAt(block.centroid)];
        const ZoneRule& rule = kZoneRules[static_cast<int>(district.zone)];

        switch (district.zone)
        {
            case ZoneType::Park:
                PlaceParkPlanting(block, district, rng);
                return;
            case ZoneType::Suburb:
                PlaceSuburbHouses(block, blockIndex, district, rng);
                return;
            case ZoneType::Industrial:
                PlaceWarehouses(block, blockIndex, district, rng);
                return;
            default:
                break;
        }

        PlacePerimeterBuildings(block, blockIndex, district, rng, 1.0f,
                                rule.widthMin, rule.widthMax, rule.kind);

        // A downtown block with room to spare gets a tower set back behind the street wall --
        // the shape of every real central business district, and the reason its skyline has
        // depth rather than being one flat wall of glass.
        if (district.zone == ZoneType::Downtown && block.area > 5200.0f && rng.Chance(0.55f))
            PlaceTower(block, blockIndex, district, rng);
    }

    void City::PlacePerimeterBuildings(const CityBlock& block, std::uint32_t blockIndex,
                                       const District& district, Rng& rng, float depthScale,
                                       float minWidth, float maxWidth, BuildingKind kind)
    {
        const ZoneRule& rule = kZoneRules[static_cast<int>(district.zone)];
        const std::vector<Vec2>& poly = block.buildable;
        const std::size_t edgeCount = poly.size();

        for (std::size_t e = 0; e < edgeCount; ++e)
        {
            const Vec2 a = poly[e];
            const Vec2 b = poly[(e + 1) % edgeCount];
            const float edgeLength = Distance(a, b);
            if (edgeLength < minWidth + 3.0f) continue;

            const Vec2 along = Normalized(b - a);
            const Vec2 inward = Perp(along);        // the buildable ring is counter-clockwise

            // One base height per street frontage, with only a small per-building deviation on
            // top of it. A terrace whose every house is an independent draw from the height
            // distribution looks like a bar chart; real streets step, they do not jump.
            const float frontageHeight = Clamp(rng.NextGaussian(rule.heightMean, rule.heightSigma),
                                               rule.heightMin, rule.heightMax);

            float cursor = rng.NextFloat(0.6f, 2.0f);
            // The tail of a frontage is filled by a narrow plot rather than left blank: a terrace
            // that stops eight metres short of the corner is the tell that a generator built it.
            const float tailWidth = minWidth * 0.7f;
            while (edgeLength - cursor > tailWidth)
            {
                const float remaining = edgeLength - cursor - 0.6f;
                const float width = remaining < minWidth
                                        ? remaining
                                        : rng.NextFloat(minWidth, std::min(maxWidth, remaining));
                float depth = rng.NextFloat(rule.depthMin, rule.depthMax) * depthScale;
                const Vec2 frontMid = a + along * (cursor + width * 0.5f);

                // Shrink the plot until it fits the block rather than skipping it: a triangular
                // block's sharp corner has room for a thin building and none for a fat one, and
                // leaving it empty is what makes generated cities full of inexplicable gaps.
                Vec2 center = frontMid + inward * (depth * 0.5f);
                int attempts = 0;
                while (attempts < 6 &&
                       !FootprintFits(poly, center, along, inward, width * 0.5f, depth * 0.5f))
                {
                    depth *= 0.72f;
                    center = frontMid + inward * (depth * 0.5f);
                    ++attempts;
                }
                if (depth < 4.0f ||
                    !FootprintFits(poly, center, along, inward, width * 0.5f, depth * 0.5f))
                {
                    cursor += width * 0.5f + 0.8f;
                    continue;
                }

                Building building;
                building.center = center;
                building.halfExtent = Vec2(width * 0.5f, depth * 0.5f);
                building.rotation = Heading(along);
                building.height = Clamp(frontageHeight * rng.NextFloat(0.82f, 1.20f),
                                        rule.heightMin, rule.heightMax);
                building.kind = kind;
                building.block = blockIndex;
                building.district = district.id;
                // The door is on the street side, a metre out from the facade, which is the
                // pavement the agent will actually stand on.
                building.doorway = frontMid - inward * 1.1f;

                // A tall building on a shallow plot is a stick. Real ones widen or stay short,
                // so the height is capped by the plot's smaller dimension.
                const float slenderCap = std::min(width, depth) * 9.5f;
                if (building.height > slenderCap) building.height = std::max(rule.heightMin, slenderCap);

                // Above roughly ten storeys a street-wall building steps back, which is what makes
                // a downtown block read as layered rather than as one extruded polygon.
                if (building.height > 38.0f && rng.Chance(0.7f))
                {
                    building.podiumHeight = rng.NextFloat(9.0f, 18.0f);
                    building.podiumHalfExtent = building.halfExtent;
                    const float shrink = rng.NextFloat(0.68f, 0.86f);
                    building.halfExtent = Vec2(building.halfExtent.X * shrink,
                                               building.halfExtent.Y * shrink);
                }

                FinishBuilding(building, district, rng);
                buildings_.push_back(building);
                cursor += width + rng.NextFloat(0.2f, 1.9f);
            }
        }
    }

    void City::PlaceTower(const CityBlock& block, std::uint32_t blockIndex, const District& district,
                          Rng& rng)
    {
        const ZoneRule& rule = kZoneRules[static_cast<int>(ZoneType::Downtown)];
        // The inner ring is what is left after the street wall has taken its depth; a tower placed
        // on the centroid without it would push its corners through the buildings in front.
        const std::vector<Vec2> core = InsetPolygon(block.buildable, rule.depthMax + 3.0f);
        if (core.size() < 3 || PolygonArea(core) < 320.0f) return;

        const Vec2 center = PolygonCentroid(core);
        if (!PointInPolygon(core, center)) return;

        const float rotation = rng.NextFloat(-kPi, kPi);
        const Vec2 along = FromHeading(rotation);
        const Vec2 across = Perp(along);
        float halfW = rng.NextFloat(11.0f, 21.0f);
        float halfD = halfW * rng.NextFloat(0.72f, 1.0f);
        int attempts = 0;
        while (attempts < 8 && !FootprintFits(core, center, along, across, halfW, halfD))
        {
            halfW *= 0.82f;
            halfD *= 0.82f;
            ++attempts;
        }
        if (halfW < 7.0f || !FootprintFits(core, center, along, across, halfW, halfD)) return;

        Building tower;
        tower.center = center;
        tower.halfExtent = Vec2(halfW, halfD);
        tower.rotation = rotation;
        tower.height = Clamp(rng.NextGaussian(rule.heightMean * 1.35f, rule.heightSigma),
                             56.0f, rule.heightMax);
        tower.kind = BuildingKind::Tower;
        tower.block = blockIndex;
        tower.district = district.id;
        tower.doorway = center + across * (halfD + 2.0f);
        tower.podiumHeight = rng.NextFloat(12.0f, 22.0f);
        tower.podiumHalfExtent = Vec2(halfW * rng.NextFloat(1.15f, 1.35f),
                                      halfD * rng.NextFloat(1.15f, 1.35f));
        if (!FootprintFits(core, center, along, across, tower.podiumHalfExtent.X,
                           tower.podiumHalfExtent.Y))
            tower.podiumHeight = 0.0f;

        FinishBuilding(tower, district, rng);
        buildings_.push_back(tower);
    }

    void City::PlaceSuburbHouses(const CityBlock& block, std::uint32_t blockIndex,
                                 const District& district, Rng& rng)
    {
        const ZoneRule& rule = kZoneRules[static_cast<int>(ZoneType::Suburb)];
        // Suburban plots front the street the same way, they are just smaller and further apart --
        // the gap between them is the garden, and it is the whole visual difference.
        PlacePerimeterBuildings(block, blockIndex, district, rng, 1.0f, rule.widthMin,
                                rule.widthMax, BuildingKind::House);
    }

    void City::PlaceWarehouses(const CityBlock& block, std::uint32_t blockIndex,
                               const District& district, Rng& rng)
    {
        const ZoneRule& rule = kZoneRules[static_cast<int>(ZoneType::Industrial)];
        const std::vector<Vec2> yard = InsetPolygon(block.buildable, 7.0f);
        if (yard.size() < 3 || PolygonArea(yard) < 600.0f)
        {
            PlacePerimeterBuildings(block, blockIndex, district, rng, 0.7f, 18.0f, 34.0f,
                                    BuildingKind::Warehouse);
            return;
        }

        const int count = rng.NextInt(1, 3);
        for (int i = 0; i < count; ++i)
        {
            const Vec2 centroid = PolygonCentroid(yard);
            const Vec2 jitter(rng.NextFloat(-1.0f, 1.0f), rng.NextFloat(-1.0f, 1.0f));
            const Vec2 center = centroid + jitter * (Length(yard[0] - centroid) * 0.35f);
            const float rotation = district.gridAngle + (rng.Chance(0.5f) ? 0.0f : kPi * 0.5f);
            const Vec2 along = FromHeading(rotation);
            const Vec2 across = Perp(along);
            float halfW = rng.NextFloat(rule.widthMin, rule.widthMax) * 0.5f;
            float halfD = rng.NextFloat(rule.depthMin, rule.depthMax) * 0.5f;
            int attempts = 0;
            while (attempts < 8 && !FootprintFits(yard, center, along, across, halfW, halfD))
            {
                halfW *= 0.78f;
                halfD *= 0.78f;
                ++attempts;
            }
            if (halfW < 8.0f || !FootprintFits(yard, center, along, across, halfW, halfD)) continue;

            Building shed;
            shed.center = center;
            shed.halfExtent = Vec2(halfW, halfD);
            shed.rotation = rotation;
            shed.height = Clamp(rng.NextGaussian(rule.heightMean, rule.heightSigma),
                                rule.heightMin, rule.heightMax);
            shed.kind = BuildingKind::Warehouse;
            shed.block = blockIndex;
            shed.district = district.id;
            shed.doorway = center + across * (halfD + 3.0f);
            FinishBuilding(shed, district, rng);
            buildings_.push_back(shed);
        }
    }

    void City::PlaceParkPlanting(const CityBlock& block, const District& district, Rng& rng)
    {
        // Density by area rather than a fixed count, so a small pocket park does not end up as
        // dense as the city's central green.
        const int trees = static_cast<int>(block.area / 190.0f);
        const Bounds2 bounds = [&] {
            Bounds2 b;
            for (Vec2 p : block.buildable) b.Add(p);
            return b;
        }();
        for (int i = 0; i < trees; ++i)
        {
            const Vec2 candidate(rng.NextFloat(bounds.min.X, bounds.max.X),
                                 rng.NextFloat(bounds.min.Y, bounds.max.Y));
            if (!PointInPolygon(block.buildable, candidate)) continue;
            Prop tree;
            tree.position = candidate;
            tree.kind = rng.Chance(0.72f) ? PropKind::TreeRound : PropKind::TreeConifer;
            tree.rotation = rng.NextFloat(-kPi, kPi);
            tree.scale = rng.NextFloat(0.75f, 1.6f);
            tree.district = district.id;
            props_.push_back(tree);
        }
        const int benches = static_cast<int>(block.area / 2400.0f);
        for (int i = 0; i < benches; ++i)
        {
            const Vec2 candidate(rng.NextFloat(bounds.min.X, bounds.max.X),
                                 rng.NextFloat(bounds.min.Y, bounds.max.Y));
            if (!PointInPolygon(block.buildable, candidate)) continue;
            Prop bench;
            bench.position = candidate;
            bench.kind = rng.Chance(0.7f) ? PropKind::Bench : PropKind::Bin;
            bench.rotation = rng.NextFloat(-kPi, kPi);
            bench.district = district.id;
            props_.push_back(bench);
        }
    }

    void City::FinishBuilding(Building& building, const District& district, Rng& rng)
    {
        const ZoneRule& rule = kZoneRules[static_cast<int>(district.zone)];
        building.floors = static_cast<std::uint8_t>(
            Clamp(static_cast<int>(std::lround(building.height / rule.floorHeight)), 1, 255));
        building.variant = static_cast<std::uint8_t>(rng.NextUInt(4));

        const float footprint = 4.0f * building.halfExtent.X * building.halfExtent.Y;
        const float floorArea = footprint * static_cast<float>(building.floors);

        // Occupancies are per usable square metre and roughly match real densities: ~38 m2 of
        // dwelling per resident, ~16 m2 of office per desk, ~120 m2 of shed per warehouse job.
        // They matter because they are what makes the morning commute go the right way.
        switch (building.kind)
        {
            case BuildingKind::Tower:
                building.jobs = static_cast<std::uint32_t>(floorArea / 24.0f);
                building.residents = static_cast<std::uint32_t>(floorArea * 0.06f / 34.0f);
                break;
            case BuildingKind::Office:
                building.jobs = static_cast<std::uint32_t>(floorArea / 26.0f);
                building.residents = static_cast<std::uint32_t>(floorArea * 0.22f / 32.0f);
                break;
            case BuildingKind::Apartment:
                building.residents = static_cast<std::uint32_t>(floorArea * 0.88f / 27.0f);
                // The ground floor of a perimeter block is shops: a handful of jobs, and the
                // reason a residential street has anywhere to go at lunchtime.
                building.jobs = static_cast<std::uint32_t>(footprint / 34.0f);
                if (rng.Chance(0.18f)) building.kind = BuildingKind::Shop;
                break;
            case BuildingKind::House:
                building.residents = static_cast<std::uint32_t>(
                    std::max(1L, std::lround(rng.NextGaussian(2.7f, 1.2f))));
                building.jobs = 0;
                break;
            case BuildingKind::Shop:
                building.jobs = static_cast<std::uint32_t>(floorArea / 42.0f);
                building.residents = static_cast<std::uint32_t>(floorArea * 0.5f / 32.0f);
                break;
            case BuildingKind::Warehouse:
                building.jobs = static_cast<std::uint32_t>(floorArea / 150.0f);
                building.residents = 0;
                break;
        }
    }

    void City::GenerateParking(Rng& rng)
    {
        parked_.clear();
        const std::vector<RoadNode>& nodes = roads_.nodes();
        // 5.6 m of kerb per car: a five-metre car and the gap a driver actually leaves.
        constexpr float kBay = 5.6f;

        for (const RoadSegment& segment : roads_.segments())
        {
            // Only where a car may legally stand: no parking on the ring highway, none in an
            // alley, and none on an arterial, where the kerb lane is a running lane.
            if (segment.roadClass != RoadClass::Local && segment.roadClass != RoadClass::Collector)
                continue;
            const ZoneType zone = districts_[segment.district].zone;
            if (zone == ZoneType::Park) continue;

            const RoadProfile& profile = ProfileOf(segment.roadClass);
            const Vec2 a = nodes[segment.nodeA].position;
            const Vec2 side = Perp(segment.direction);
            // Clear of the junction at either end: the last few metres of a road are a bay nobody
            // is allowed to use, and a car parked across a crossing looks like a bug.
            const float usable = segment.length - 2.0f * (profile.carriagewayHalfWidth + 3.0f);
            if (usable < kBay) continue;
            const int bays = static_cast<int>(usable / kBay);

            // Industry and downtown have loading bays and car parks rather than residents' cars,
            // so their kerbs are emptier; a suburb's are fuller.
            const float occupancy = config_.kerbOccupancy *
                                    (zone == ZoneType::Suburb      ? 1.15f
                                     : zone == ZoneType::Residential ? 1.05f
                                     : zone == ZoneType::Industrial  ? 0.45f
                                     : zone == ZoneType::Downtown    ? 0.55f
                                                                     : 0.85f);

            for (int s = -1; s <= 1; s += 2)
                for (int bay = 0; bay < bays; ++bay)
                {
                    if (!rng.Chance(Saturate(occupancy))) continue;
                    const float along = profile.carriagewayHalfWidth + 3.0f +
                                        (static_cast<float>(bay) + 0.5f) * kBay;
                    ParkedVehicle car;
                    // Half a metre off the kerb line, which is where a parked car sits: outside the
                    // running lane and not on the pavement.
                    car.position = a + segment.direction * along +
                                   side * ((profile.carriagewayHalfWidth - 1.05f) *
                                           static_cast<float>(s));
                    // Facing the direction of travel on that side of the road, with the small
                    // misalignment every real parked car has.
                    car.rotation = Heading(segment.direction * static_cast<float>(-s)) +
                                   rng.NextFloat(-0.035f, 0.035f);
                    const std::uint32_t roll = rng.NextUInt(100);
                    car.kind = static_cast<std::uint8_t>(roll < 52 ? 0 : roll < 84 ? 1 : roll < 95 ? 2 : 5);
                    car.appearance = static_cast<std::uint8_t>(rng.NextUInt(8));
                    parked_.push_back(car);
                }
        }
    }

    void City::BuildOccupancy()
    {
        const float span = config_.halfSize * 2.0f + 120.0f;
        occupancySide_ = std::max(1, static_cast<int>(span / kOccupancyCell) + 1);
        occupancy_.assign(static_cast<std::size_t>(occupancySide_) * occupancySide_, 0);

        // Each building stamps its own rotated footprint by walking the axis-aligned bound and
        // testing the local coordinates. Rasterising the rotated rectangle exactly would be a
        // scanline fill; at two metres per cell the difference is one cell of overdraw at the
        // corners, and a camera pushed one cell further out of a wall is not a defect.
        for (const Building& building : buildings_)
        {
            const Vec2 axisU = FromHeading(building.rotation);
            const Vec2 axisV = Perp(axisU);
            const float reach = Length(building.halfExtent) + kOccupancyCell;
            const auto height = static_cast<std::uint8_t>(
                Clamp(static_cast<int>(building.height * 2.0f), 1, 255));
            const int x0 = CellIndex(building.center.X - reach);
            const int x1 = CellIndex(building.center.X + reach);
            const int y0 = CellIndex(building.center.Y - reach);
            const int y1 = CellIndex(building.center.Y + reach);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    if (x < 0 || y < 0 || x >= occupancySide_ || y >= occupancySide_) continue;
                    const Vec2 at(-config_.halfSize - 60.0f + (static_cast<float>(x) + 0.5f) * kOccupancyCell,
                                  -config_.halfSize - 60.0f + (static_cast<float>(y) + 0.5f) * kOccupancyCell);
                    const Vec2 local = at - building.center;
                    if (std::fabs(Dot(local, axisU)) > building.halfExtent.X + 0.6f) continue;
                    if (std::fabs(Dot(local, axisV)) > building.halfExtent.Y + 0.6f) continue;
                    std::uint8_t& cell = occupancy_[static_cast<std::size_t>(y) * occupancySide_ + x];
                    if (height > cell) cell = height;
                }
        }
    }

    float City::BuildingHeightAt(Vec2 point) const
    {
        if (occupancy_.empty()) return 0.0f;
        const int x = CellIndex(point.X);
        const int y = CellIndex(point.Y);
        if (x < 0 || y < 0 || x >= occupancySide_ || y >= occupancySide_) return 0.0f;
        return static_cast<float>(occupancy_[static_cast<std::size_t>(y) * occupancySide_ + x]) * 0.5f;
    }

    void City::GenerateProps(Rng& rng)
    {
        // Park planting was already appended while the blocks were being built, so this stage adds
        // to props_ rather than clearing it.
        const std::vector<RoadNode>& nodes = roads_.nodes();

        for (const RoadSegment& segment : roads_.segments())
        {
            const RoadProfile& profile = ProfileOf(segment.roadClass);
            if (!profile.hasStreetLights) continue;
            if (segment.length < 12.0f) continue;

            const Vec2 a = nodes[segment.nodeA].position;
            const Vec2 dir = segment.direction;
            const Vec2 side = Perp(dir);
            const ZoneType zone = districts_[segment.district].zone;

            // Lamp spacing follows the class: a lit arterial is a continuous ribbon of sodium, a
            // local street is dots. Both sides on the bigger roads, one side on the small ones.
            const float spacing = segment.roadClass == RoadClass::Highway   ? 42.0f
                                : segment.roadClass == RoadClass::Arterial  ? 30.0f
                                : segment.roadClass == RoadClass::Collector ? 32.0f
                                                                            : 36.0f;
            const bool bothSides = static_cast<int>(segment.roadClass) <= static_cast<int>(RoadClass::Collector);
            const float lampOffset = profile.carriagewayHalfWidth + profile.sidewalkWidth * 0.35f;

            const int lamps = std::max(1, static_cast<int>(segment.length / spacing));
            for (int i = 0; i < lamps; ++i)
            {
                const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(lamps);
                const Vec2 base = a + dir * (t * segment.length);
                for (int s = 0; s < (bothSides ? 2 : 1); ++s)
                {
                    const float sign = (s == 0) ? 1.0f : -1.0f;
                    Prop lamp;
                    lamp.position = base + side * (lampOffset * sign);
                    // The lamp head overhangs the carriageway, so it faces the road.
                    lamp.rotation = Heading(side * -sign);
                    lamp.kind = PropKind::StreetLamp;
                    lamp.district = segment.district;
                    lamp.scale = static_cast<int>(segment.roadClass) <= static_cast<int>(RoadClass::Arterial)
                                     ? 1.25f : 1.0f;
                    props_.push_back(lamp);
                }
            }

            // Street trees, on the streets that would actually have them. An industrial estate
            // and a ring road get none, and their absence is as legible as the planting is.
            if (zone == ZoneType::Residential || zone == ZoneType::Commercial || zone == ZoneType::Suburb)
            {
                if (profile.sidewalkWidth > 1.5f)
                {
                    const int trees = std::max(0, static_cast<int>(segment.length / 21.0f));
                    for (int i = 0; i < trees; ++i)
                    {
                        if (!rng.Chance(0.62f)) continue;
                        const float t = (static_cast<float>(i) + rng.NextFloat(0.2f, 0.8f)) /
                                        static_cast<float>(std::max(1, trees));
                        const float sign = rng.Chance(0.5f) ? 1.0f : -1.0f;
                        Prop tree;
                        tree.position = a + dir * (t * segment.length) +
                                        side * ((profile.carriagewayHalfWidth + profile.sidewalkWidth * 0.72f) * sign);
                        tree.kind = PropKind::TreeRound;
                        tree.rotation = rng.NextFloat(-kPi, kPi);
                        tree.scale = rng.NextFloat(0.8f, 1.25f);
                        tree.district = segment.district;
                        props_.push_back(tree);
                    }
                }
            }

            // Bus shelters used to be scattered along the arterials at random, which put them
            // where no bus ever stopped. They are drawn at the actual stops now -- see
            // CityGame::CollectInstances -- because a shelter is where a service calls, and a
            // shelter that is not is a piece of set dressing that contradicts the simulation
            // running past it.
        }

        // One signal head per approach, set back on the corner it governs. Placing them from the
        // node's own angle-sorted incidences means a five-way junction gets five heads pointing
        // the right ways without anything having to special-case it.
        for (std::uint32_t n = 0; n < nodes.size(); ++n)
        {
            const RoadNode& node = nodes[n];
            if (!node.signalised) continue;
            const RoadProfile& profile = ProfileOf(node.highestClass);
            for (std::uint32_t k = 0; k < node.incidentCount; ++k)
            {
                const Incidence& inc = roads_.incidenceBegin(n)[k];
                const Vec2 dir = FromHeading(inc.heading);
                Prop signal;
                signal.position = node.position + dir * (profile.carriagewayHalfWidth + 2.2f) +
                                  Perp(dir) * -(profile.carriagewayHalfWidth + 1.6f);
                signal.rotation = inc.heading + kPi;
                signal.kind = PropKind::TrafficSignal;
                signal.district = node.district;
                signal.node = n;
                signal.incidence = node.firstIncident + k;
                props_.push_back(signal);
            }
        }
    }
}
