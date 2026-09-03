// SPDX-License-Identifier: MIT
#include "BusNetwork.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "City.hpp"
#include "Pathfinder.hpp"
#include "Rng.hpp"

namespace CnaCity
{
    namespace
    {
        /// Stops closer together than this are the same stop as far as a passenger is concerned,
        /// and a bus that pulls up every eighty metres never reaches its cruising speed.
        constexpr float kStopSpacing = 210.0f;
        /// Buses do not do the arterial's 50: they do the arterial's 50 minus the stopping.
        constexpr float kCruiseSpeed = 12.5f;
        constexpr float kAcceleration = 1.15f;
        /// Long enough to see, short enough that a route of a dozen stops is not an hour round.
        constexpr float kDwellSeconds = 12.0f;
        /// Bumper to bumper, near enough: a bus is twelve metres long and this is measured centre
        /// to centre, so anything below this is one bus inside another.
        constexpr float kFollowingGap = 15.0f;
        /// How far right of the centreline the middle of the kerbside lane is, for a road of this
        /// class. Buses are not in the lane arrays -- see the class comment -- so this stands in
        /// for a lane index, and it has to agree with Traffic::Placement or a bus drives down the
        /// oncoming carriageway.
        float NearsideOffset(RoadClass roadClass)
        {
            const RoadProfile& profile = ProfileOf(roadClass);
            return profile.laneWidth * (static_cast<float>(profile.lanesPerSide) - 0.5f);
        }
    }

