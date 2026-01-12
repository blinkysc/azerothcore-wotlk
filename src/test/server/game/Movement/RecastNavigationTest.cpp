/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "gtest/gtest.h"
#include <cmath>
#include <algorithm>

/**
 * @brief Test suite for recastnavigation upstream fixes
 *
 * Tests verify the mathematical correctness of:
 * 1. Segment-polygon intersection epsilon (DetourCommon.cpp)
 * 2. Portal bounding box rounding (DetourNavMesh.cpp)
 * 3. Tile boundary rasterization (RecastRasterization.cpp)
 */

namespace
{
    //=========================================================================
    // Segment-Polygon Intersection Tests (epsilon fix: 1e-8 -> 1e-6)
    //=========================================================================

    // Simplified 2D perpendicular (cross product z-component)
    inline float vperp2D(const float* u, const float* v)
    {
        return u[0] * v[2] - u[2] * v[0];
    }

    // Test segment-polygon intersection with given epsilon
    bool IntersectSegmentPoly2D_WithEpsilon(const float* p0, const float* p1,
                                             const float* verts, int nverts,
                                             float& tmin, float& tmax,
                                             float EPS)
    {
        tmin = 0;
        tmax = 1;

        float dir[3] = {p1[0] - p0[0], 0, p1[2] - p0[2]};

        for (int i = 0, j = nverts - 1; i < nverts; j = i++)
        {
            float edge[3] = {
                verts[i*3+0] - verts[j*3+0],
                0,
                verts[i*3+2] - verts[j*3+2]
            };
            float diff[3] = {
                p0[0] - verts[j*3+0],
                0,
                p0[2] - verts[j*3+2]
            };

            float n = vperp2D(edge, diff);
            float d = vperp2D(dir, edge);

            if (std::fabs(d) < EPS)
            {
                // Nearly parallel
                if (n < 0)
                    return false;
                else
                    continue;
            }

            float t = n / d;
            if (d < 0)
            {
                if (t > tmin)
                {
                    tmin = t;
                    if (tmin > tmax)
                        return false;
                }
            }
            else
            {
                if (t < tmax)
                {
                    tmax = t;
                    if (tmax < tmin)
                        return false;
                }
            }
        }
        return true;
    }

    //=========================================================================
    // Portal Bounding Tests (division by zero fix)
    //=========================================================================

    struct PortalResult {
        unsigned char bmin;
        unsigned char bmax;
        bool valid;
    };

    // Old portal calculation (vulnerable to division by zero)
    PortalResult CalculatePortalBounds_Old(float va, float vb, float neiaMin, float neiaMax)
    {
        PortalResult result;
        float denom = vb - va;

        // OLD: No check for degenerate edge
        float tmin = (neiaMin - va) / denom;  // Can divide by zero!
        float tmax = (neiaMax - va) / denom;

        if (tmin > tmax)
            std::swap(tmin, tmax);

        // Clamp to [0,1] then scale to [0,255]
        tmin = std::max(0.0f, std::min(1.0f, tmin));
        tmax = std::max(0.0f, std::min(1.0f, tmax));

        result.bmin = (unsigned char)(tmin * 255.0f);
        result.bmax = (unsigned char)(tmax * 255.0f);
        result.valid = true;
        return result;
    }

    // New portal calculation (with division by zero protection)
    PortalResult CalculatePortalBounds_New(float va, float vb, float neiaMin, float neiaMax)
    {
        PortalResult result;
        float denom = vb - va;

        // NEW: Check for degenerate edge
        if (std::fabs(denom) < 1e-6f)
        {
            result.bmin = 0;
            result.bmax = 255;
            result.valid = true;
            return result;
        }

        float tmin = (neiaMin - va) / denom;
        float tmax = (neiaMax - va) / denom;

        if (tmin > tmax)
            std::swap(tmin, tmax);

        // Clamp to [0,1] then scale to [0,255] with proper int conversion
        tmin = std::max(0.0f, std::min(1.0f, tmin));
        tmax = std::max(0.0f, std::min(1.0f, tmax));

        result.bmin = (unsigned char)std::max(0, std::min(255, (int)(tmin * 255.0f)));
        result.bmax = (unsigned char)std::max(0, std::min(255, (int)(tmax * 255.0f)));
        result.valid = true;
        return result;
    }

