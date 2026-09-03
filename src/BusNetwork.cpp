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
                route.offset.push_back(route.offset.front());
                route.signalNode.push_back(route.signalNode.front());
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
                          const std::function<bool(Vec2, Vec2)>& mayProceed)
    {
        (void)city;

        // Where every bus is, before any of them moves. Buses are not in the car-following stream
        // -- see the class comment -- so nothing else stops two of them occupying the same twelve
        // metres of road, and two routes sharing an arterial did exactly that: a red bus driving
        // through a green one, in the middle of the shot. This is not car-following, it is one
        // rule -- do not drive into the bus in front -- and at ninety-four buses the all-pairs
        // check costs less than the route lookup that follows it.
        occupancy_.resize(buses_.size());
        dwelling_.clear();
        for (std::size_t b = 0; b < buses_.size(); ++b)
        {
            Placement(buses_[b], occupancy_[b].position, occupancy_[b].heading);
            occupancy_[b].direction = FromHeading(occupancy_[b].heading);
            if (buses_[b].dwellRemaining > 0.0f)
                dwelling_.push_back(static_cast<std::uint32_t>(b));
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
            // here, so a bus stops at the same lights the cars do -- which is most of what makes
            // one look like part of the traffic despite not being in the car-following stream.
            // The hold has a ceiling for the same reason the vehicles' does: a bus that waits
            // forever at a signal it has misread takes its whole route's service with it.
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
                if (next < route.signalNode.size() &&
                    route.distance[next] - bus.position < 14.0f)
                    signalAhead = route.signalNode[next];
            }
            const Vec2 at = signalAhead != 0xFFFFFFFFu ? PointOnRoute(bus.route, bus.position)
                                                       : Vec2(0.0f, 0.0f);
            const Vec2 ahead = signalAhead != 0xFFFFFFFFu
                                   ? PointOnRoute(bus.route, bus.position + 11.0f)
                                   : Vec2(0.0f, 0.0f);
            if (signalAhead != 0xFFFFFFFFu && bus.redLightSeconds < 45.0f && !mayProceed(at, ahead))
            {
                bus.redLightSeconds += dt;
                bus.speed = std::max(0.0f, bus.speed - kAcceleration * 2.2f * dt);
                bus.position += bus.speed * dt;
                continue;
            }
            bus.redLightSeconds = 0.0f;

            // The gap to the bus in front, if there is one -- ahead of this one rather than
            // beside or behind it, and travelling the same way, because two buses passing on
            // opposite carriageways are three metres apart and that is correct.
            //
            // This started as a boolean at eighteen metres and that was not enough: braking from
            // twelve and a half metres a second takes four seconds and covers twenty-seven, so the
            // bus in front was reached and driven through long before the brake took effect. It
            // is still not car-following -- there is no desired headway and no smooth approach --
            // but the stopping distance now comes out of the gap rather than out of a constant.
            float gapAhead = 1e9f;
            for (std::size_t other = 0; other < buses_.size(); ++other)
            {
                if (other == index) continue;
                const Vec2 delta = occupancy_[other].position - occupancy_[index].position;
                const float ahead = Dot(delta, occupancy_[index].direction);
                if (ahead > 60.0f) continue;
                if (std::abs(Dot(delta, Perp(occupancy_[index].direction))) > 4.0f) continue;
                if (Dot(occupancy_[other].direction, occupancy_[index].direction) < 0.5f) continue;
                // "In front of me" has to be antisymmetric or two buses each yield to the other
                // and both stop forever -- which is what the first attempt did: the tiebreak
                // below also matched a bus clearly *behind*, so a follower correctly held for its
                // leader while the leader held for its follower, nine metres apart, for the rest
                // of the day. Two on the same route are unambiguous -- if I am 9 m
                // behind you, you are 9 m in front of me -- but two on routes that meet at an
                // angle can each project the other forwards, and an exact overlap has no
                // direction at all. Both of those fall back to the index: the lower one has
                // priority and drives on, the higher one holds until it is clear.
                if (ahead < -0.5f) continue;   // behind me, and therefore their problem
                const float theirs = Dot(Vec2(-delta.X, -delta.Y), occupancy_[other].direction);
                if (ahead < 0.5f || theirs > 0.0f)
                {
                    if (other < index) gapAhead = std::min(gapAhead, std::max(0.0f, ahead));
                    continue;
                }
                gapAhead = std::min(gapAhead, ahead);
            }
            float target = route.stopDistance[bus.nextStop] - bus.position;
            if (target < 0.0f) target += route.length;

            const float brakingDistance = (bus.speed * bus.speed) / (2.0f * kAcceleration);
            if (target <= brakingDistance + 0.5f)
                bus.speed = std::max(1.0f, bus.speed - kAcceleration * dt);
            else
                bus.speed = std::min(kCruiseSpeed, bus.speed + kAcceleration * dt);

            // The gap is applied *after* the approach to the stop, and that ordering is the whole
            // of it: the first version capped the speed and then the stop logic below raised it
            // again on the next line -- `std::max(1.0f, ...)` while braking, and an outright
            // increase while cruising -- so the cap was written and immediately overwritten, and
            // four hundred bus pairs a run were still inside each other.
            // Approaching a stop somebody else is standing in: hold a bus length short of it
            // rather than up against it. The check below at the moment of arrival stops a bus
            // teleporting the last metre into an occupied bay, but by then it is already there --
            // and two twelve-metre buses ten metres apart still overlap.
            if (target < 45.0f)
            {
                const float along = route.stopDistance[bus.nextStop];
                const Vec2 stopPoint = PointOnRoute(bus.route, along) -
                                       Perp(DirectionOnRoute(bus.route, along)) *
                                           OffsetOnRoute(bus.route, along);
                for (std::uint32_t other : dwelling_)
                {
                    if (other == index) continue;
                    if (DistanceSq(occupancy_[other].position, stopPoint) >
                        kFollowingGap * kFollowingGap)
                        continue;
                    gapAhead = std::min(gapAhead, target);
                    break;
                }
            }

            if (gapAhead < kFollowingGap + 30.0f)
            {
                // Stopped at a bus length and a half, creeping in above that, and free beyond it.
                const float allowed =
                    std::max(0.0f, (gapAhead - kFollowingGap) / 30.0f) * kCruiseSpeed;
                // And never far enough in one step to close the gap anyway. A capped *speed* is
                // not a guarantee when the step is long: at a time scale of sixty a sub-step is
                // half a simulated second and a bus covers six metres in it, so a bus thirty
                // metres back can be nine metres back by the time the cap is next consulted.
                const float reachable = std::max(0.0f, gapAhead - kFollowingGap) /
                                        std::max(dt, 1e-3f);
                bus.speed = std::min({bus.speed, allowed, reachable});
            }

            bus.position += bus.speed * dt;
            if (bus.position >= route.length) bus.position -= route.length;

            float remaining = route.stopDistance[bus.nextStop] - bus.position;
            if (remaining < -route.length * 0.5f) remaining += route.length;
            if (remaining <= 1.2f && remaining > -20.0f)
            {
                // Is the bay free? Two routes calling at the same shelter stop within a few metres
                // of each other, and taking the stop snaps the bus onto its exact stopping point
                // -- so without this a bus arriving at an occupied stop teleports the last metre
                // into the one already standing there. It holds instead and takes the stop when
                // the bay clears, which is also what happens on a real street.
                bool bayTaken = false;
                for (std::uint32_t other : dwelling_)
                    bayTaken = bayTaken ||
                               (other != index &&
                                DistanceSq(occupancy_[other].position, occupancy_[index].position) <
                                    kFollowingGap * kFollowingGap);
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
                     route.stops.capacity() * sizeof(std::uint32_t) +
                     route.stopDistance.capacity() * sizeof(float) +
                     route.points.capacity() * sizeof(Vec2) +
                     route.distance.capacity() * sizeof(float);
        return bytes;
    }
}