    void BusNetwork::Generate(const City& city, Pathfinder& pathfinder, int routeCount,
                              std::uint64_t seed)
    {
        stops_.clear();
        routes_.clear();
        buses_.clear();
        if (routeCount <= 0) return;

        Rng rng(seed ^ 0x8E3Bu);
        const RoadNetwork& roads = city.roads();

        // --- Stops --------------------------------------------------------------------------
        //
        // On the arterials, because that is where the shelters are and where a twelve-metre
        // vehicle belongs. Spaced by a fixed minimum rather than picked at random: a route whose
        // stops happen to bunch spends its whole day accelerating and braking, and one whose stops
        // happen to be a kilometre apart is not a bus service.
        std::vector<std::uint32_t> candidates;
        for (std::uint32_t n = 0; n < roads.nodes().size(); ++n)
            if (roads.nodes()[n].highestClass == RoadClass::Arterial ||
                roads.nodes()[n].highestClass == RoadClass::Collector)
                candidates.push_back(n);
        if (candidates.size() < 8) return;

        for (std::uint32_t node : candidates)
        {
            const Vec2 position = roads.nodes()[node].position;
            bool tooClose = false;
            for (const BusStop& existing : stops_)
                if (DistanceSq(existing.position, position) < kStopSpacing * kStopSpacing)
                {
                    tooClose = true;
                    break;
                }
            if (tooClose) continue;

            BusStop stop;
            stop.node = node;
            stop.position = position;
            stop.kerb = position;
            stop.name = city.districts()[roads.nodes()[node].district].name;
            stops_.push_back(std::move(stop));
        }
        if (stops_.size() < 6) { stops_.clear(); return; }

        // --- Routes -------------------------------------------------------------------------
        //
        // Each route is grown greedily from a seed stop, always taking the nearest stop that is
        // not already on this route and that does not double back on the last leg. The heading
        // test is what stops a route becoming a zigzag between two adjacent stops: without it the
        // nearest unused stop is very often the one behind you.
        // A dozen stops a route. Deriving this from the stop count divided by the route count was
        // the first version and it produced fourteen routes of five stops each: a "route" with five
        // calls spread over four kilometres is an express service to nowhere, and the stops between
        // them were served by nothing at all.
        const int perRoute = 14;
        std::vector<int> served(stops_.size(), 0);

        for (int r = 0; r < routeCount; ++r)
        {
            BusRoute route;
            route.number = static_cast<std::uint8_t>(10 + r * 7);

            // Start from a stop nothing serves yet, so the routes spread over the city instead of
            // all growing out of the same corner.
            std::uint32_t current = kNoStop;
            int fewest = std::numeric_limits<int>::max();
            for (std::uint32_t s = 0; s < stops_.size(); ++s)
            {
                const int score = served[s] * 4 + static_cast<int>(rng.NextUInt(4));
                if (score < fewest) { fewest = score; current = s; }
            }
            if (current == kNoStop) break;

            route.stops.push_back(current);
            Vec2 heading = FromHeading(rng.NextFloat(-3.14159f, 3.14159f));
            for (int k = 1; k < perRoute; ++k)
            {
                std::uint32_t best = kNoStop;
                float bestScore = std::numeric_limits<float>::max();
                const Vec2 at = stops_[current].position;
                for (std::uint32_t s = 0; s < stops_.size(); ++s)
                {
                    if (std::find(route.stops.begin(), route.stops.end(), s) != route.stops.end())
                        continue;
                    const Vec2 delta = stops_[s].position - at;
                    const float d = Length(delta);
                    if (d < 1.0f || d > 1400.0f) continue;
                    // Distance, penalised for turning back and for stops that are already busy.
                    const float turn = 1.0f - Dot(Normalized(delta), heading);   // 0 straight on, 2 back
                    const float score = d * (1.0f + 0.85f * turn) +
                                        static_cast<float>(served[s]) * 140.0f;
                    if (score < bestScore) { bestScore = score; best = s; }
                }
                if (best == kNoStop) break;
                heading = Normalized(stops_[best].position - at);
                current = best;
                route.stops.push_back(best);
            }
            if (route.stops.size() < 4) continue;

            // --- The driving line ---------------------------------------------------------
            //
            // Consecutive stops are joined by an actual road path, so the bus drives round the
            // streets. A leg the router cannot serve drops the stop rather than the route: a
            // straight line between two arterial junctions would put a bus through the middle of
            // a city block, and one such leg in a route of twelve is enough for the whole thing
            // to read as broken.
            std::vector<std::uint32_t> kept;
            std::vector<std::uint32_t> path;
            const std::size_t legs = route.stops.size();
            for (std::size_t i = 0; i < legs; ++i)
            {
                const std::uint32_t fromStop = route.stops[i];
                const std::uint32_t toStop = route.stops[(i + 1) % legs];
                path.clear();
                const std::uint32_t length = pathfinder.FindPath(
                    stops_[fromStop].node, stops_[toStop].node, TravelMode::Car, path);
                if (length < 2) continue;

                if (kept.empty() || kept.back() != fromStop)
                {
                    kept.push_back(fromStop);
                    route.stopDistance.push_back(0.0f);   // filled in below
                }
                for (std::uint32_t p = 0; p < length; ++p)
                {
                    const Vec2 point = roads.nodes()[path[p]].position;
                    if (!route.points.empty() && DistanceSq(route.points.back(), point) < 0.25f)
                        continue;
                    route.points.push_back(point);
                    route.node.push_back(path[p]);
                    // The road being driven, not the junction being passed: a node's own class is
                    // the most important road meeting it, so at every crossing of an arterial a
                    // bus on a collector would step out into the arterial's kerbside lane.
                    const std::uint32_t leg =
                        p + 1 < length ? roads.FindSegmentBetween(path[p], path[p + 1])
                                       : roads.FindSegmentBetween(path[p - 1], path[p]);
                    route.offset.push_back(NearsideOffset(
                        leg == 0xFFFFFFFFu ? RoadClass::Collector
                                           : roads.segments()[leg].roadClass));
                    route.signalNode.push_back(roads.nodes()[path[p]].signalised ? path[p]
                                                                                 : 0xFFFFFFFFu);
                }
            }
            if (kept.size() < 4 || route.points.size() < 4) continue;
            route.stops = kept;

            // Close the loop back to the first point, and measure it.
            if (DistanceSq(route.points.back(), route.points.front()) > 0.25f)
            {
                route.points.push_back(route.points.front());
                route.node.push_back(route.node.front());
                route.offset.push_back(route.offset.front());
                route.signalNode.push_back(route.signalNode.front());
            }

            // The legs, which are what put the bus on the same segments the cars are on. A leg
            // whose two nodes are not joined by a road is a hole the router should never produce;
            // it is recorded as one rather than guessed at, and a bus on it simply does not appear
            // in the traffic model for that stretch.
            route.legSegment.assign(route.points.size(), 0xFFFFFFFFu);
            route.legForward.assign(route.points.size(), 1u);
            for (std::size_t p = 0; p + 1 < route.points.size(); ++p)
            {
                const std::uint32_t leg = roads.FindSegmentBetween(route.node[p], route.node[p + 1]);
                route.legSegment[p] = leg;
                if (leg != 0xFFFFFFFFu)
                    route.legForward[p] =
                        roads.segments()[leg].nodeA == route.node[p] ? 1u : 0u;
            }
            route.distance.assign(route.points.size(), 0.0f);
            for (std::size_t p = 1; p < route.points.size(); ++p)
                route.distance[p] = route.distance[p - 1] +
                                    Distance(route.points[p - 1], route.points[p]);
            route.length = route.distance.back();
            if (route.length < 600.0f) continue;

            // Where each stop sits along the driving line. Found by proximity rather than
            // remembered from the walk above, because the walk emitted a variable number of road
            // nodes per leg and a stop is wherever its junction ended up on the line.
            route.stopDistance.assign(route.stops.size(), 0.0f);
            for (std::size_t i = 0; i < route.stops.size(); ++i)
            {
                const Vec2 at = stops_[route.stops[i]].position;
                float best = std::numeric_limits<float>::max();
                for (std::size_t p = 0; p < route.points.size(); ++p)
                {
                    const float d = DistanceSq(route.points[p], at);
                    if (d < best) { best = d; route.stopDistance[i] = route.distance[p]; }
                }
            }
            // The stops must be in increasing order round the loop or a bus will drive past one
            // and then reverse its target. Anything out of order is dropped.
            std::vector<std::uint32_t> ordered;
            std::vector<float> orderedDistance;
            float last = -1.0f;
            for (std::size_t i = 0; i < route.stops.size(); ++i)
                if (route.stopDistance[i] > last + 40.0f)
                {
                    ordered.push_back(route.stops[i]);
                    orderedDistance.push_back(route.stopDistance[i]);
                    last = route.stopDistance[i];
                }
            if (ordered.size() < 4) continue;
            route.stops = ordered;
            route.stopDistance = orderedDistance;

            // Set back from the junction, which is where a bus stop belongs and where the geometry
            // works. A stop is laid out on a road node, and a route turns at a node: sampling the
            // direction there gives the bisector of the turn, so offsetting into the nearside lane
            // throws the bus outside the corner by the offset times root two -- which downtown put
            // a twelve-metre bus on the grass beside the crossing. Every stop on the route moves
            // back by the same distance, so their order round the loop is unchanged.
            for (float& d : route.stopDistance)
            {
                d -= 18.0f;
                if (d < 0.0f) d += route.length;
            }

            const auto routeIndex = static_cast<std::uint32_t>(routes_.size());
            for (std::uint32_t i = 0; i < route.stops.size(); ++i)
            {
                stops_[route.stops[i]].routes.emplace_back(routeIndex, i);
                ++served[route.stops[i]];
            }
            routes_.push_back(std::move(route));
        }

        if (routes_.empty()) { stops_.clear(); return; }

        // A stop's kerb is where the bus pulls up, which is the nearside of the road it is on --
        // and a passenger stands a further step back, on the pavement, not in the gutter.
        for (BusStop& stop : stops_)
        {
            if (stop.routes.empty()) continue;
            const auto& [route, index] = stop.routes.front();
            const float along = routes_[route].stopDistance[index];
            // Taken from the route rather than from the node the stop was laid out on, because the
            // stop has just moved back from that node. A shelter that stays at the junction while
            // the bus pulls up eighteen metres short of it is two pieces of furniture for one
            // stop.
            const Vec2 centre = PointOnRoute(route, along);
            const Vec2 direction = DirectionOnRoute(route, along);
            const Vec2 nearside = Perp(direction) * -1.0f;
            stop.kerb = centre + nearside * OffsetOnRoute(route, along);
            stop.position = centre + nearside * (OffsetOnRoute(route, along) + 3.2f);
        }

        // Drop the stops nothing calls at. A shelter with no service is worse than no shelter:
        // passengers would queue at it forever.
        std::vector<std::uint32_t> remap(stops_.size(), kNoStop);
        std::vector<BusStop> live;
        for (std::uint32_t s = 0; s < stops_.size(); ++s)
            if (!stops_[s].routes.empty())
            {
                remap[s] = static_cast<std::uint32_t>(live.size());
                live.push_back(std::move(stops_[s]));
            }
        stops_ = std::move(live);
        for (BusRoute& route : routes_)
            for (std::uint32_t& s : route.stops) s = remap[s];

        if (std::getenv("CNA_CITY_BUS_REPORT") != nullptr)
        {
            std::printf("bus network: %zu stops of %zu candidates on %zu routes\n", stops_.size(),
                        candidates.size(), routes_.size());
            for (std::size_t r = 0; r < routes_.size(); ++r)
                std::printf("  route %2zu  %2zu stops  %6.0f m\n", r, routes_[r].stops.size(),
                            static_cast<double>(routes_[r].length));
        }

        // --- The fleet ----------------------------------------------------------------------
        //
        // Enough buses that one comes along inside the passengers' patience, spaced evenly round
        // the loop so a route does not start the day with all four of its buses nose to tail.
        for (std::uint32_t r = 0; r < routes_.size(); ++r)
        {
            const BusRoute& route = routes_[r];
            const int count = Clamp(static_cast<int>(route.length / 900.0f), 2, 7);
            for (int b = 0; b < count; ++b)
            {
                Bus bus;
                bus.route = r;
                // Staggered by route as well as within it. Routes overlap -- that is what makes a
                // network -- so starting every route's first bus at its own first stop puts three
                // of them on the same junction at once when three routes share a terminus.
                bus.position = std::fmod(route.length * (static_cast<float>(b) + 0.37f *
                                                          static_cast<float>(r)) /
                                             static_cast<float>(count),
                                         route.length);
                bus.appearance = static_cast<std::uint8_t>(rng.NextUInt(16));
                // Start each bus heading for the first stop ahead of it.
                bus.nextStop = 0;
                for (std::uint32_t i = 0; i < route.stopDistance.size(); ++i)
                    if (route.stopDistance[i] > bus.position) { bus.nextStop = i; break; }
                buses_.push_back(bus);
            }
        }
    }