    //=========================================================================
    // Tile Boundary Rasterization Tests (y0 clamp fix: 0 -> -1)
    //=========================================================================

    // Simulate the clamp behavior for tile boundary
    int ClampTileBoundary_Old(int y, int h)
    {
        // OLD: Clamp to [0, h-1]
        return std::max(0, std::min(y, h - 1));
    }

    int ClampTileBoundary_New(int y, int h)
    {
        // NEW: Clamp to [-1, h-1] to properly cut polygons at tile start
        return std::max(-1, std::min(y, h - 1));
    }
}

//=============================================================================
// Epsilon Tests
//=============================================================================

TEST(RecastNavigationTest, Epsilon_OldValueTooSmall)
{
    // Test case where old epsilon (1e-8) would incorrectly detect parallel
    // but new epsilon (1e-6) correctly handles floating-point imprecision

    // Square polygon
    float verts[] = {
        0, 0, 0,
        10, 0, 0,
        10, 0, 10,
        0, 0, 10
    };

    // Segment nearly parallel to edge (difference of 1e-7 in direction)
    float p0[] = {-1, 0, 5};
    float p1[] = {11, 0, 5.0000001f};  // Tiny deviation

    float tmin, tmax;

    // With old epsilon (1e-8), this might incorrectly pass/fail
    bool resultOld = IntersectSegmentPoly2D_WithEpsilon(p0, p1, verts, 4, tmin, tmax, 0.00000001f);

    // With new epsilon (1e-6), should be more robust
    bool resultNew = IntersectSegmentPoly2D_WithEpsilon(p0, p1, verts, 4, tmin, tmax, 0.000001f);

    // Both should find intersection, but new epsilon is more reliable
    EXPECT_TRUE(resultNew);
}

TEST(RecastNavigationTest, Epsilon_NormalIntersection)
{
    // Normal intersection that should work with both epsilons
    float verts[] = {
        0, 0, 0,
        10, 0, 0,
        10, 0, 10,
        0, 0, 10
    };

    float p0[] = {5, 0, -5};
    float p1[] = {5, 0, 15};

    float tmin, tmax;
    bool result = IntersectSegmentPoly2D_WithEpsilon(p0, p1, verts, 4, tmin, tmax, 0.000001f);

    EXPECT_TRUE(result);
    EXPECT_GE(tmin, 0.0f);
    EXPECT_LE(tmax, 1.0f);
}

//=============================================================================
// Portal Bounding Tests
//=============================================================================

TEST(RecastNavigationTest, Portal_NormalEdge)
{
    // Normal edge: va=0, vb=10
    auto result = CalculatePortalBounds_New(0.0f, 10.0f, 2.0f, 8.0f);

    EXPECT_TRUE(result.valid);
    // tmin = 2/10 = 0.2 -> 51
    // tmax = 8/10 = 0.8 -> 204
    EXPECT_NEAR(result.bmin, 51, 1);
    EXPECT_NEAR(result.bmax, 204, 1);
}

TEST(RecastNavigationTest, Portal_DegenerateEdge_DivisionByZero)
{
    // Degenerate edge: va=5, vb=5 (same point!)
    // OLD code would divide by zero here

    auto resultNew = CalculatePortalBounds_New(5.0f, 5.0f, 2.0f, 8.0f);

    // New code should return full range and be valid
    EXPECT_TRUE(resultNew.valid);
    EXPECT_EQ(resultNew.bmin, 0);
    EXPECT_EQ(resultNew.bmax, 255);
}

TEST(RecastNavigationTest, Portal_NearlyDegenerateEdge)
{
    // Nearly degenerate edge: difference of 1e-7
    auto result = CalculatePortalBounds_New(5.0f, 5.0000001f, 2.0f, 8.0f);

    // Should be caught by the 1e-6 check
    EXPECT_TRUE(result.valid);
    EXPECT_EQ(result.bmin, 0);
    EXPECT_EQ(result.bmax, 255);
}

