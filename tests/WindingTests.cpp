// SPDX-License-Identifier: MIT
//
// The face winding rule, which has cost this program three separate defects: every carriageway
// back-facing (a strip of grass down the middle of every street), every flat roof missing (which
// reads as a roof, because you see the pavement between the buildings), and four of the six faces
// of every metro tunnel inside out (the city visible through the wall of a train).
//
// None of the three announced itself and none was found by reading code. They are the reason
// MeshData::AddFacet exists, and this file is the reason it will keep working.

#include "TestSupport.hpp"

namespace CnaCityTests
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
    }

    // --- The primitive ---------------------------------------------------------------------

    TEST(Winding, AFacetIsAlwaysVisibleFromTheInteriorPointItWasGiven)
    {
        // Both circulations of the same quad, and the interior on either side of it. All four
        // combinations must produce a surface visible from the point the caller named -- that is
        // the whole contract, and the reason the caller no longer states a vertex order.
        const Vector3 corners[4] = {Vector3(-1.0f, 0.0f, -1.0f), Vector3(1.0f, 0.0f, -1.0f),
                                    Vector3(1.0f, 0.0f, 1.0f), Vector3(-1.0f, 0.0f, 1.0f)};
        for (const float side : {1.0f, -1.0f})
        {
            const Vector3 interior(0.0f, side * 5.0f, 0.0f);
            MeshData forward;
            forward.AddFacet(corners[0], corners[1], corners[2], corners[3], interior,
                             Vec2(0, 0), Vec2(1, 1));
            MeshData reversed;
            reversed.AddFacet(corners[3], corners[2], corners[1], corners[0], interior,
                              Vec2(0, 0), Vec2(1, 1));

            EXPECT_EQ(VisibleTriangleCount(forward, interior), TriangleCount(forward));
            EXPECT_EQ(VisibleTriangleCount(reversed, interior), TriangleCount(reversed));
            // And invisible from the other side, which is what makes it an enclosure rather than
            // a double-sided sheet.
            const Vector3 outside(0.0f, -side * 5.0f, 0.0f);
            EXPECT_EQ(VisibleTriangleCount(forward, outside), 0);
            EXPECT_EQ(VisibleTriangleCount(reversed, outside), 0);
        }
    }

    TEST(Winding, MirroringAFacetDoesNotInvertIt)
    {
        // The defect itself. Two walls either side of a tunnel are the same quad reflected, and
        // reflecting a correct vertex order produces an incorrect one -- so exactly one wall of
        // each pair was culled, which from inside is a tunnel with one side missing.
        const Vector3 interior(0.0f, 2.0f, 0.0f);
        for (const float side : {1.0f, -1.0f})
        {
            MeshData wall;
            wall.AddFacet(Vector3(-10.0f, 0.0f, side * 4.0f), Vector3(10.0f, 0.0f, side * 4.0f),
                          Vector3(10.0f, 4.0f, side * 4.0f), Vector3(-10.0f, 4.0f, side * 4.0f),
                          interior, Vec2(0, 0), Vec2(1, 1));
            EXPECT_EQ(VisibleTriangleCount(wall, interior), TriangleCount(wall))
                << "the wall at z = " << side * 4.0f << " is back-facing from inside";
        }
    }

    TEST(Winding, AFacetSurvivesAnObliqueMitredCrossSection)
    {
        // Tunnel walls are swept along a mitred polyline, so the quads are not axis-aligned and
        // not rectangular. The rule has to hold for a general quad or the bends leak.
        const Vector3 interior(0.0f, 2.0f, 0.0f);
        MeshData mesh;
        mesh.AddFacet(Vector3(-12.0f, 0.0f, 3.0f), Vector3(9.0f, 0.0f, 5.5f),
                      Vector3(9.0f, 4.2f, 5.5f), Vector3(-12.0f, 4.2f, 3.0f), interior,
                      Vec2(0, 0), Vec2(1, 1));
        EXPECT_EQ(VisibleTriangleCount(mesh, interior), TriangleCount(mesh));
    }

    // --- The primitives the city is actually built from ------------------------------------

    TEST(Winding, ABoxIsVisibleFromOutsideAndNotFromWithin)
    {
        // Buildings, vehicles, train cars. `includeTop` is the one that hid every flat roof.
        MeshData mesh;
        mesh.AddBox(Vec2(0.0f, 0.0f), 0.0f, Vec2(5.0f, 3.0f), 8.0f, 0.0f, Vec2(1, 1), Vec2(0, 0),
                    true, Vec2(1, 1), true);
        ASSERT_GT(TriangleCount(mesh), 0);

        // From inside the solid, every face must be culled.
        EXPECT_EQ(VisibleTriangleCount(mesh, Vector3(0.0f, 4.0f, 0.0f)), 0);

        // And from far enough away on each axis, exactly the faces turned towards the viewer.
        for (const Vector3& eye : {Vector3(400.0f, 4.0f, 0.0f), Vector3(-400.0f, 4.0f, 0.0f),
                                   Vector3(0.0f, 4.0f, 400.0f), Vector3(0.0f, 4.0f, -400.0f),
                                   Vector3(0.0f, 400.0f, 0.0f), Vector3(0.0f, -400.0f, 0.0f)})
        {
            const int visible = VisibleTriangleCount(mesh, eye);
            EXPECT_GT(visible, 0) << "no face of the box is visible from an eye outside it";
            EXPECT_LT(visible, TriangleCount(mesh)) << "every face visible at once is not a solid";
        }
    }

    TEST(Winding, ABoxRoofIsVisibleFromAbove)
    {
        // Named for the defect: from four hundred metres up, a city whose roofs are back-facing
        // looks like a city, because you see the pavement between the buildings through them. It
        // survived a dozen aerial screenshots and was found by tinting the roof material magenta.
        MeshData withTop;
        withTop.AddBox(Vec2(0, 0), 0.0f, Vec2(5.0f, 5.0f), 10.0f, 0.0f, Vec2(1, 1), Vec2(0, 0),
                       true, Vec2(1, 1), false);
        MeshData withoutTop;
        withoutTop.AddBox(Vec2(0, 0), 0.0f, Vec2(5.0f, 5.0f), 10.0f, 0.0f, Vec2(1, 1), Vec2(0, 0),
                          false, Vec2(1, 1), false);

        const Vector3 aboveIt(0.0f, 400.0f, 0.0f);
        EXPECT_GT(VisibleTriangleCount(withTop, aboveIt), VisibleTriangleCount(withoutTop, aboveIt))
            << "the roof is not drawn for a camera above the building";
    }

    TEST(Winding, ARibbonIsVisibleFromAboveAndNotFromBelow)
    {
        // Every carriageway, pavement and rail in the city is a ribbon. When these were wound the
        // other way the roads vanished and the ground plane showed through: a strip of grass down
        // the middle of every street, with the lamp posts correctly placed on either side of it.
        MeshData mesh;
        mesh.AddRibbon(Vec2(-50.0f, 0.0f), Vec2(50.0f, 0.0f), 4.0f, 0.0f, 0.0f, 10.0f, 0.0f, 1.0f);
        ASSERT_GT(TriangleCount(mesh), 0);
        EXPECT_EQ(VisibleTriangleCount(mesh, Vector3(0.0f, 50.0f, 0.0f)), TriangleCount(mesh));
        EXPECT_EQ(VisibleTriangleCount(mesh, Vector3(0.0f, -50.0f, 0.0f)), 0);
    }

    TEST(Winding, ARibbonHoldsAtEveryOrientation)
    {
        // A ribbon's winding must not depend on which way the road runs. It is the same expression
        // for all of them, but the same was true of the tunnel walls.
        for (int step = 0; step < 16; ++step)
        {
            const float angle = static_cast<float>(step) * (2.0f * kPi / 16.0f);
            const Vec2 direction(std::cos(angle), std::sin(angle));
            MeshData mesh;
            mesh.AddRibbon(direction * -40.0f, direction * 40.0f, 3.0f, 0.0f, 0.0f, 8.0f, 0.0f, 1.0f);
            EXPECT_EQ(VisibleTriangleCount(mesh, Vector3(0.0f, 60.0f, 0.0f)), TriangleCount(mesh))
                << "a road at " << angle << " rad is back-facing from above";
            EXPECT_EQ(VisibleTriangleCount(mesh, Vector3(0.0f, -60.0f, 0.0f)), 0);
        }
    }

    TEST(Winding, APitchedRoofIsVisibleFromAbove)
    {
        // Every house in the suburbs. This found a live defect: both slopes were wound inside out,
        // so the roofs were visible only from *below* -- which is where a street-level camera is,
        // relative to a two-storey house. The suburbs looked right in every eye-level screenshot
        // and were missing from every aerial one.
        MeshData mesh;
        mesh.AddPitchedRoof(Vec2(0, 0), 6.0f, Vec2(4.0f, 3.0f), 0.0f, 2.5f, 0.4f, Vec2(1, 1));
        ASSERT_EQ(TriangleCount(mesh), 6) << "two slopes and two gables";

        // The four slope triangles must all be drawn for a camera above the house, and none of
        // them for one inside it. The gables are vertical and exactly edge-on to a viewer directly
        // overhead, so they are checked from the side instead.
        EXPECT_EQ(VisibleTriangleCount(mesh, Vector3(0.0f, 400.0f, 0.0f)), 4)
            << "the roof slopes are back-facing from above";
        EXPECT_EQ(VisibleTriangleCount(mesh, Vector3(0.0f, 6.2f, 0.0f)), 0)
            << "a roof seen from inside the loft it covers should be culled entirely";
        // And a gable, from square in front of it.
        EXPECT_GT(VisibleTriangleCount(mesh, Vector3(-400.0f, 7.0f, 0.0f)), 0);
    }

    TEST(Winding, ACylinderIsVisibleFromOutside)
    {
        // Lamp columns, tree trunks, masts.
        MeshData mesh;
        mesh.AddCylinder(Vec2(0, 0), 0.0f, 0.5f, 9.0f, 8, Vec2(0, 0), Vec2(1, 1), true);
        ASSERT_GT(TriangleCount(mesh), 0);
        EXPECT_GT(VisibleTriangleCount(mesh, Vector3(200.0f, 4.0f, 0.0f)), 0);
        EXPECT_EQ(VisibleTriangleCount(mesh, Vector3(0.0f, 4.0f, 0.0f)), 0)
            << "a lamp column seen from inside itself should be entirely culled";
    }
}