    Vec2 BusNetwork::PointOnRoute(std::uint32_t route, float distance) const
    {
        const BusRoute& r = routes_[route];
        if (r.points.size() < 2) return r.points.empty() ? Vec2(0.0f, 0.0f) : r.points[0];
        float wrapped = std::fmod(distance, r.length);
        if (wrapped < 0.0f) wrapped += r.length;
        const auto it = std::upper_bound(r.distance.begin(), r.distance.end(), wrapped);
        const std::size_t index = static_cast<std::size_t>(
            Clamp(static_cast<int>(it - r.distance.begin()) - 1, 0,
                  static_cast<int>(r.points.size()) - 2));
        const float span = r.distance[index + 1] - r.distance[index];
        const float t = span > 1e-3f ? (wrapped - r.distance[index]) / span : 0.0f;
        return Lerp(r.points[index], r.points[index + 1], t);
    }

    Vec2 BusNetwork::DirectionOnRoute(std::uint32_t route, float distance) const
    {
        const Vec2 ahead = PointOnRoute(route, distance + 3.0f);
        const Vec2 behind = PointOnRoute(route, distance - 3.0f);
        const Vec2 delta = ahead - behind;
        return LengthSq(delta) > 1e-6f ? Normalized(delta) : Vec2(1.0f, 0.0f);
    }