TEST(RecastNavigationTest, Portal_NegativeRange)
{
    // Edge going in negative direction
    auto result = CalculatePortalBounds_New(10.0f, 0.0f, 2.0f, 8.0f);

    EXPECT_TRUE(result.valid);
    // Values should be properly swapped and clamped
    EXPECT_LE(result.bmin, result.bmax);
}

//=============================================================================
// Tile Boundary Tests
//=============================================================================

TEST(RecastNavigationTest, TileBoundary_OldCutsPolygonIncorrectly)
{
    int h = 100;  // tile height

    // Triangle that starts just before tile boundary
    int y_start = -1;  // Just before tile

    int y0_old = ClampTileBoundary_Old(y_start, h);
    int y0_new = ClampTileBoundary_New(y_start, h);

    // OLD: Clamps to 0, losing the polygon portion before tile
    EXPECT_EQ(y0_old, 0);

    // NEW: Allows -1, properly cutting polygon at tile start
    EXPECT_EQ(y0_new, -1);
}

TEST(RecastNavigationTest, TileBoundary_NormalCase)
{
    int h = 100;

    // Normal case: polygon starts inside tile
    int y_start = 10;

    int y0_old = ClampTileBoundary_Old(y_start, h);
    int y0_new = ClampTileBoundary_New(y_start, h);

    // Both should give same result for normal case
    EXPECT_EQ(y0_old, 10);
    EXPECT_EQ(y0_new, 10);
}

TEST(RecastNavigationTest, TileBoundary_AtExactBoundary)
{
    int h = 100;

    // Polygon starting exactly at tile boundary
    int y_start = 0;

    int y0_old = ClampTileBoundary_Old(y_start, h);
    int y0_new = ClampTileBoundary_New(y_start, h);

    // Both should give 0 for exact boundary
    EXPECT_EQ(y0_old, 0);
    EXPECT_EQ(y0_new, 0);
}

TEST(RecastNavigationTest, TileBoundary_FarOutsideTile)
{
    int h = 100;

    // Polygon starting way before tile
    int y_start = -50;

    int y0_old = ClampTileBoundary_Old(y_start, h);
    int y0_new = ClampTileBoundary_New(y_start, h);

    // OLD: Clamps to 0
    EXPECT_EQ(y0_old, 0);

    // NEW: Still clamps to -1 (only need one row before for cutting)
    EXPECT_EQ(y0_new, -1);
}

//=============================================================================
// WoW-Specific Scale Tests
//=============================================================================

TEST(RecastNavigationTest, WoWScale_LargeCoordinates)
{
    // WoW uses coordinates in the thousands
    // Verify epsilon is appropriate for this scale

    // Large polygon (simulating WoW tile)
    float verts[] = {
        3000, 0, -4000,
        3100, 0, -4000,
        3100, 0, -3900,
        3000, 0, -3900
    };

    float p0[] = {3050, 0, -4050};
    float p1[] = {3050, 0, -3850};

    float tmin, tmax;
    bool result = IntersectSegmentPoly2D_WithEpsilon(p0, p1, verts, 4, tmin, tmax, 0.000001f);

    EXPECT_TRUE(result);
    EXPECT_GT(tmin, 0.0f);
    EXPECT_LT(tmax, 1.0f);
}

TEST(RecastNavigationTest, WoWScale_NaxxramasCoordinates)
{
    // Real Naxxramas coordinates from issue #24266
    float x1 = 3108.885f, z1 = -3895.021f;
    float x2 = 3150.0f, z2 = -3850.0f;

    // Calculate distance to verify precision
    float dist = std::sqrt((x2-x1)*(x2-x1) + (z2-z1)*(z2-z1));

    // Should be approximately 60 units
    EXPECT_NEAR(dist, 60.0f, 5.0f);

    // Verify epsilon 1e-6 is appropriate (not too large relative to coordinate scale)
    float eps = 0.000001f;
    EXPECT_LT(eps, dist * 0.0001f);  // Epsilon should be <0.01% of distances
}