    float BusNetwork::OffsetOnRoute(std::uint32_t route, float distance) const
    {
        const BusRoute& r = routes_[route];
        if (r.offset.size() < 2) return 4.0f;
        float wrapped = std::fmod(distance, r.length);
        if (wrapped < 0.0f) wrapped += r.length;
        const auto it = std::upper_bound(r.distance.begin(), r.distance.end(), wrapped);
        const std::size_t index = static_cast<std::size_t>(
            Clamp(static_cast<int>(it - r.distance.begin()) - 1, 0,
                  static_cast<int>(r.offset.size()) - 2));
        const float span = r.distance[index + 1] - r.distance[index];
        const float t = span > 1e-3f ? (wrapped - r.distance[index]) / span : 0.0f;
        return r.offset[index] + (r.offset[index + 1] - r.offset[index]) * t;
    }

    void BusNetwork::Placement(const Bus& bus, Vec2& outPosition, float& outHeading) const
    {
        // One search, not four. This is called for every bus every tick to build the occupancy
        // snapshot, and going through PointOnRoute, DirectionOnRoute and OffsetOnRoute separately
        // meant four binary searches over the same cold array for one answer -- which for ninety
        // buses was most of what the bus tick cost. The direction now comes from the located
        // segment rather than from sampling three metres either side, which is also more correct:
        // a road polyline's direction is piecewise constant, not smoothed.
        const BusRoute& r = routes_[bus.route];
        if (r.points.size() < 2)
        {
            outPosition = r.points.empty() ? Vec2(0.0f, 0.0f) : r.points[0];
            outHeading = 0.0f;
            return;
        }
        float wrapped = std::fmod(bus.position, r.length);
        if (wrapped < 0.0f) wrapped += r.length;
        const auto it = std::upper_bound(r.distance.begin(), r.distance.end(), wrapped);
        const std::size_t i = static_cast<std::size_t>(
            Clamp(static_cast<int>(it - r.distance.begin()) - 1, 0,
                  static_cast<int>(r.points.size()) - 2));
        const float span = r.distance[i + 1] - r.distance[i];
        const float t = span > 1e-3f ? (wrapped - r.distance[i]) / span : 0.0f;

        const Vec2 at = Lerp(r.points[i], r.points[i + 1], t);
        const Vec2 delta = r.points[i + 1] - r.points[i];
        const Vec2 direction = LengthSq(delta) > 1e-6f ? Normalized(delta) : Vec2(1.0f, 0.0f);
        const float lane = r.offset[i] + (r.offset[i + 1] - r.offset[i]) * t;
        // Driving on the left, like the rest of the city: `Perp` is the vehicle's own right, so
        // negating puts it on the nearside. This has to be the same expression Traffic::Placement
        // uses, sign for sign, or buses come down the oncoming carriageway.
        outPosition = at - Perp(direction) * lane;
        outHeading = Heading(direction);
    }

    bool BusNetwork::RoadPositionOf(const Bus& bus, std::uint32_t& outSegment,
                                    std::uint8_t& outForward, float& outS) const
    {
        const BusRoute& route = routes_[bus.route];
        if (route.points.size() < 2 || route.legSegment.empty()) return false;
        float wrapped = std::fmod(bus.position, route.length);
        if (wrapped < 0.0f) wrapped += route.length;
        const auto it = std::upper_bound(route.distance.begin(), route.distance.end(), wrapped);
        const std::size_t leg = static_cast<std::size_t>(
            Clamp(static_cast<int>(it - route.distance.begin()) - 1, 0,
                  static_cast<int>(route.points.size()) - 2));
        if (route.legSegment[leg] == 0xFFFFFFFFu) return false;
        outSegment = route.legSegment[leg];
        outForward = route.legForward[leg];
        outS = wrapped - route.distance[leg];
        return true;
    }

    namespace
    {
        /// The kerbside lane index a given lateral offset lands in. One expression, used for the
        /// lane a bus is in and for the lane it is about to be in -- they differ whenever a route
        /// turns from a collector onto an arterial, and asking about the wrong one is how the
        /// look-ahead over a junction came to be querying an empty lane.
        std::uint8_t LaneFromOffset(float offset)
        {
            return static_cast<std::uint8_t>(std::max(0, static_cast<int>(offset / 3.4f)));
        }
    }

    std::uint8_t BusNetwork::LaneOf(const Bus& bus) const
    {
        // The kerbside lane, which is where a bus belongs and which is also the lane its own
        // lateral offset already puts it in. Deriving it from the offset rather than storing a
        // second copy is what keeps the two from disagreeing.
        const BusRoute& route = routes_[bus.route];
        if (route.offset.empty()) return 0;
        return LaneFromOffset(OffsetOnRoute(bus.route, bus.position));
    }

    std::vector<RoadObstacle> BusNetwork::RoadOccupancy() const
    {
        std::vector<RoadObstacle> out;
        out.reserve(buses_.size());
        for (const Bus& bus : buses_)
        {
            RoadObstacle obstacle;
            if (!RoadPositionOf(bus, obstacle.segment, obstacle.forward, obstacle.s)) continue;
            obstacle.speed = bus.speed;
            obstacle.length = ProfileOf(VehicleKind::Bus).length;
            obstacle.lane = LaneOf(bus);
            out.push_back(obstacle);
        }
        return out;
    }

    std::uint32_t BusNetwork::NearestStop(Vec2 point) const
    {
        std::uint32_t best = kNoStop;
        float bestDist = std::numeric_limits<float>::max();
        for (std::uint32_t s = 0; s < stops_.size(); ++s)
        {
            const float d = DistanceSq(stops_[s].position, point);
            if (d < bestDist) { bestDist = d; best = s; }
        }
        return best;
    }

    std::uint32_t BusNetwork::RouteBetween(std::uint32_t fromStop, std::uint32_t toStop) const
    {
        if (fromStop >= stops_.size() || toStop >= stops_.size()) return 0xFFFFFFFFu;
        for (const auto& [route, index] : stops_[fromStop].routes)
        {
            (void)index;
            const BusRoute& r = routes_[route];
            if (std::find(r.stops.begin(), r.stops.end(), toStop) != r.stops.end()) return route;
        }
        return 0xFFFFFFFFu;
    }

    bool BusNetwork::PlanRide(Vec2 from, Vec2 to, Ride& ride) const
    {
        if (stops_.size() < 2) return false;

        // Every stop within walking distance, not just the nearest one. Taking only the nearest
        // was the first version and it threw away most of the network: the stop outside your door
        // is on one route, that route does not go where you are going, and the answer came back
        // "no bus" even though the stop across the junction runs a service that does.
        std::uint32_t nearby[6];
        float nearbyWalk[6];
        int nearbyCount = 0;
        for (std::uint32_t s = 0; s < stops_.size(); ++s)
        {
            const float walk = Distance(stops_[s].position, from);
            if (walk > 500.0f) continue;
            int slot = nearbyCount;
            if (nearbyCount < 6) ++nearbyCount;
            else
            {
                slot = -1;
                float worst = walk;
                for (int k = 0; k < 6; ++k)
                    if (nearbyWalk[k] > worst) { worst = nearbyWalk[k]; slot = k; }
                if (slot < 0) continue;
            }
            nearby[slot] = s;
            nearbyWalk[slot] = walk;
        }
        if (nearbyCount == 0) return false;

        std::uint32_t board = kNoStop;
        std::uint32_t alight = kNoStop;
        std::uint32_t bestRoute = 0;
        float bestCost = std::numeric_limits<float>::max();
        float bestWalk = 0.0f;
        float rideDistance = 0.0f;
        for (int n = 0; n < nearbyCount; ++n)
        {
            const std::uint32_t candidate = nearby[n];
            for (const auto& [route, index] : stops_[candidate].routes)
            {
                const BusRoute& r = routes_[route];
                for (std::uint32_t i = 0; i < r.stops.size(); ++i)
                {
                    const std::uint32_t other = r.stops[i];
                    if (other == candidate) continue;
                    const float walk = Distance(stops_[other].position, to);
                    // Round the loop in the one direction buses run.
                    float along = r.stopDistance[i] - r.stopDistance[index];
                    if (along < 0.0f) along += r.length;
                    // Rank on time rather than on the far walk alone: nine metres a second on the
                    // bus against one and a third on foot, plus a stop-by-stop penalty for the
                    // dwells, which is what stops a passenger riding twelve stops to save one.
                    const float cost = (nearbyWalk[n] + walk) / 1.35f + along / 9.0f;
                    if (cost >= bestCost) continue;
                    bestCost = cost;
                    board = candidate;
                    alight = other;
                    bestRoute = route;
                    bestWalk = walk;
                    rideDistance = along;
                }
            }
        }
        if (alight == kNoStop || board == kNoStop) return false;
        // The ride has to save more than it costs. Walking to a stop, waiting, riding round three
        // sides of a loop and walking again is slower than walking, and a passenger who cannot see
        // that will do it every time.
        //
        // The first gate is the one that matters and it is the same arithmetic the ranking above
        // uses: the whole trip by bus, in seconds, against the whole trip on foot. Comparing
        // *distances* -- which is what this did at first -- accepts trips that are shorter through
        // the air and slower to make.
        const float direct = Distance(from, to);
        if (bestCost > direct / 1.35f) return false;
        const float viaBus = Distance(from, stops_[board].position) + bestWalk;
        // A bus is worth a longer walk than the ride saves in distance, because it is nine times
        // walking pace: two hundred metres of extra pavement costs two and a half minutes and a
        // kilometre of riding saves ten. What it is not worth is a walk that is most of the trip.
        if (viaBus > direct * 1.05f + 120.0f) return false;
        if (rideDistance < 300.0f || rideDistance > direct * 3.0f + 600.0f) return false;

        ride.boardStop = board;
        ride.alightStop = alight;
        ride.route = bestRoute;
        ride.rideDistance = rideDistance;
        return true;
    }

    void BusNetwork::Step(const City& city, float dt,
                          const std::function<bool(Vec2, Vec2)>& mayProceed,
                          const std::function<float(std::uint32_t, std::uint8_t, std::uint8_t,
                                                    float, std::uint32_t, float*)>& gapAhead)
    {
        // Where the buses that are standing at a stop are, so the bay check below scans a handful
        // rather than the whole fleet. Everything else a bus needs to know about what is in front
        // of it now comes out of the road's own occupancy structure, which counts cars too -- the
        // all-pairs scan this replaced could only ever see other buses.
        dwelling_.clear();
        dwellingAt_.clear();
        for (std::size_t b = 0; b < buses_.size(); ++b)
            if (buses_[b].dwellRemaining > 0.0f)
            {
                Vec2 at(0.0f, 0.0f);
                float heading = 0.0f;
                Placement(buses_[b], at, heading);
                dwelling_.push_back(static_cast<std::uint32_t>(b));
                dwellingAt_.push_back(at);
            }

        for (std::size_t index = 0; index < buses_.size(); ++index)
        {
            Bus& bus = buses_[index];
            const BusRoute& route = routes_[bus.route];
            if (route.stops.size() < 2) continue;

            if (bus.dwellRemaining > 0.0f)
            {
                bus.dwellRemaining -= dt;
                bus.speed = 0.0f;
                continue;
            }

            // Held at a red. The signal is read from the traffic model rather than modelled again
            // here, so a bus stops at the same lights the cars do. The hold has a ceiling for the
            // same reason the vehicles' does: a bus that waits forever at a signal it has misread
            // takes its whole route's service with it.
            //
            // The signal is only asked about when there is one to ask about. Every bus used to put
            // the question every tick, and answering it starts with a nearest-node search over the
            // road graph -- which for the ninety-odd buses in the city was a third of the entire
            // simulation tick, spent almost every time to be told there is no junction here.
            std::uint32_t signalAhead = 0xFFFFFFFFu;
            {
                const auto it = std::upper_bound(route.distance.begin(), route.distance.end(),
                                                 bus.position);
                const std::size_t next = static_cast<std::size_t>(it - route.distance.begin());
                if (next < route.signalNode.size() && route.distance[next] - bus.position < 14.0f)
                    signalAhead = route.signalNode[next];
            }
            if (signalAhead != 0xFFFFFFFFu && bus.redLightSeconds < 45.0f &&
                !mayProceed(PointOnRoute(bus.route, bus.position),
                            PointOnRoute(bus.route, bus.position + 11.0f)))
            {
                bus.redLightSeconds += dt;
                bus.speed = std::max(0.0f, bus.speed - kAcceleration * 2.2f * dt);
                bus.position += bus.speed * dt;
                continue;
            }
            bus.redLightSeconds = 0.0f;

            float target = route.stopDistance[bus.nextStop] - bus.position;
            if (target < 0.0f) target += route.length;

            const float brakingDistance = (bus.speed * bus.speed) / (2.0f * kAcceleration);
            if (target <= brakingDistance + 0.5f)
                bus.speed = std::max(1.0f, bus.speed - kAcceleration * dt);
            else
                bus.speed = std::min(bus.speed + kAcceleration * dt, kCruiseSpeed);

            // What is actually in front, from the road rather than from a list of buses: a car, a
            // lorry, another bus, or a bus standing at the stop this one is approaching. Before
            // this, a bus could not see a car at all -- it drove through the queue it should have
            // been at the back of, and the queue drove through it.
            std::uint32_t segment = 0;
            std::uint8_t forward = 1;
            float along = 0.0f;
            if (gapAhead && RoadPositionOf(bus, segment, forward, along))
            {
                const std::uint8_t lane = LaneOf(bus);
                float leaderSpeed = 0.0f;
                float gap = gapAhead(segment, forward, lane, along,
                                     static_cast<std::uint32_t>(index), &leaderSpeed);

                // And over the junction. The lane buckets are per segment, so a bus at the end of
                // one cannot see the queue that starts at the beginning of the next -- it drove
                // into the back of it every time, which is the same blind spot the cars have and
                // which the junction pass handles for them. Looking one leg ahead when the end is
                // close costs one more query and closes it.
                const float toEnd = city.roads().segments()[segment].length - along;
                if (toEnd < kFollowingGap + 20.0f)
                {
                    const BusRoute& r = routes_[bus.route];
                    const auto it = std::upper_bound(r.distance.begin(), r.distance.end(),
                                                     std::fmod(bus.position, r.length));
                    const std::size_t next = static_cast<std::size_t>(it - r.distance.begin());
                    if (next < r.legSegment.size() && r.legSegment[next] != 0xFFFFFFFFu)
                    {
                        float overSpeed = 0.0f;
                        const float over =
                            gapAhead(r.legSegment[next], r.legForward[next],
                                     LaneFromOffset(r.offset[next]), 0.0f, 0xFFFFFFFFu, &overSpeed);
                        if (toEnd + over < gap)
                        {
                            gap = toEnd + over;
                            leaderSpeed = overSpeed;
                        }
                    }
                }
                if (gap < kFollowingGap + 30.0f)
                {
                    // Stopped at a bus length and a half, creeping in above that, free beyond it,
                    // and never far enough in one step to close the gap anyway -- a capped speed
                    // is not a guarantee when the step is long.
                    const float allowed =
                        std::max(0.0f, (gap - kFollowingGap) / 30.0f) * kCruiseSpeed;
                    const float reachable = std::max(0.0f, gap - kFollowingGap) / std::max(dt, 1e-3f);
                    bus.speed = std::min({bus.speed, std::max(allowed, leaderSpeed), reachable});
                }
            }

            bus.position += bus.speed * dt;
            if (bus.position >= route.length) bus.position -= route.length;

            float remaining = route.stopDistance[bus.nextStop] - bus.position;
            if (remaining < -route.length * 0.5f) remaining += route.length;
            if (remaining <= 1.2f && remaining > -20.0f)
            {
                // Is the bay free? Two routes calling at the same shelter stop within a few metres
                // of each other, and taking the stop snaps the bus onto its exact stopping point --
                // so without this a bus arriving at an occupied stop teleports the last metre into
                // the one already standing there. It holds instead and takes the stop when the bay
                // clears, which is also what happens on a real street.
                Vec2 here(0.0f, 0.0f);
                float heading = 0.0f;
                Placement(bus, here, heading);
                bool bayTaken = false;
                for (std::size_t d = 0; d < dwelling_.size() && !bayTaken; ++d)
                    bayTaken = dwelling_[d] != index &&
                               DistanceSq(dwellingAt_[d], here) < kFollowingGap * kFollowingGap;
                if (bayTaken)
                {
                    bus.speed = 0.0f;
                    continue;
                }

                bus.position = route.stopDistance[bus.nextStop];
                bus.speed = 0.0f;
                bus.dwellRemaining = kDwellSeconds;
                bus.nextStop = (bus.nextStop + 1) % static_cast<std::uint32_t>(route.stops.size());
            }
        }
    }

    std::size_t BusNetwork::MemoryBytes() const
    {
        std::size_t bytes = stops_.capacity() * sizeof(BusStop) +
                            routes_.capacity() * sizeof(BusRoute) +
                            buses_.capacity() * sizeof(Bus);
        for (const BusStop& stop : stops_)
            bytes += stop.name.capacity() + stop.routes.capacity() * sizeof(stop.routes[0]);
        for (const BusRoute& route : routes_)
            bytes += route.signalNode.capacity() * sizeof(std::uint32_t) +
                     route.node.capacity() * sizeof(std::uint32_t) +
                     route.legSegment.capacity() * sizeof(std::uint32_t) +
                     route.legForward.capacity() * sizeof(std::uint8_t) +
                     route.stops.capacity() * sizeof(std::uint32_t) +
                     route.stopDistance.capacity() * sizeof(float) +
                     route.points.capacity() * sizeof(Vec2) +
                     route.distance.capacity() * sizeof(float);
        return bytes;
    }
}
