#ifndef NOISE_H
#define NOISE_H

#include "bounded_cell_cache.h"
#include "river_connection_guard.h"

#include <array>
#include <cstdint>
#include <vector>
#include <random>
#include <tuple>
#include <limits>
#include <cmath>
#include <cassert>
#include <memory>
#include <stdexcept>

#include "math2d.h"
#include "math3d.h"
#include "spline.h"
#include "utils.h"
#include "perlin.h"
#include "controlfunction.h"

template <typename I>
class Noise
{
public:
	Noise(std::unique_ptr<I> controlFunction,
		  const Point2D& noiseTopLeft,
		  const Point2D& noiseBottomRight,
		  const Point2D& controlFunctionTopLeft,
		  const Point2D& controlFunctionBottomRight,
		  int seed = 0,
		  double eps = 0.0,
		  int resolution = 1,
		  double displacement = 0.0,
		  int primitivesResolutionSteps = 3,
		  double noiseAmplitudeProportion = 0.0,
		  bool displayFunction = true,
		  bool displayPoints = false,
	      bool displaySegments = false,
	      bool displayGrid = false,
		  bool displayDistance = false);

	double evaluateTerrain(double x, double y) const;
    double evaluateTerrainCached(double x, double y) const;
    double evaluateTerrainUncached(double x, double y) const;
    double evaluateHierarchy(double x, double y, uint32_t level) const;
    std::array<uint64_t, 10> hierarchyEntries() const;
    uint64_t hierarchyBudgetUsed() const { return hierarchyBudget_.used(); }
    uint64_t hierarchyBudgetLimit() const { return hierarchyBudget_.limit(); }
    CellBudget& hierarchyAllocationBudget() const { return hierarchyBudget_; }
    template <typename Visitor>
    void visitHierarchySegments(double x, double y, uint32_t level, Visitor visitor) const;
    std::array<uint64_t, 4> cacheEntries() const;
    uint64_t cacheLogicalBytes() const;
    uint64_t cacheLogicalCapacityBytes() const;
    std::array<double, 2> controlRange() const
    {
        return {ControlFunctionMinimum(), ControlFunctionMaximum()};
    }
    template <typename Visitor>
    void visitSegments(double x, double y, Visitor visitor) const;
	double evaluateLichtenberg(double x, double y) const;

private:
	// ----- Types -----
	template <typename T, size_t N>
	using Array2D = std::array<std::array<T, N>, N>;

	template <size_t D>
	using Segment3DChain = std::array<Segment3D, D>;

	template <size_t N>
	using DoubleArray = Array2D<double, N>;

	template <size_t N>
	using Point2DArray = Array2D<Point2D, N>;

	template <size_t N, size_t D>
	using Segment3DChainArray = Array2D<Segment3DChain<D>, N>;

	// Random generator used by the class
	typedef std::mt19937_64 RandomGenerator;

	enum class ConnectionStrategy
	{
		Angle,
		AngleMid,
		NearestPoint,
		Rivers
	};

	/// <summary>
	/// Represents a cell at a specific resolution
	/// </summary>
	struct Cell
	{
		int x;
		int y;
		int resolution;

		Cell() : x(0), y(0), resolution(0) {}

		Cell(const int x, const int y, const int resolution) : x(x), y(y), resolution(resolution) {}
	};

    mutable CellBudget hierarchyBudget_{536870912};

    struct Graph1Memo final
    {
        Cell cell;
        Point2DArray<9> points;
        Segment3DChainArray<5, 4> segments;
    };
    struct Graph2Memo final
    {
        Cell cell;
        Point2DArray<5> points;
        Segment3DChainArray<5, 3> segments;
        std::shared_ptr<const Graph1Memo> base;
    };
    struct Graph3Memo final
    {
        Cell cell;
        Point2DArray<5> points;
        Segment3DChainArray<5, 2> segments;
        std::shared_ptr<const Graph1Memo> base;
        std::shared_ptr<const Graph2Memo> parent;
    };
    struct PrimitiveMemo final
    {
        Point2DArray<5> points;
        DoubleArray<5> preNoise;
        double amplitude;
        double radius;
        double wavelengthX;
        double wavelengthY;
    };

    std::shared_ptr<const Graph1Memo> memoGraph1(uint32_t x, uint32_t y) const;
    std::shared_ptr<const Graph2Memo> memoGraph2(uint32_t x, uint32_t y) const;
    std::shared_ptr<const Graph3Memo> memoGraph3(uint32_t x, uint32_t y) const;
    std::shared_ptr<const PrimitiveMemo> memoPrimitives(uint32_t x, uint32_t y) const;
    PrimitiveMemo buildPrimitiveMemo(uint32_t x, uint32_t y) const;
    double evaluateTerrain(const Point2D& point, const PrimitiveMemo& entry) const;
    mutable BoundedCellMemo<Graph1Memo, 32> graph1Memo_{hierarchyBudget_};
    mutable BoundedCellMemo<Graph2Memo, 64> graph2Memo_{hierarchyBudget_};
    mutable BoundedCellMemo<Graph3Memo, 128> graph3Memo_{hierarchyBudget_};
    mutable BoundedCellMemo<PrimitiveMemo, 1024> primitiveMemo_{hierarchyBudget_};

    struct Graph4Memo final
    {
        Cell cell;
        Point2DArray<5> points;
        Segment3DChainArray<5, 1> segments;
        std::shared_ptr<const Graph3Memo> parent;
    };
    std::shared_ptr<const Graph4Memo> memoGraph4(uint32_t x, uint32_t y) const;
    std::shared_ptr<const PrimitiveMemo> memoPrimitive4(uint32_t x, uint32_t y) const;
    mutable BoundedCellMemo<Graph4Memo, 256> graph4Memo_{hierarchyBudget_};
    mutable BoundedCellMemo<PrimitiveMemo, 2048> primitive4Memo_{hierarchyBudget_};
    struct Graph5Memo final
    {
        Cell cell;
        Point2DArray<5> points;
        Segment3DChainArray<5, 1> segments;
        std::shared_ptr<const Graph4Memo> parent;
    };
    std::shared_ptr<const Graph5Memo> memoGraph5(uint32_t x, uint32_t y) const;
    std::shared_ptr<const PrimitiveMemo> memoPrimitive5(uint32_t x, uint32_t y) const;
    mutable BoundedCellMemo<Graph5Memo, 512> graph5Memo_{hierarchyBudget_};
    mutable BoundedCellMemo<PrimitiveMemo, 4096> primitive5Memo_{hierarchyBudget_};
    struct Graph6Memo final
    {
        Cell cell;
        Point2DArray<5> points;
        Segment3DChainArray<5, 1> segments;
        std::shared_ptr<const Graph5Memo> parent;
    };
    std::shared_ptr<const Graph6Memo> memoGraph6(uint32_t x, uint32_t y) const;
    std::shared_ptr<const PrimitiveMemo> memoPrimitive6(uint32_t x, uint32_t y) const;
    mutable BoundedCellMemo<Graph6Memo, 1024> graph6Memo_{hierarchyBudget_};
    mutable BoundedCellMemo<PrimitiveMemo, 8192> primitive6Memo_{hierarchyBudget_};

	// ----- Points -----

	void InitPointCache();

	RandomGenerator InitRandomGenerator(int32_t i, int32_t j) const;

	Point2D GeneratePoint(int x, int y) const;
	Point2D GeneratePointCached(int x, int y) const;

	// ----- Utils -----

	Cell GetCell(double x, double y, int resolution) const;

	double EvaluateControlFunction(const Point2D& point) const;
	double EvaluateReliefSlope(const Point2D& point) const;

	bool InsideDomain(const Point2D& point) const;

	bool InsideDomain(const Segment2D& segment) const;
	
	bool InsideDomain(const Point3D& point) const;

	bool InsideDomain(const Segment3D& segment) const;

	bool DistToDomain(const Point2D& point) const;

	double ControlFunctionMinimum() const;
	
	double ControlFunctionMaximum() const;

	template <size_t D>
	Segment3DChain<D> ConnectPointToSegmentAngle(const Point3D& point, double segmentDist, const Segment3D& segment) const;

	template <size_t D>
	Segment3DChain<D> ConnectPointToSegmentAngleMid(const Point3D& point, double segmentDist, const Segment3D& segment) const;

	template <size_t D>
	Segment3DChain<D> ConnectPointToSegmentNearestPoint(const Point3D& point, double segmentDist, const Segment3D& segment) const;

	template <size_t D>
	Segment3DChain<D> ConnectPointToSegmentRivers(const Point3D& point, double segmentDist, const Segment3D& segment) const;

	template <size_t D>
	Segment3DChain<D> ConnectPointToSegment(const ConnectionStrategy& strategy, const Point3D& point, double segmentDist, const Segment3D& segment) const;

	template <typename T, size_t N>
	std::tuple<int, int> GetArrayCell(const Cell& arrCell, const Array2D<T, N>& arr, const Cell& cell) const;

	template <size_t N, size_t D>
	double NearestSegmentAndCellProjectionZ(int neighborhood, const Point2D& point, Cell& nearestSegmentCellOut, Segment3D& nearestSegmentOut, const Cell& cell, const Segment3DChainArray<N, D>& segments) const;

	template <size_t N, size_t D, typename ...Tail>
	double NearestSegmentAndCellProjectionZ(int neighborhood, const Point2D& point, Cell& nearestSegmentCellOut, Segment3D& nearestSegmentOut, const Cell& cell, const Segment3DChainArray<N, D>& segments, Tail&&... tail) const;

	template <size_t N, size_t D>
	double NearestSegmentProjectionZ(int neighborhood, const Point2D& point, Segment3D& nearestSegmentOut, const Cell& cell, const Segment3DChainArray<N, D>& segments) const;

	template <size_t N, size_t D, typename ...Tail>
	double NearestSegmentProjectionZ(int neighborhood, const Point2D& point, Segment3D& nearestSegmentOut, const Cell& cell, const Segment3DChainArray<N, D>& segments, Tail&&... tail) const;

	template <size_t N>
	int SegmentsEndingInP(const Cell& cell, const Segment3DChainArray<N, 1>& segments, const Point3D& point, Segment3D& lastSegmentEndingInP) const;

	template <size_t N>
	int SegmentsStartingInP(const Cell& cell, const Segment3DChainArray<N, 1>& segments, const Point3D& point, Segment3D& lastSegmentStartingInP) const;

	// ----- Generate -----

	template <size_t N>
	Point2DArray<N> GenerateNeighboringPoints(const Cell& cell) const;

	template <size_t N, size_t M>
	void ReplaceNeighboringPoints(const Cell& cell, const Point2DArray<M>& points, const Cell& subCell, Point2DArray<N>& subPoints) const;

	template <size_t N>
	DoubleArray<N> ComputeElevations(const Point2DArray<N>& points) const;
	
	template <size_t N>
	Segment3DChainArray<N - 2, 1> GenerateSegments(const Point2DArray<N>& points) const;

	template <size_t D>
	void SegmentChainFromPoints(const Point3D& start, const std::array<Point3D, D - 1>& midPoints, const Point3D& end, Segment3DChain<D>& outSegmentChain) const;

	template <size_t N, size_t D>
	void SubdivideSegments(const Cell& cell, const Segment3DChainArray<N, 1>& segments, Segment3DChainArray<N - 2, D>& subdividedSegments) const;
	
	template <size_t N, size_t D>
	void DisplaceSegments(double displacementFactor, const Cell& cell, Segment3DChainArray<N, D>& segments) const;

	template <size_t N2, size_t N1, size_t D1>
	void CheckEnoughSegmentInVicinity(const Point2DArray<N2>& points, const Cell& cell, const Segment3DChainArray<N1, D1>& segments) const;

	template <size_t N2, size_t N1, size_t D1, typename ...Tail>
	void CheckEnoughSegmentInVicinity(const Point2DArray<N2>& points, const Cell& cell, const Segment3DChainArray<N1, D1>& segments, Tail&&... tail) const;

	template <size_t N, size_t D, typename ...Tail>
	Segment3DChainArray<N, D> GenerateSubSegments(const ConnectionStrategy& connectionStrategy, double minSlope, const Point2DArray<N>& points, Tail&&... tail) const;

	// ----- Compute Color -----

	double ComputeColorBase(double dist, double radius) const;

	double ComputeColorPoint(double x, double y, const Point2D& point, double radius) const;

	template <size_t N>
	double ComputeColorPoints(double x, double y, const Point2DArray<N>& points, double radius) const;

	template <size_t N, size_t D>
	double ComputeColorPoints(double x, double y, const Segment3DChainArray<N, D>& segments, double radius) const;

	double ComputeColorSegment(double x, double y, const Segment2D& segment, double radius) const;

	template <size_t N, size_t D>
	double ComputeColorSegments(const Cell& cell, const Segment3DChainArray<N, D>& segments, int neighborhood, double x, double y, double radius) const;

	double ComputeColorGrid(double x, double y, double deltaX, double deltaY, double radius) const;

	template <size_t N1, size_t D1, size_t N2>
	double ComputeColor(double x, double y, const Cell& cell, const Segment3DChainArray<N1, D1>& segments, const Point2DArray<N2>& points) const;

	template <size_t N1, size_t D1, size_t N2, typename ...Tail>
	double ComputeColor(double x, double y, const Cell& cell, const Segment3DChainArray<N1, D1>& segments, const Point2DArray<N2>& points, Tail&&... tail) const;

	template <size_t N, typename ...Tail>
	double ComputeColorPrimitives(double x, double y, const Cell& higherResCell, const Point2DArray<N>& higherResPoints, Tail&&... tail) const;

	template <typename ...Tail>
	double ComputeColorControlFunction(double x, double y, Tail&&... tail) const;

	template <typename ...Tail>
	double ComputeColorDistance(double x, double y, Tail&&... tail) const;

	// Seed of the noise
	const int m_seed;

	// A control function
	const std::unique_ptr<I> m_controlFunction;

	const bool m_displayFunction;
	const bool m_displayPoints;
	const bool m_displaySegments;
	const bool m_displayGrid;
	const bool m_displayDistance;

	const Point2D m_noiseTopLeft;
	const Point2D m_noiseBottomRight;
	const Point2D m_controlFunctionTopLeft;
	const Point2D m_controlFunctionBottomRight;

	// Epsilon used to bias the area where points are generated in cells
	const double m_eps;

	// Number of resolutions in the noise function
	const int m_resolution;

	// Maximum displacement of segments
	const double m_displacement;

	// Additional resolution steps in the ComputeColorPrimitives function
	const int m_primitivesResolutionSteps;

	// Proportion of the amplitude of the control function as noise
	const double m_noiseAmplitudeProportion;


	const int CACHE_X = 128;
	const int CACHE_Y = 128;
	std::vector<std::vector<Point2D> > m_pointCache;
};

template <typename I>
Noise<I>::Noise(std::unique_ptr<I> controlFunction, const Point2D& noiseTopLeft, const Point2D& noiseBottomRight, const Point2D & controlFunctionTopLeft, const Point2D & controlFunctionBottomRight, int seed, double eps, int resolution, double displacement, int primitivesResolutionSteps, double noiseAmplitudeProportion, bool displayFunction, bool displayPoints, bool displaySegments, bool displayGrid, bool displayDistance) :
	m_seed(seed),
	m_controlFunction(std::move(controlFunction)),
	m_displayFunction(displayFunction),
	m_displayPoints(displayPoints),
	m_displaySegments(displaySegments),
	m_displayGrid(displayGrid),
	m_displayDistance(displayDistance),
	m_noiseTopLeft(noiseTopLeft),
	m_noiseBottomRight(noiseBottomRight),
	m_controlFunctionTopLeft(controlFunctionTopLeft),
	m_controlFunctionBottomRight(controlFunctionBottomRight),
	m_eps(eps),
	m_resolution(resolution),
	m_displacement(displacement),
    m_primitivesResolutionSteps(primitivesResolutionSteps),
	m_noiseAmplitudeProportion(noiseAmplitudeProportion)
{
	InitPointCache();
}

template <typename I>
void Noise<I>::InitPointCache()
{
	m_pointCache.resize(CACHE_X);

	for (int x = -CACHE_X / 2; x < CACHE_X / 2; x++)
	{
		m_pointCache[x + CACHE_X / 2].resize(CACHE_Y);

		for (int y = -CACHE_Y / 2; y < CACHE_Y / 2; y++)
		{
			m_pointCache[x + CACHE_X / 2][y + CACHE_Y / 2] = GeneratePoint(x, y);
		}
	}
}

template <typename I>
typename Noise<I>::RandomGenerator Noise<I>::InitRandomGenerator(int32_t i, int32_t j) const
{
	// TODO: implement a better permutation method
    uint64_t key = static_cast<uint64_t>(i) * UINT64_C(0x9e3779b97f4a7c15)
        ^ static_cast<uint64_t>(j) * UINT64_C(0xbf58476d1ce4e5b9)
        ^ static_cast<uint64_t>(m_seed);
    key = (key ^ (key >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    key = (key ^ (key >> 27)) * UINT64_C(0x94d049bb133111eb);
    return RandomGenerator(key ^ (key >> 31));
}

/// <summary>
/// Generate a point in a cell.
/// This function is reproducible.
/// </summary>
/// <param name="x">x coordinate of the cell</param>
/// <param name="y">y coordinate of the cell</param>
/// <returns>A Point2D in this cell</returns>
template <typename I>
Point2D Noise<I>::GeneratePoint(int x, int y) const
{
	RandomGenerator generator = InitRandomGenerator(x, y);

	std::uniform_real_distribution<double> distribution(m_eps, 1.0 - m_eps);
	const double px = distribution(generator);
	const double py = distribution(generator);

	return { double(x) + px, double(y) + py };
}

/// <summary>
/// Generate a point in a cell.
/// This function is reproducible.
/// Use the point cache if possible.
/// </summary>
/// <param name="x">x coordinate of the cell</param>
/// <param name="y">y coordinate of the cell</param>
/// <returns>A Point2D in this cell</returns>
template <typename I>
Point2D Noise<I>::GeneratePointCached(int x, int y) const
{
	if (x >= -CACHE_X / 2 && x < CACHE_X / 2 && y >= -CACHE_Y / 2 && y < CACHE_Y / 2)
	{
		return m_pointCache[x + CACHE_X / 2][y + CACHE_Y / 2];
	}
	else
	{
		return GeneratePoint(x, y);
	}
}

template <typename I>
typename Noise<I>::Cell Noise<I>::GetCell(double x, double y, int resolution) const
{
	// Return the coordinates of the cell in which (x, y)
	// For example, for resolution 1:
	// 
	//      0   1   2   3   
	//    0 -----------------
	//      |0;0|1;0|2;0|3;0|
	//    1 -----------------
	//      |0;1|1;1|2;1|3;1|
	//    2 -----------------
	//      |0;2|1;2|2;2|3;2|
	//    3 -----------------
	//      |0;3|1;3|2;3|3;3|
	//      -----------------
	//
	// If x is in [0, 1[ and y is in [0, 1[, then the cell is (0, 0)
	//
	// For example, for resolution 2:
	// 
	//      0       1       2  
	//    0 -----------------
	//      |0;0|1;0|2;0|3;0|
	//      -----------------
	//      |0;1|1;1|2;1|3;1|
	//    1 -----------------
	//      |0;2|1;2|2;2|3;2|
	//      -----------------
	//      |0;3|1;3|2;3|3;3|
	//    2 -----------------
	//
	// If x is in [0, 0.5[ and y is in [0, 0.5[, then the cell is (0, 0)

	// Resolution is strictly positive
	assert(resolution > 0);

	const auto cellX = int(floor(x * resolution));
	const auto cellY = int(floor(y * resolution));

	return { cellX, cellY , resolution };
}

/// <summary>
/// Evaluate the control function at a point (x, y)
/// </summary>
/// <param name="point">Coordinates of the point</param>
/// <returns>The value of the function at the point</returns>
template <typename I>
double Noise<I>::EvaluateControlFunction(const Point2D& point) const
{
	const double x = remap(point.x, m_noiseTopLeft.x, m_noiseBottomRight.x, m_controlFunctionTopLeft.x, m_controlFunctionBottomRight.x);
	const double y = remap(point.y, m_noiseTopLeft.y, m_noiseBottomRight.y, m_controlFunctionTopLeft.y, m_controlFunctionBottomRight.y);

	double value = 0.0;

	if (m_controlFunction)
	{
		value = m_controlFunction->evaluate(x, y);
	}

	return value;
}

/// <summary>
/// Check if one point (x, y) is in the domain of the control function
/// </summary>
/// <param name="point">Coordinates of the point</param>
/// <returns>True if the point is in the domain of the function</returns>
template <typename I>
double Noise<I>::EvaluateReliefSlope(const Point2D& point) const
{
    const double x = remap(point.x, m_noiseTopLeft.x, m_noiseBottomRight.x,
        m_controlFunctionTopLeft.x, m_controlFunctionBottomRight.x);
    const double y = remap(point.y, m_noiseTopLeft.y, m_noiseBottomRight.y,
        m_controlFunctionTopLeft.y, m_controlFunctionBottomRight.y);
    return static_cast<const I*>(m_controlFunction.get())->reliefSlope(x, y);
}

template <typename I>
bool Noise<I>::InsideDomain(const Point2D& point) const
{
	const double x = remap(point.x, m_noiseTopLeft.x, m_noiseBottomRight.x, m_controlFunctionTopLeft.x, m_controlFunctionBottomRight.x);
	const double y = remap(point.y, m_noiseTopLeft.y, m_noiseBottomRight.y, m_controlFunctionTopLeft.y, m_controlFunctionBottomRight.y);

	bool value = false;

	if (m_controlFunction)
	{
		value = m_controlFunction->insideDomain(x, y);
	}

	return value;
}

template <typename I>
bool Noise<I>::InsideDomain(const Segment2D& segment) const
{
	return (InsideDomain(segment.a) && InsideDomain(segment.b));
}

template <typename I>
bool Noise<I>::InsideDomain(const Point3D& point) const {
	return InsideDomain(ProjectionZ(point));
}

template <typename I>
bool Noise<I>::InsideDomain(const Segment3D& segment) const
{
	return (InsideDomain(segment.a) && InsideDomain(segment.b));
}

template <typename I>
bool Noise<I>::DistToDomain(const Point2D& point) const
{
	const double x = remap(point.x, m_noiseTopLeft.x, m_noiseBottomRight.x, m_controlFunctionTopLeft.x, m_controlFunctionBottomRight.x);
	const double y = remap(point.y, m_noiseTopLeft.y, m_noiseBottomRight.y, m_controlFunctionTopLeft.y, m_controlFunctionBottomRight.y);

	double value = 0.0;

	if (m_controlFunction)
	{
		value = m_controlFunction->distToDomain(x, y);
	}

	return value;
}

template <typename I>
double Noise<I>::ControlFunctionMinimum() const
{
	double value = 0.0;

	if (m_controlFunction)
	{
		value = m_controlFunction->minimum();
	}

	return value;
}

template <typename I>
double Noise<I>::ControlFunctionMaximum() const
{
	double value = 0.0;

	if (m_controlFunction)
	{
		value = m_controlFunction->maximum();
	}

	return value;
}

/// <summary>
/// Connect a point to a segment
/// If the nearest point lies on the segment (between A and B), the point is connected to the segment to form a 45 degrees angle
/// If the nearest point on the segment is A or B, connect to it.
/// </summary>
/// <param name="point">Coordinates of the point to connect</param>
/// <param name="segmentDist">Distance from the point to the segment</param>
/// <param name="segment">Segment with which connect the point</param>
/// <returns>A chain of segments connecting the point with the segment</returns>
template <typename I>
template <size_t D>
typename Noise<I>::template Segment3DChain<D> Noise<I>::ConnectPointToSegmentAngle(const Point3D & point, double segmentDist, const Segment3D& segment) const
{
	Segment3DChain<D> generatedSegment;

	// Find an intersection on the segment with respect to constraints
	// u = 0 is point A of the segment ; u = 1 is point B of the segment
	double u = pointLineSegmentProjection(ProjectionZ(point), ProjectionZ(segment));

	// If, on the segment, the nearest point is between A and B, we shift it so that the angle constraint is respected
	if (u > 0.0 && u < 1.0)
	{
		// Find the intersection so that the angle between the two segments is 45°
		// v designates the ratio of the segment on which the intersection is located
		// v = 0 is point A of the segment ; v = 1 is point B of the segment
		const double v = u + segmentDist / length(ProjectionZ(segment));

		if (v > 1.0)
		{
			// If the intersection is farther than B, simply take B as intersection
			u = 1.0;
		}
		else
		{
			// Otherwise take a point on the segment
			u = v;
		}
	}

	const Point3D straightSegmentEnd(lerp(segment, u));
	const Segment3D straightSegment(point, straightSegmentEnd);

	// Subdivide the straightSegment into D smaller segments
	std::array<Point3D, D - 1> generatedSegmentPoints;
	if (length_sq(straightSegment) > 0.0 && length_sq(segment))
	{
		// If the segment exists, we can smooth it
		const Point3D splineStart = 2.0 * straightSegment.a - straightSegment.b;
		const Point3D splineEnd = 2.0 * segment.b - segment.a;
		generatedSegmentPoints = SubdivideCatmullRomSpline<D - 1>(splineStart, straightSegment.a, straightSegment.b, splineEnd);
	}
	else
	{
		// If the segment is a point, it is impossible to smooth it
		generatedSegmentPoints = SubdivideInPoints<D - 1>(straightSegment);
	}

	SegmentChainFromPoints(straightSegment.a, generatedSegmentPoints, straightSegment.b, generatedSegment);
	
	return generatedSegment;
}

/// <summary>
/// Connect a point to a segment
/// Most of the time, the point is connected to the segment to form a 45 degrees angle
/// The rest of the time it is connected to either segment.a or segment.b
/// </summary>
/// <param name="point">Coordinates of the point to connect</param>
/// <param name="segmentDist">Distance from the point to the segment</param>
/// <param name="segment">Segment with which connect the point</param>
/// <returns>A chain of segments connecting the point with the segment</returns>
template <typename I>
template <size_t D>
typename Noise<I>::template Segment3DChain<D> Noise<I>::ConnectPointToSegmentAngleMid(const Point3D& point, double segmentDist, const Segment3D& segment) const
{
	Segment3DChain<D> generatedSegment;

	// Find an intersection on the segment with respect to constraints
	// u = 0 is point A of the segment ; u = 1 is point B of the segment
	const double u = pointLineProjection(ProjectionZ(point), ProjectionZ(segment));

	const double tanAngle = 1.0; // tan(45 deg) = 1.0
	// Find the intersection so that the angle between the two segments is 45°
	// v designates the ratio of the segment on which the intersection is located
	// v = 0 is point A of the segment ; v = 1 is point B of the segment
	// TODO: segmentDist could actually not be the distance to the line which means the angle is not 45 degrees
	// TODO: make sure that segmentDist is the distance to the line (AB) using u or by recomputing it in this function
	double v = u + (segmentDist / tanAngle) / length(ProjectionZ(segment));
	// The intersection must lie on the segment
	v = std::clamp(v, 0.0, 1.0);

	const Point3D straightSegmentEnd(lerp(segment, v));
	const Segment3D straightSegment(point, straightSegmentEnd);

	// Subdivide the straightSegment into D smaller segments
	std::array<Point3D, D - 1> generatedSegmentPoints;
	if (length_sq(straightSegment) > 0.0 && length_sq(segment))
	{
		// If the segment exists, we can smooth it
		const Point3D splineStart = 2.0 * straightSegment.a - straightSegment.b;
		const Point3D splineEnd = 2.0 * segment.b - segment.a;
		generatedSegmentPoints = SubdivideCatmullRomSpline<D - 1>(splineStart, straightSegment.a, straightSegment.b, splineEnd);
	}
	else
	{
		// If the segment is a point, it is impossible to smooth it
		generatedSegmentPoints = SubdivideInPoints<D - 1>(straightSegment);
	}

	SegmentChainFromPoints(straightSegment.a, generatedSegmentPoints, straightSegment.b, generatedSegment);

	return generatedSegment;
}

/// <summary>
/// Connect a point to a segment
/// The point is connected to the nearest point on the segment
/// Most of the time, the generated segment is orthogonal to the segment
/// The rest of the time it is connected to segment.a
/// </summary>
/// <param name="point">Coordinates of the point to connect</param>
/// <param name="segmentDist">Distance from the point to the segment</param>
/// <param name="segment">Segment with which connect the point</param>
/// <returns>A chain of segments connecting the point with the segment</returns>
template <typename I>
template <size_t D>
typename Noise<I>::template Segment3DChain<D> Noise<I>::ConnectPointToSegmentNearestPoint(const Point3D& point, double segmentDist, const Segment3D& segment) const
{
	// Find an intersection on the segment with respect to constraints
	// u = 0 is point A of the segment ; u = 1 is point B of the segment
	const double u = pointLineSegmentProjection(ProjectionZ(point), ProjectionZ(segment));

	const Point3D segmentEnd(lerp(segment, u));
	const Segment3D straightSegment(point, segmentEnd);

	// Subdivide the straightSegment into D smaller segments
	return SubdivideInSegments<D>(straightSegment);
}

template <typename I>
template <size_t D>
typename Noise<I>::template Segment3DChain<D> Noise<I>::ConnectPointToSegmentRivers(const Point3D& point, double segmentDist, const Segment3D& segment) const
{
	Segment3DChain<D> generatedSegment;

	// The connection point is the nearest point among A, B and middle of the segment
	Point3D connectionPoint = MidPoint(segment);
	double distConnectionPoint = dist(connectionPoint, point);

	const double distanceA = dist(segment.a, point);
	if (distanceA < distConnectionPoint)
	{
		connectionPoint = segment.a;
		distConnectionPoint = distanceA;
	}

	const double distanceB = dist(segment.b, point);
	if (distanceB < distConnectionPoint)
	{
		connectionPoint = segment.b;
		distConnectionPoint = distanceB;
	}

	// Segment before subdivision
	const Segment3D straightSegment(point, connectionPoint);

	// Subdivide the straightSegment into D smaller segments
	std::array<Point3D, D - 1> generatedSegmentPoints;
	if (length_sq(straightSegment) > 0.0 && canSmoothRiverConnection(straightSegment, segment))
	{
		// Compute the connection angle
		const double mainSegmentSlope = std::abs(segment.b.z - segment.a.z) / length(ProjectionZ(segment));
		const double tributarySlope = std::abs(straightSegment.b.z - straightSegment.a.z) / length(ProjectionZ(straightSegment));		
		double connectionAngle = 0.0;
		if (tributarySlope >= 0.0 && mainSegmentSlope <= tributarySlope)
		{
			connectionAngle = acos(mainSegmentSlope / tributarySlope);
		}

		// The three points (segment.a, point, segment.b) constitute a plane
		// The normal of this plane is the cross product between IP and AB
		const Vec3D vecSegment(segment.a, segment.b);
		const Vec3D vecStraightSegment(straightSegment.b, straightSegment.a);
		const Vec3D normal = normalized(cross(vecStraightSegment, vecSegment));
		const Vec3D result = rotate_axis(normalized(vecSegment), normal, connectionAngle);

		// If the segment exists, we can smooth it
		const Point3D splineStart = 2.0 * straightSegment.a - straightSegment.b;
		const Point3D splineEnd = connectionPoint + (result * 0.1 * norm(vecStraightSegment));
		generatedSegmentPoints = SubdivideCatmullRomSpline<D - 1>(splineStart, straightSegment.a, straightSegment.b, splineEnd);
	}
	else
	{
		// If the segment is a point, it is impossible to smooth it
		generatedSegmentPoints = SubdivideInPoints<D - 1>(straightSegment);
	}

	SegmentChainFromPoints(straightSegment.a, generatedSegmentPoints, straightSegment.b, generatedSegment);

	return generatedSegment;
}

template <typename I>
template <size_t D>
typename Noise<I>::template Segment3DChain<D> Noise<I>::ConnectPointToSegment(const ConnectionStrategy& strategy, const Point3D& point, double segmentDist, const Segment3D& segment) const
{
	Segment3DChain<D> connectionSegments;

	switch (strategy)
	{
	case ConnectionStrategy::Angle:
		connectionSegments = ConnectPointToSegmentAngle<D>(point ,segmentDist, segment);
		break;

	case ConnectionStrategy::AngleMid:
		connectionSegments = ConnectPointToSegmentAngleMid<D>(point, segmentDist, segment);
		break;

	case ConnectionStrategy::Rivers:
		connectionSegments = ConnectPointToSegmentRivers<D>(point, segmentDist, segment);
		break;

	default:
	case ConnectionStrategy::NearestPoint:
		connectionSegments = ConnectPointToSegmentNearestPoint<D>(point, segmentDist, segment);
		break;
	}

	return connectionSegments;
}

template <typename I>
double Noise<I>::ComputeColorBase(double dist, double radius) const
{
	if (dist < radius)
	{
		return 1.0;
	}

	return 0.0;
}

template <typename I>
double Noise<I>::ComputeColorPoint(double x, double y, const Point2D& point, double radius) const
{
	const double d = dist(Point2D(x, y), point);
	return ComputeColorBase(d, radius);
}

template <typename I>
double Noise<I>::ComputeColorSegment(double x, double y, const Segment2D& segment, double radius) const
{
	Point2D c;
	const double d = distToLineSegment(Point2D(x, y), segment, c);
	return ComputeColorBase(d, radius);
}

template <typename I>
double Noise<I>::ComputeColorGrid(double x, double y, double deltaX, double deltaY, double radius) const
{
	double value = 0.0;

	// When near to the grid
	if (std::abs(x - floor(x) - deltaX) < radius || std::abs(y - floor(y) - deltaY) < radius)
	{
		value = 1.0;
	}

	return value;
}

template <typename I>
template <typename Visitor>
void Noise<I>::visitSegments(double x, double y, Visitor visitor) const
{
    const Cell cell1 = GetCell(x, y, 1);
    Point2DArray<9> points1 = GenerateNeighboringPoints<9>(cell1);
    const Segment3DChainArray<7, 1> straight1 = GenerateSegments(points1);
    Segment3DChainArray<5, 4> segments1;
    SubdivideSegments(cell1, straight1, segments1);
    DisplaceSegments(m_displacement, cell1, segments1);
    const Cell cell2 = GetCell(x, y, 2);
    Point2DArray<5> points2 = GenerateNeighboringPoints<5>(cell2);
    ReplaceNeighboringPoints(cell1, points1, cell2, points2);
    auto segments2 = GenerateSubSegments<5, 3>(ConnectionStrategy::Rivers, 0.09,
        points2, cell1, segments1);
    DisplaceSegments(m_displacement / 4.0, cell2, segments2);
    const Cell cell3 = GetCell(x, y, 4);
    Point2DArray<5> points3 = GenerateNeighboringPoints<5>(cell3);
    ReplaceNeighboringPoints(cell2, points2, cell3, points3);
    auto segments3 = GenerateSubSegments<5, 2>(ConnectionStrategy::Rivers, 0.18,
        points3, cell1, segments1, cell2, segments2);
    DisplaceSegments(m_displacement / 16.0, cell3, segments3);
    const auto emit = [&](const uint32_t level, const Cell& cell, const auto& segments) {
        const auto ownerX = static_cast<uint32_t>(robust_mod(cell.x, 32 * cell.resolution));
        const auto ownerY = static_cast<uint32_t>(robust_mod(cell.y, 32 * cell.resolution));
        for (uint32_t k = 0; k < segments[2][2].size(); ++k) {
            visitor(level, ownerX, ownerY, k, segments[2][2][k]);
        }
    };
    emit(1, cell1, segments1);
    emit(2, cell2, segments2);
    emit(3, cell3, segments3);
}


template <typename I>
std::shared_ptr<const typename Noise<I>::Graph1Memo> Noise<I>::memoGraph1(uint32_t x, uint32_t y) const
{
    return graph1Memo_.get(x, y, [&]() {
        Graph1Memo entry{};
        entry.cell = Cell(static_cast<int32_t>(x), static_cast<int32_t>(y), 1);
        entry.points = GenerateNeighboringPoints<9>(entry.cell);
        const auto straight = GenerateSegments(entry.points);
        SubdivideSegments(entry.cell, straight, entry.segments);
        DisplaceSegments(m_displacement, entry.cell, entry.segments);
        return entry;
    });
}

template <typename I>
std::shared_ptr<const typename Noise<I>::Graph2Memo> Noise<I>::memoGraph2(uint32_t x, uint32_t y) const
{
    return graph2Memo_.get(x, y, [&]() {
        Graph2Memo entry{};
        entry.cell = Cell(static_cast<int32_t>(x), static_cast<int32_t>(y), 2);
        entry.base = memoGraph1(x / 2, y / 2);
        entry.points = GenerateNeighboringPoints<5>(entry.cell);
        ReplaceNeighboringPoints(entry.base->cell, entry.base->points, entry.cell, entry.points);
        entry.segments = GenerateSubSegments<5, 3>(ConnectionStrategy::Rivers, 0.09, entry.points,
            entry.base->cell, entry.base->segments);
        DisplaceSegments(m_displacement / 4.0, entry.cell, entry.segments);
        return entry;
    });
}

template <typename I>
std::shared_ptr<const typename Noise<I>::Graph3Memo> Noise<I>::memoGraph3(uint32_t x, uint32_t y) const
{
    return graph3Memo_.get(x, y, [&]() {
        Graph3Memo entry{};
        entry.cell = Cell(static_cast<int32_t>(x), static_cast<int32_t>(y), 4);
        entry.parent = memoGraph2(x / 2, y / 2);
        entry.base = entry.parent->base;
        entry.points = GenerateNeighboringPoints<5>(entry.cell);
        ReplaceNeighboringPoints(entry.parent->cell, entry.parent->points, entry.cell, entry.points);
        entry.segments = GenerateSubSegments<5, 2>(ConnectionStrategy::Rivers, 0.18, entry.points,
            entry.base->cell, entry.base->segments, entry.parent->cell, entry.parent->segments);
        DisplaceSegments(m_displacement / 16.0, entry.cell, entry.segments);
        return entry;
    });
}

template <typename I>
typename Noise<I>::PrimitiveMemo Noise<I>::buildPrimitiveMemo(uint32_t x, uint32_t y) const
{
    const auto graph = memoGraph3(x / 8, y / 8);
    Cell highestCell = graph->cell;
    Point2DArray<5> highestPoints = graph->points;
    for (uint32_t step = 0; step < 3; ++step) {
        const uint32_t divisor = 4 >> step;
        const Cell next(static_cast<int32_t>(x / divisor), static_cast<int32_t>(y / divisor),
            static_cast<int32_t>(8 << step));
        auto nextPoints = GenerateNeighboringPoints<5>(next);
        ReplaceNeighboringPoints(highestCell, highestPoints, next, nextPoints);
        highestCell = next;
        highestPoints = nextPoints;
    }
    PrimitiveMemo entry{};
    entry.points = highestPoints;
    entry.radius = 2.0 / highestCell.resolution;
    for (uint32_t i = 0; i < highestPoints.size(); ++i) {
        for (uint32_t j = 0; j < highestPoints[i].size(); ++j) {
            Cell nearestCell;
            Segment3D nearest;
            const double distanceCenter = NearestSegmentAndCellProjectionZ(1, highestPoints[i][j],
                nearestCell, nearest, graph->base->cell, graph->base->segments,
                graph->parent->cell, graph->parent->segments, graph->cell, graph->segments);
            const double u = pointLineSegmentProjection(highestPoints[i][j], ProjectionZ(nearest));
            const double nearestHeight = lerp(nearest.a.z, nearest.b.z, u);
            const double minimum = ControlFunctionMinimum();
            const double maximum = ControlFunctionMaximum();
            const double slope = EvaluateReliefSlope(highestPoints[i][j]);
            const double amplitudeMax = m_noiseAmplitudeProportion * (maximum - minimum)
                / graph->cell.resolution;
            const double terrainSizeX = m_noiseBottomRight.x - m_noiseTopLeft.x;
            const double terrainSizeY = m_noiseBottomRight.y - m_noiseTopLeft.y;
            const double higherCellSize = std::max(terrainSizeX, terrainSizeY) / graph->cell.resolution;
            const double highestCellSizeX = terrainSizeX / highestCell.resolution;
            const double highestCellSizeY = terrainSizeY / highestCell.resolution;
            const double amplitude = amplitudeMax * smootherstep(0.0, higherCellSize / 4.0, distanceCenter);
            if (i == 0 && j == 0) {
                entry.amplitude = amplitude;
            } else if (amplitude != entry.amplitude
                || std::signbit(amplitude) != std::signbit(entry.amplitude)) {
                throw std::logic_error("frozen zero-amplitude primitive contract changed");
            }
            entry.wavelengthX = highestCellSizeX / 4.0;
            entry.wavelengthY = highestCellSizeY / 4.0;
            entry.preNoise[i][j] = nearestHeight + slope * distanceCenter;
        }
    }
    return entry;
}

template <typename I>
std::shared_ptr<const typename Noise<I>::PrimitiveMemo> Noise<I>::memoPrimitives(uint32_t x, uint32_t y) const
{
    return primitiveMemo_.get(x, y, [&]() {
        const auto graph = memoGraph3(x / 8, y / 8);
        Cell highestCell = graph->cell;
        Point2DArray<5> highestPoints = graph->points;
        for (uint32_t step = 0; step < 3; ++step) {
            const uint32_t divisor = 4 >> step;
            const Cell next(static_cast<int32_t>(x / divisor), static_cast<int32_t>(y / divisor),
                static_cast<int32_t>(8 << step));
            auto nextPoints = GenerateNeighboringPoints<5>(next);
            ReplaceNeighboringPoints(highestCell, highestPoints, next, nextPoints);
            highestCell = next;
            highestPoints = nextPoints;
        }
        PrimitiveMemo entry{};
        entry.points = highestPoints;
        entry.radius = 2.0 / highestCell.resolution;
        for (uint32_t i = 0; i < highestPoints.size(); ++i) {
            for (uint32_t j = 0; j < highestPoints[i].size(); ++j) {
                Cell nearestCell;
                Segment3D nearest;
                const double distanceCenter = NearestSegmentAndCellProjectionZ(1, highestPoints[i][j],
                    nearestCell, nearest, graph->base->cell, graph->base->segments,
                    graph->parent->cell, graph->parent->segments, graph->cell, graph->segments);
                const double u = pointLineSegmentProjection(highestPoints[i][j], ProjectionZ(nearest));
                const double nearestHeight = lerp(nearest.a.z, nearest.b.z, u);
                const double minimum = ControlFunctionMinimum();
                const double maximum = ControlFunctionMaximum();
                const double slope = EvaluateReliefSlope(highestPoints[i][j]);
                const double amplitudeMax = m_noiseAmplitudeProportion * (maximum - minimum)
                    / graph->cell.resolution;
                const double terrainSizeX = m_noiseBottomRight.x - m_noiseTopLeft.x;
                const double terrainSizeY = m_noiseBottomRight.y - m_noiseTopLeft.y;
                const double higherCellSize = std::max(terrainSizeX, terrainSizeY) / graph->cell.resolution;
                const double highestCellSizeX = terrainSizeX / highestCell.resolution;
                const double highestCellSizeY = terrainSizeY / highestCell.resolution;
                const double amplitude = amplitudeMax * smootherstep(0.0, higherCellSize / 4.0, distanceCenter);
                if (i == 0 && j == 0) {
                    entry.amplitude = amplitude;
                } else if (amplitude != entry.amplitude
                    || std::signbit(amplitude) != std::signbit(entry.amplitude)) {
                    throw std::logic_error("frozen zero-amplitude primitive contract changed");
                }
                entry.wavelengthX = highestCellSizeX / 4.0;
                entry.wavelengthY = highestCellSizeY / 4.0;
                entry.preNoise[i][j] = nearestHeight + slope * distanceCenter;
            }
        }
        return entry;
    });
}

template <typename I>
double Noise<I>::evaluateTerrain(const Point2D& point, const PrimitiveMemo& entry) const
{
    const double noise = entry.amplitude * Perlin(point.x / entry.wavelengthX, point.y / entry.wavelengthY)
        + 0.5 * entry.amplitude * Perlin(point.x / (2.0 * entry.wavelengthX), point.y / (2.0 * entry.wavelengthY))
        + 0.25 * entry.amplitude * Perlin(point.x / (4.0 * entry.wavelengthX), point.y / (4.0 * entry.wavelengthY));
    double numerator = 0.0;
    double denominator = 0.0;
    for (uint32_t i = 0; i < entry.points.size(); ++i) {
        for (uint32_t j = 0; j < entry.points[i].size(); ++j) {
            const double distancePrimitive = dist(point, entry.points[i][j]);
            const double alpha = WyvillGalinFunction(distancePrimitive, entry.radius, 3.0);
            const double elevation = entry.preNoise[i][j] + noise;
            numerator += alpha * elevation;
            denominator += alpha;
        }
    }
    assert(denominator != 0.0);
    return numerator / denominator;
}

template <typename I>
double Noise<I>::evaluateTerrainCached(double x, double y) const
{
    if (m_resolution != 3 || m_primitivesResolutionSteps != 3 || m_noiseAmplitudeProportion != 0.0
        || !m_displayFunction || m_displayPoints || m_displaySegments || m_displayGrid || m_displayDistance) {
        throw std::invalid_argument("cache supports only the frozen graph3 height field");
    }
    const auto canonical = [](const double value) {
        const double result = std::fmod(value, 32.0);
        return result < 0 ? result + 32.0 : result;
    };
    x = canonical(x);
    y = canonical(y);
    const Cell cell = GetCell(x, y, 32);
    const auto entry = memoPrimitives(static_cast<uint32_t>(cell.x), static_cast<uint32_t>(cell.y));
    const double noise = entry->amplitude * Perlin(x / entry->wavelengthX, y / entry->wavelengthY)
        + 0.5 * entry->amplitude * Perlin(x / (2.0 * entry->wavelengthX), y / (2.0 * entry->wavelengthY))
        + 0.25 * entry->amplitude * Perlin(x / (4.0 * entry->wavelengthX), y / (4.0 * entry->wavelengthY));
    const Point2D point(x, y);
    double numerator = 0.0;
    double denominator = 0.0;
    for (uint32_t i = 0; i < entry->points.size(); ++i) {
        for (uint32_t j = 0; j < entry->points[i].size(); ++j) {
            const double distancePrimitive = dist(point, entry->points[i][j]);
            const double alpha = WyvillGalinFunction(distancePrimitive, entry->radius, 3.0);
            const double elevation = entry->preNoise[i][j] + noise;
            numerator += alpha * elevation;
            denominator += alpha;
        }
    }
    assert(denominator != 0.0);
    return numerator / denominator;
}

template <typename I>
double Noise<I>::evaluateTerrainUncached(double x, double y) const
{
    if (m_resolution != 3 || m_primitivesResolutionSteps != 3 || m_noiseAmplitudeProportion != 0.0
        || !m_displayFunction || m_displayPoints || m_displaySegments || m_displayGrid || m_displayDistance) {
        throw std::invalid_argument("uncached evaluation supports only the frozen graph3 height field");
    }
    const auto canonical = [](const double value) {
        const double result = std::fmod(value, 32.0);
        return result < 0 ? result + 32.0 : result;
    };
    x = canonical(x);
    y = canonical(y);
    const Cell cell = GetCell(x, y, 32);
    const PrimitiveMemo entry = buildPrimitiveMemo(static_cast<uint32_t>(cell.x), static_cast<uint32_t>(cell.y));
    return evaluateTerrain(Point2D(x, y), entry);
}

template <typename I>
std::array<uint64_t, 4> Noise<I>::cacheEntries() const
{
    return {graph1Memo_.entries(), graph2Memo_.entries(), graph3Memo_.entries(), primitiveMemo_.entries()};
}

template <typename I>
uint64_t Noise<I>::cacheLogicalBytes() const
{
    return graph1Memo_.logicalBytes() + graph2Memo_.logicalBytes() + graph3Memo_.logicalBytes()
        + primitiveMemo_.logicalBytes();
}

template <typename I>
uint64_t Noise<I>::cacheLogicalCapacityBytes() const
{
    return graph1Memo_.logicalCapacityBytes() + graph2Memo_.logicalCapacityBytes()
        + graph3Memo_.logicalCapacityBytes() + primitiveMemo_.logicalCapacityBytes();
}


template <typename I>
std::shared_ptr<const typename Noise<I>::Graph4Memo> Noise<I>::memoGraph4(uint32_t x, uint32_t y) const
{
    return graph4Memo_.get(x, y, [&]() {
        Graph4Memo entry{};
        entry.cell = Cell(static_cast<int32_t>(x), static_cast<int32_t>(y), 8);
        entry.parent = memoGraph3(x / 2, y / 2);
        entry.points = GenerateNeighboringPoints<5>(entry.cell);
        ReplaceNeighboringPoints(entry.parent->cell, entry.parent->points, entry.cell, entry.points);
        const auto& g3 = *entry.parent;
        const auto& g2 = *g3.parent;
        const auto& g1 = *g3.base;
        entry.segments = GenerateSubSegments<5, 1>(ConnectionStrategy::Rivers, .38, entry.points,
            g1.cell, g1.segments, g2.cell, g2.segments, g3.cell, g3.segments);
        return entry;
    });
}

template <typename I>
std::shared_ptr<const typename Noise<I>::PrimitiveMemo> Noise<I>::memoPrimitive4(uint32_t x, uint32_t y) const
{
    return primitive4Memo_.get(x, y, [&]() {
        const auto graph = memoGraph4(x / 8, y / 8);
        const auto& g3 = *graph->parent;
        const auto& g2 = *g3.parent;
        const auto& g1 = *g3.base;
        Cell highestCell = graph->cell;
        Point2DArray<5> highestPoints = graph->points;
        for (uint32_t step = 0; step < 3; ++step) {
            const uint32_t divisor = 4 >> step;
            const Cell next(static_cast<int32_t>(x / divisor), static_cast<int32_t>(y / divisor),
                static_cast<int32_t>(16 << step));
            auto nextPoints = GenerateNeighboringPoints<5>(next);
            ReplaceNeighboringPoints(highestCell, highestPoints, next, nextPoints);
            highestCell = next;
            highestPoints = nextPoints;
        }
        PrimitiveMemo entry{};
        entry.points = highestPoints;
        entry.radius = 2.0 / highestCell.resolution;
        for (uint32_t i = 0; i < 5; ++i) {
            for (uint32_t j = 0; j < 5; ++j) {
                Cell nearestCell;
                Segment3D nearest;
                const double distanceCenter = NearestSegmentAndCellProjectionZ(1, highestPoints[i][j],
                    nearestCell, nearest, g1.cell, g1.segments, g2.cell, g2.segments, g3.cell, g3.segments, graph->cell, graph->segments);
                const double u = pointLineSegmentProjection(highestPoints[i][j], ProjectionZ(nearest));
                entry.preNoise[i][j] = lerp(nearest.a.z, nearest.b.z, u)
                    + EvaluateReliefSlope(highestPoints[i][j]) * distanceCenter;
            }
        }
        return entry;
    });
}

template <typename I>
std::shared_ptr<const typename Noise<I>::Graph5Memo> Noise<I>::memoGraph5(uint32_t x, uint32_t y) const
{
    return graph5Memo_.get(x, y, [&]() {
        Graph5Memo entry{};
        entry.cell = Cell(static_cast<int32_t>(x), static_cast<int32_t>(y), 16);
        entry.parent = memoGraph4(x / 2, y / 2);
        entry.points = GenerateNeighboringPoints<5>(entry.cell);
        ReplaceNeighboringPoints(entry.parent->cell, entry.parent->points, entry.cell, entry.points);
        const auto& g4 = *entry.parent;
        const auto& g3 = *g4.parent;
        const auto& g2 = *g3.parent;
        const auto& g1 = *g3.base;
        entry.segments = GenerateSubSegments<5, 1>(ConnectionStrategy::Rivers, 1.0, entry.points,
            g1.cell, g1.segments, g2.cell, g2.segments, g3.cell, g3.segments, g4.cell, g4.segments);
        return entry;
    });
}

template <typename I>
std::shared_ptr<const typename Noise<I>::PrimitiveMemo> Noise<I>::memoPrimitive5(uint32_t x, uint32_t y) const
{
    return primitive5Memo_.get(x, y, [&]() {
        const auto graph = memoGraph5(x / 8, y / 8);
        const auto& g4 = *graph->parent;
        const auto& g3 = *g4.parent;
        const auto& g2 = *g3.parent;
        const auto& g1 = *g3.base;
        Cell highestCell = graph->cell;
        Point2DArray<5> highestPoints = graph->points;
        for (uint32_t step = 0; step < 3; ++step) {
            const uint32_t divisor = 4 >> step;
            const Cell next(static_cast<int32_t>(x / divisor), static_cast<int32_t>(y / divisor),
                static_cast<int32_t>(32 << step));
            auto nextPoints = GenerateNeighboringPoints<5>(next);
            ReplaceNeighboringPoints(highestCell, highestPoints, next, nextPoints);
            highestCell = next;
            highestPoints = nextPoints;
        }
        PrimitiveMemo entry{};
        entry.points = highestPoints;
        entry.radius = 2.0 / highestCell.resolution;
        for (uint32_t i = 0; i < 5; ++i) {
            for (uint32_t j = 0; j < 5; ++j) {
                Cell nearestCell;
                Segment3D nearest;
                const double distanceCenter = NearestSegmentAndCellProjectionZ(1, highestPoints[i][j],
                    nearestCell, nearest, g1.cell, g1.segments, g2.cell, g2.segments, g3.cell, g3.segments, g4.cell, g4.segments, graph->cell, graph->segments);
                const double u = pointLineSegmentProjection(highestPoints[i][j], ProjectionZ(nearest));
                entry.preNoise[i][j] = lerp(nearest.a.z, nearest.b.z, u)
                    + EvaluateReliefSlope(highestPoints[i][j]) * distanceCenter;
            }
        }
        return entry;
    });
}

template <typename I>
std::shared_ptr<const typename Noise<I>::Graph6Memo> Noise<I>::memoGraph6(uint32_t x, uint32_t y) const
{
    return graph6Memo_.get(x, y, [&]() {
        Graph6Memo entry{};
        entry.cell = Cell(static_cast<int32_t>(x), static_cast<int32_t>(y), 32);
        entry.parent = memoGraph5(x / 2, y / 2);
        entry.points = GenerateNeighboringPoints<5>(entry.cell);
        ReplaceNeighboringPoints(entry.parent->cell, entry.parent->points, entry.cell, entry.points);
        const auto& g5 = *entry.parent;
        const auto& g4 = *g5.parent;
        const auto& g3 = *g4.parent;
        const auto& g2 = *g3.parent;
        const auto& g1 = *g3.base;
        entry.segments = GenerateSubSegments<5, 1>(ConnectionStrategy::Rivers, 1.0, entry.points,
            g1.cell, g1.segments, g2.cell, g2.segments, g3.cell, g3.segments, g4.cell, g4.segments, g5.cell, g5.segments);
        return entry;
    });
}

template <typename I>
std::shared_ptr<const typename Noise<I>::PrimitiveMemo> Noise<I>::memoPrimitive6(uint32_t x, uint32_t y) const
{
    return primitive6Memo_.get(x, y, [&]() {
        const auto graph = memoGraph6(x / 8, y / 8);
        const auto& g5 = *graph->parent;
        const auto& g4 = *g5.parent;
        const auto& g3 = *g4.parent;
        const auto& g2 = *g3.parent;
        const auto& g1 = *g3.base;
        Cell highestCell = graph->cell;
        Point2DArray<5> highestPoints = graph->points;
        for (uint32_t step = 0; step < 3; ++step) {
            const uint32_t divisor = 4 >> step;
            const Cell next(static_cast<int32_t>(x / divisor), static_cast<int32_t>(y / divisor),
                static_cast<int32_t>(64 << step));
            auto nextPoints = GenerateNeighboringPoints<5>(next);
            ReplaceNeighboringPoints(highestCell, highestPoints, next, nextPoints);
            highestCell = next;
            highestPoints = nextPoints;
        }
        PrimitiveMemo entry{};
        entry.points = highestPoints;
        entry.radius = 2.0 / highestCell.resolution;
        for (uint32_t i = 0; i < 5; ++i) {
            for (uint32_t j = 0; j < 5; ++j) {
                Cell nearestCell;
                Segment3D nearest;
                const double distanceCenter = NearestSegmentAndCellProjectionZ(1, highestPoints[i][j],
                    nearestCell, nearest, g1.cell, g1.segments, g2.cell, g2.segments, g3.cell, g3.segments, g4.cell, g4.segments, g5.cell, g5.segments, graph->cell, graph->segments);
                const double u = pointLineSegmentProjection(highestPoints[i][j], ProjectionZ(nearest));
                entry.preNoise[i][j] = lerp(nearest.a.z, nearest.b.z, u)
                    + EvaluateReliefSlope(highestPoints[i][j]) * distanceCenter;
            }
        }
        return entry;
    });
}

template <typename I>
double Noise<I>::evaluateHierarchy(double x, double y, uint32_t level) const
{
    if (level == 3) {
        return evaluateTerrainCached(x, y);
    }
    if (level < 4 || level > 6 || !std::isfinite(x) || !std::isfinite(y)) {
        throw std::invalid_argument("hierarchy requires finite coordinates and graph levels3..6");
    }
    const auto canonical = [](const double value) {
        const double result = std::fmod(value, 32.0);
        return result < 0 ? result + 32.0 : result;
    };
    x = canonical(x);
    y = canonical(y);
    const auto resolution = uint32_t{1} << (level + 2);
    const Cell cell = GetCell(x, y, resolution);
    const auto entry = level == 4 ? memoPrimitive4(cell.x, cell.y)
        : level == 5 ? memoPrimitive5(cell.x, cell.y) : memoPrimitive6(cell.x, cell.y);
    const Point2D point(x, y);
    double numerator = 0.0;
    double denominator = 0.0;
    for (uint32_t i = 0; i < 5; ++i) {
        for (uint32_t j = 0; j < 5; ++j) {
            const double weight = WyvillGalinFunction(dist(point, entry->points[i][j]), entry->radius, 3.0);
            numerator += weight * entry->preNoise[i][j];
            denominator += weight;
        }
    }
    if (denominator == 0.0) {
        throw std::runtime_error("empty hierarchy primitive blend");
    }
    return numerator / denominator;
}

template <typename I>
std::array<uint64_t, 10> Noise<I>::hierarchyEntries() const
{
    return {graph1Memo_.entries(), graph2Memo_.entries(), graph3Memo_.entries(), primitiveMemo_.entries(),
        graph4Memo_.entries(), primitive4Memo_.entries(), graph5Memo_.entries(), primitive5Memo_.entries(),
        graph6Memo_.entries(), primitive6Memo_.entries()};
}

template <typename I>
template <typename Visitor>
void Noise<I>::visitHierarchySegments(double x, double y, uint32_t level, Visitor visitor) const
{
    const auto resolution = uint32_t{1} << (level - 1);
    const Cell cell = GetCell(x, y, resolution);
    if (level == 4) {
        const auto entry = memoGraph4(cell.x, cell.y);
        const auto& edge = entry->segments[2][2][0];
        visitor(level, cell.x, cell.y, 0, edge);
        return;
    }
    if (level == 5) {
        const auto entry = memoGraph5(cell.x, cell.y);
        const auto& edge = entry->segments[2][2][0];
        visitor(level, cell.x, cell.y, 0, edge);
        return;
    }
    if (level == 6) {
        const auto entry = memoGraph6(cell.x, cell.y);
        const auto& edge = entry->segments[2][2][0];
        visitor(level, cell.x, cell.y, 0, edge);
        return;
    }
    throw std::invalid_argument("new branch visitor level must be4..6");
}

template <typename I>
double Noise<I>::evaluateTerrain(double x, double y) const
{
	assert(m_resolution >= 1 && m_resolution <= 5);

	const ConnectionStrategy connectionStrategy = ConnectionStrategy::Rivers;
	const double minSlopeLevel2 = 0.09;
	const double minSlopeLevel3 = 0.18;
	const double minSlopeLevel4 = 0.38;
	const double minSlopeLevel5 = 1.0;
	const double displacementLevel1 = m_displacement;
	const double displacementLevel2 = displacementLevel1 / 4;
	const double displacementLevel3 = displacementLevel2 / 4;

	double value = 0.0;

	// In which level 1 cell is the point (x, y)
	Cell cell1 = GetCell(x, y, 1);
	// Level 1: Points in neighboring cells
	Point2DArray<9> points1 = GenerateNeighboringPoints<9>(cell1);
	// Level 1: List of segments
	const Segment3DChainArray<7, 1> straightSegments1 = GenerateSegments(points1);
	// Subdivide segments of level 1
	Segment3DChainArray<5, 4> segments1;
	SubdivideSegments(cell1, straightSegments1, segments1);
	DisplaceSegments(displacementLevel1, cell1, segments1);

	if (m_resolution == 1)
	{
		if (m_displayFunction)
		{
			value = std::max(value, ComputeColorPrimitives(x, y, cell1, points1, cell1, segments1));
		}
		
		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1));
		}
		
		if (m_displayDistance)
		{
			value = std::max(value, ComputeColorDistance(x, y, cell1, segments1));
		}

		return value;
	}

	// In which level 2 cell is the point (x, y)
	Cell cell2 = GetCell(x, y, 2);
	// Level 2: Points in neighboring cells
	Point2DArray<5> points2 = GenerateNeighboringPoints<5>(cell2);
	ReplaceNeighboringPoints(cell1, points1, cell2, points2);
	// Level 2: List of segments
	Segment3DChainArray<5, 3> segments2 = GenerateSubSegments<5, 3>(connectionStrategy, minSlopeLevel2, points2, cell1, segments1);
	DisplaceSegments(displacementLevel2, cell2, segments2);

	if (m_resolution == 2)
	{
		if (m_displayFunction)
		{
			value = std::max(value, ComputeColorPrimitives(x, y, cell2, points2, cell1, segments1, cell2, segments2));
		}

		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1, cell2, segments2, points2));
		}

		if (m_displayDistance)
		{
			value = ComputeColorDistance(x, y, cell1, segments1, cell2, segments2);
		}

		return value;
	}
	
	// In which level 3 cell is the point (x, y)
	Cell cell3 = GetCell(x, y, 4);
	// Level 3: Points in neighboring cells
	Point2DArray<5> points3 = GenerateNeighboringPoints<5>(cell3);
	ReplaceNeighboringPoints(cell2, points2, cell3, points3);
	// Level 3: List of segments
	Segment3DChainArray<5, 2> segments3 = GenerateSubSegments<5, 2>(connectionStrategy, minSlopeLevel3, points3, cell1, segments1, cell2, segments2);
	DisplaceSegments(displacementLevel3, cell3, segments3);

	if (m_resolution == 3)
	{
		if (m_displayFunction)
		{
			value = std::max(value, ComputeColorPrimitives(x, y, cell3, points3, cell1, segments1, cell2, segments2, cell3, segments3));
		}

		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1, cell2, segments2, points2, cell3, segments3, points3));
		}

		if (m_displayDistance)
		{
			value = ComputeColorDistance(x, y, cell1, segments1, cell2, segments2, cell3, segments3);
		}

		return value;
	}
	
	// In which level 4 cell is the point (x, y)
	Cell cell4 = GetCell(x, y, 8);
	// Level 4: Points in neighboring cells
	Point2DArray<5> points4 = GenerateNeighboringPoints<5>(cell4);
	ReplaceNeighboringPoints(cell3, points3, cell4, points4);
	// Level 4: List of segments
	Segment3DChainArray<5, 1> segments4 = GenerateSubSegments<5, 1>(connectionStrategy, minSlopeLevel4, points4, cell1, segments1, cell2, segments2, cell3, segments3);

	if (m_resolution == 4)
	{
		if (m_displayFunction)
		{
			value = std::max(value, ComputeColorPrimitives(x, y, cell4, points4, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4));
		}

		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1, cell2, segments2, points2, cell3, segments3, points3, cell4, segments4, points4));
		}

		if (m_displayDistance)
		{
			value = ComputeColorDistance(x, y, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4);
		}

		return value;
	}

	// In which level 5 cell is the point (x, y)
	Cell cell5 = GetCell(x, y, 16);
	// Level 5: Points in neighboring cells
	Point2DArray<5> points5 = GenerateNeighboringPoints<5>(cell5);
	ReplaceNeighboringPoints(cell4, points4, cell5, points5);
	// Level 5: List of segments
	Segment3DChainArray<5, 1> segments5 = GenerateSubSegments<5, 1>(connectionStrategy, minSlopeLevel5, points5, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4);

	if (m_resolution == 5)
	{
		if (m_displayFunction)
		{
			value = std::max(value, ComputeColorPrimitives(x, y, cell5, points5, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4, cell5, segments5));
		}

		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1, cell2, segments2, points2, cell3, segments3, points3, cell4, segments4, points4, cell5, segments5, points5));
		}

		if (m_displayDistance)
		{
			value = ComputeColorDistance(x, y, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4, cell5, segments5);
		}

		return value;
	}

	return value;
}

template <typename I>
double Noise<I>::evaluateLichtenberg(double x, double y) const
{
	assert(m_resolution >= 1 && m_resolution <= 6);

	const ConnectionStrategy connectionStrategy = ConnectionStrategy::AngleMid;
	const double displacementLevel1 = m_displacement;
	const double displacementLevel2 = displacementLevel1 / 4;
	const double displacementLevel3 = displacementLevel2 / 4;

	double value = 0.0;

	// In which level 1 cell is the point (x, y)
	Cell cell1 = GetCell(x, y, 1);
	// Level 1: Points in neighboring cells
	Point2DArray<9> points1 = GenerateNeighboringPoints<9>(cell1);
	// Level 1: List of segments
	const Segment3DChainArray<7, 1> straightSegments1 = GenerateSegments(points1);
	// Subdivide segments of level 1
	Segment3DChainArray<5, 4> segments1;
	SubdivideSegments(cell1, straightSegments1, segments1);
	DisplaceSegments(displacementLevel1, cell1, segments1);

	if (m_resolution == 1)
	{
		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1));
		}

		if (m_displayDistance)
		{
			value = std::max(value, ComputeColorDistance(x, y, cell1, segments1));
		}

		return value;
	}

	// In which level 2 cell is the point (x, y)
	Cell cell2 = GetCell(x, y, 2);
	// Level 2: Points in neighboring cells
	Point2DArray<5> points2 = GenerateNeighboringPoints<5>(cell2);
	ReplaceNeighboringPoints(cell1, points1, cell2, points2);
	// Level 2: List of segments
	Segment3DChainArray<5, 3> segments2 = GenerateSubSegments<5, 3>(connectionStrategy, 0.0, points2, cell1, segments1);
	DisplaceSegments(displacementLevel2, cell2, segments2);

	if (m_resolution == 2)
	{
		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1, cell2, segments2, points2));
		}

		if (m_displayDistance)
		{
			value = std::max(value, ComputeColorDistance(x, y, cell1, segments1, cell2, segments2));
		}

		return value;
	}

	// In which level 3 cell is the point (x, y)
	Cell cell3 = GetCell(x, y, 4);
	// Level 3: Points in neighboring cells
	Point2DArray<5> points3 = GenerateNeighboringPoints<5>(cell3);
	ReplaceNeighboringPoints(cell2, points2, cell3, points3);
	// Level 3: List of segments
	Segment3DChainArray<5, 2> segments3 = GenerateSubSegments<5, 2>(connectionStrategy, 0.0, points3, cell1, segments1, cell2, segments2);
	DisplaceSegments(displacementLevel3, cell3, segments3);

	if (m_resolution == 3)
	{
		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1, cell2, segments2, points2, cell3, segments3, points3));
		}

		if (m_displayDistance)
		{
			value = std::max(value, ComputeColorDistance(x, y, cell1, segments1, cell2, segments2, cell3, segments3));
		}

		return value;
	}

	// In which level 4 cell is the point (x, y)
	Cell cell4 = GetCell(x, y, 8);
	// Level 4: Points in neighboring cells
	Point2DArray<5> points4 = GenerateNeighboringPoints<5>(cell4);
	ReplaceNeighboringPoints(cell3, points3, cell4, points4);
	// Level 4: List of segments
	Segment3DChainArray<5, 1> segments4 = GenerateSubSegments<5, 1>(connectionStrategy, 0.0, points4, cell1, segments1, cell2, segments2, cell3, segments3);

	if (m_resolution == 4)
	{
		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1, cell2, segments2, points2, cell3, segments3, points3, cell4, segments4, points4));
		}

		if (m_displayDistance)
		{
			value = std::max(value, ComputeColorDistance(x, y, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4));
		}

		return value;
	}

	// In which level 5 cell is the point (x, y)
	Cell cell5 = GetCell(x, y, 16);
	// Level 5: Points in neighboring cells
	Point2DArray<5> points5 = GenerateNeighboringPoints<5>(cell5);
	ReplaceNeighboringPoints(cell4, points4, cell5, points5);
	// Level 5: List of segments
	Segment3DChainArray<5, 1> segments5 = GenerateSubSegments<5, 1>(connectionStrategy, 0.0, points5, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4);

	if (m_resolution == 5)
	{
		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1, cell2, segments2, points2, cell3, segments3, points3, cell4, segments4, points4, cell5, segments5, points5));
		}

		if (m_displayDistance)
		{
			value = std::max(value, ComputeColorDistance(x, y, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4, cell5, segments5));
		}

		return value;
	}

	// In which level 6 cell is the point (x, y)
	Cell cell6 = GetCell(x, y, 32);
	// Level 6: Points in neighboring cells
	Point2DArray<5> points6 = GenerateNeighboringPoints<5>(cell6);
	ReplaceNeighboringPoints(cell5, points5, cell6, points6);
	// Level 6: List of segments
	Segment3DChainArray<5, 1> segments6 = GenerateSubSegments<5, 1>(connectionStrategy, 0.0, points6, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4, cell5, segments5);

	if (m_resolution == 6)
	{
		if (m_displayPoints || m_displaySegments || m_displayGrid)
		{
			value = std::max(value, ComputeColor(x, y, cell1, segments1, points1, cell2, segments2, points2, cell3, segments3, points3, cell4, segments4, points4, cell5, segments5, points5, cell6, segments6, points6));
		}

		if (m_displayDistance)
		{
			value = std::max(value, ComputeColorDistance(x, y, cell1, segments1, cell2, segments2, cell3, segments3, cell4, segments4, cell5, segments5, cell6, segments6));
		}

		return value;
	}
	
	return value;
}

template <typename I>
template <typename T, size_t N>
std::tuple<int, int> Noise<I>::GetArrayCell(const Cell& arrCell, const Array2D<T, N>& arr, const Cell& cell) const
{
	const int i = (int(arr.size()) / 2) - arrCell.y + cell.y;
	const int j = (int(arr.front().size()) / 2) - arrCell.x + cell.x;

	return std::make_tuple(i, j);
}

template <typename I>
template <size_t N, size_t D>
double Noise<I>::NearestSegmentAndCellProjectionZ(int neighborhood, const Point2D& point, Cell& nearestSegmentCellOut, Segment3D& nearestSegmentOut, const Cell& cell, const Segment3DChainArray<N, D>& segments) const
{
	assert(neighborhood >= 0);

	// Distance to the nearest segment
	double nearestSegmentDistance = std::numeric_limits<double>::max();

	int ci, cj;
	std::tie(ci, cj) = GetArrayCell(cell, segments, GetCell(point.x, point.y, cell.resolution));
	for (int i = ci - neighborhood; i <= ci + neighborhood; i++)
	{
		for (int j = cj - neighborhood; j <= cj + neighborhood; j++)
		{
			assert(i >= 0 && static_cast<unsigned int>(i) < segments.size());
			assert(j >= 0 && static_cast<unsigned int>(j) < segments.front().size());

			for (unsigned int k = 0; k < segments[i][j].size(); k++)
			{
				Point2D c;
				const double dist = distToLineSegment(point, ProjectionZ(segments[i][j][k]), c);

				if (dist < nearestSegmentDistance)
				{
					nearestSegmentDistance = dist;
					nearestSegmentOut = segments[i][j][k];

					nearestSegmentCellOut.x = i;
					nearestSegmentCellOut.y = j;
					nearestSegmentCellOut.resolution = cell.resolution;
				}
			}
		}
	}

	return nearestSegmentDistance;
}

template <typename I>
template <size_t N, size_t D, typename ...Tail>
double Noise<I>::NearestSegmentAndCellProjectionZ(int neighborhood, const Point2D& point, Cell& nearestSegmentCellOut, Segment3D& nearestSegmentOut, const Cell& cell, const Segment3DChainArray<N, D>& segments, Tail&&... tail) const
{
	assert(neighborhood >= 0);

	// Nearest segment in the sub resolutions
	Cell nearestSubSegmentCell;
	Segment3D nearestSubSegment;
	const double nearestSubSegmentDistance = NearestSegmentAndCellProjectionZ(neighborhood, point, nearestSubSegmentCell, nearestSubSegment, std::forward<Tail>(tail)...);

	// Nearest segment in the current resolution
	double nearestSegmentDistance = NearestSegmentAndCellProjectionZ(neighborhood, point, nearestSegmentCellOut, nearestSegmentOut, cell, segments);

	if (nearestSubSegmentDistance < nearestSegmentDistance)
	{
		nearestSegmentDistance = nearestSubSegmentDistance;
		nearestSegmentOut = nearestSubSegment;
		nearestSegmentCellOut = nearestSubSegmentCell;
	}

	return nearestSegmentDistance;
}

template <typename I>
template <size_t N, size_t D>
double Noise<I>::NearestSegmentProjectionZ(int neighborhood, const Point2D& point, Segment3D& nearestSegmentOut, const Cell& cell, const Segment3DChainArray<N, D>& segments) const
{
	Cell placeholderCell;
	return NearestSegmentAndCellProjectionZ(neighborhood, point, placeholderCell, nearestSegmentOut, cell, segments);
}

template <typename I>
template <size_t N, size_t D, typename ...Tail>
double Noise<I>::NearestSegmentProjectionZ(int neighborhood, const Point2D& point, Segment3D& nearestSegmentOut, const Cell& cell, const Segment3DChainArray<N, D>& segments, Tail&&... tail) const
{
	Cell placeholderCell;
	return NearestSegmentAndCellProjectionZ(neighborhood, point, placeholderCell, nearestSegmentOut, cell, segments, std::forward<Tail>(tail)...);
}

template <typename I>
template <size_t N>
int Noise<I>::SegmentsEndingInP(const Cell& cell, const Segment3DChainArray<N, 1>& segments, const Point3D& point, Segment3D& lastSegmentEndingInP) const
{
	int numberSegmentEndingInP = 0;

	// In which cell of segments is point
	int ck, cl;
	std::tie(ck, cl) = GetArrayCell(cell, segments, GetCell(point.x, point.y, cell.resolution));
	for (int k = ck - 1; k <= ck + 1; k++)
	{
		for (int l = cl - 1; l <= cl + 1; l++)
		{
			assert(k >= 0 && static_cast<unsigned int>(k) < segments.size());
			assert(l >= 0 && static_cast<unsigned int>(l) < segments.front().size());

			// If the segment's length is more than 0
			if (segments[k][l][0].a != segments[k][l][0].b)
			{
				if (segments[k][l][0].b == point)
				{
					numberSegmentEndingInP++;
					lastSegmentEndingInP = segments[k][l][0];
				}
			}
		}
	}

	return numberSegmentEndingInP;
}

template <typename I>
template <size_t N>
int Noise<I>::SegmentsStartingInP(const Cell& cell, const Segment3DChainArray<N, 1>& segments, const Point3D& point, Segment3D& lastSegmentStartingInP) const
{
	int numberStartingInP = 0;

	// In which cell of segments is B
	int m, n;
	std::tie(m, n) = GetArrayCell(cell, segments, GetCell(point.x, point.y, cell.resolution));

	assert(m >= 0 && static_cast<unsigned int>(m) < segments.size());
	assert(n >= 0 && static_cast<unsigned int>(n) < segments.front().size());

	// If the segment's length is more than 0
	if (segments[m][n][0].a != segments[m][n][0].b)
	{
		if (segments[m][n][0].a == point)
		{
			numberStartingInP++;
			lastSegmentStartingInP = segments[m][n][0];
		}
	}

	return numberStartingInP;
}

template <typename I>
template <size_t N>
typename Noise<I>::template Point2DArray<N> Noise<I>::GenerateNeighboringPoints(const Cell& cell) const
{
	Point2DArray<N> points;

	// Exploring neighboring cells
	for (unsigned int i = 0; i < points.size(); i++)
	{
		for (unsigned int j = 0; j < points[i].size(); j++)
		{
			const int x = cell.x + j - int(points[i].size()) / 2;
			const int y = cell.y + i - int(points.size()) / 2;

            const int32_t period = 32 * cell.resolution;
            const int32_t wrappedX = robust_mod(x, period);
            const int32_t wrappedY = robust_mod(y, period);
            const Point2D canonical = GeneratePointCached(wrappedX, wrappedY);
            const Point2D p = (canonical + Point2D(x - wrappedX, y - wrappedY)) / cell.resolution;

			// Bias the random generator to repulse the points outside the domain
			if (InsideDomain(p))
			{
				points[i][j] = p;
			}
			else
			{
				// Furthest point in the cell (could be improved with topRight and bottom Left)
				const Point2D topLeft(double(x) / cell.resolution, double(y) / cell.resolution);
				const Point2D bottomRight(double(x + 1) / cell.resolution, double(y + 1) / cell.resolution);

				if (DistToDomain(topLeft) < DistToDomain(bottomRight))
				{
					points[i][j] = bottomRight;
				}
				else
				{
					points[i][j] = topLeft;
				}
			}
		}
	}

	return points;
}

template <typename I>
template <size_t N, size_t M>
void Noise<I>::ReplaceNeighboringPoints(const Cell& cell, const Point2DArray<M>& points, const Cell& subCell, Point2DArray<N>& subPoints) const
{
	// Ensure that there is enough points around to replace sub-points
	static_assert(M >= (2 * ((N + 1) / 4) + 1), "Not enough points in the vicinity to replace the sub points.");
	
	// Number of cells (or points) to consider in the upper resolution
	int pointsUpRes = 2 * ((N + 1) / 4) + 1;
	// Offset to iterate over points only using the pointsUpRes cells in the center
	int offset = (M - pointsUpRes) / 2;
	// Replace sub-points by the already existing points
	for (unsigned int i = offset; i < points.size() - offset; ++i)
	{
		for (unsigned int j = offset; j < points[i].size() - offset; ++j)
		{
			int k, l;
			std::tie(k, l) = GetArrayCell(subCell, subPoints, GetCell(points[i][j].x, points[i][j].y, subCell.resolution));

			if (k >= 0 && static_cast<unsigned int>(k) < subPoints.size() && l >= 0 && static_cast<unsigned int>(l) < subPoints.front().size())
			{
				subPoints[k][l] = points[i][j];
			}
		}
	}
}

template <typename I>
template <size_t N>
typename Noise<I>::template DoubleArray<N> Noise<I>::ComputeElevations(const Point2DArray<N>& points) const
{
	DoubleArray<N> elevations;

	for (unsigned int i = 0; i < elevations.size(); i++)
	{
		for (unsigned int j = 0; j < elevations[i].size(); j++)
		{
			elevations[i][j] = EvaluateControlFunction(points[i][j]);
		}
	}

	return elevations;
}

template <typename I>
template <size_t N>
typename Noise<I>::template Segment3DChainArray<N - 2 , 1> Noise<I>::GenerateSegments(const Point2DArray<N>& points) const
{
	static_assert(N > 0, "Not enough points");

	const DoubleArray<N> elevations = ComputeElevations<N>(points);

	Segment3DChainArray<N - 2, 1> segments;
	for (unsigned int i = 1; i < points.size() - 1; i++)
	{
		for (unsigned int j = 1; j < points[i].size() - 1; j++)
		{
			// Find the lowest neighbor
			double lowestNeighborElevation = std::numeric_limits<double>::max();
			int lowestNeighborI = i;
			int lowestNeighborJ = j;

			for (unsigned int k = i - 1; k <= i + 1; k++)
			{
				for (unsigned int l = j - 1; l <= j + 1; l++)
				{
					if (elevations[k][l] < lowestNeighborElevation)
					{
						lowestNeighborElevation = elevations[k][l];
						lowestNeighborI = k;
						lowestNeighborJ = l;
					}
				}
			}
			
			const Point3D startingPoint(points[i][j].x, points[i][j].y, elevations[i][j]);
			const Point3D endingPoint(points[lowestNeighborI][lowestNeighborJ].x, points[lowestNeighborI][lowestNeighborJ].y, lowestNeighborElevation);

			// Check if the points are inside the domain
			if (InsideDomain(startingPoint) && InsideDomain(endingPoint))
			{
				// Both points are in the domain, we keep the segment
				segments[i - 1][j - 1][0] = Segment3D(startingPoint, endingPoint);
			}
			else
			{
				// If one of the two points is outside the domain
				// We discard the segment; it has a null length
				segments[i - 1][j - 1][0] = Segment3D(startingPoint, startingPoint);
			}
		}
	}

	return segments;
}

template <typename I>
template <size_t D>
void Noise<I>::SegmentChainFromPoints(const Point3D& start, const std::array<Point3D, D - 1>& midPoints, const Point3D& end, Segment3DChain<D>& outSegmentChain) const
{
	// First subdivided segment
	outSegmentChain.front().a = start;
	for (unsigned int d = 0; d < midPoints.size(); d++)
	{
		outSegmentChain[d].b = midPoints[d];
		outSegmentChain[d + 1].a = midPoints[d];
	}
	// Last subdivided segment
	outSegmentChain.back().b = end;
}

/// <summary>
/// Subdivide all segments in a Segment3DArray&lt;N&gt; in D smaller segments using an interpolation spline.
/// </summary>
/// Require a Segment3DArray&lt;N&gt; to generate a Segment3DChainArray&lt;N - 2, D&gt; because to subdivide a segment we need its predecessors and successors.
template <typename I>
template <size_t N, size_t D>
void Noise<I>::SubdivideSegments(const Cell& cell, const Segment3DChainArray<N, 1>& segments, Segment3DChainArray<N - 2, D>& subdividedSegments) const
{
	// Ensure that segments are subdivided.
	static_assert(N > 0, "Not enough segments");
	static_assert(D > 1, "Segments should be subdivided in more than 1 part");

	// Subdivide segments
	for (unsigned int i = 1; i < segments.size() - 1; i++)
	{
		for (unsigned int j = 1; j < segments[i].size() - 1; j++)
		{
			Segment3D currentSegment = segments[i][j][0];

			std::array<Point3D, D - 1> midPoints = SubdivideInPoints<D - 1>(currentSegment);

			// If the current segment's length is more than 0, we can subdivide and smooth it
			if (currentSegment.a != currentSegment.b)
			{
				// Segments ending in A
				Segment3D lastEndingInA;
				const int numberSegmentEndingInA = SegmentsEndingInP(cell, segments, currentSegment.a, lastEndingInA);

				// Segments starting in B
				Segment3D lastStartingInB;
				const int numberStartingInB = SegmentsStartingInP(cell, segments, currentSegment.b, lastStartingInB);

				if (numberSegmentEndingInA == 1 && numberStartingInB == 1)
				{
					midPoints = SubdivideCatmullRomSpline<D - 1>(lastEndingInA.a, currentSegment.a, currentSegment.b, lastStartingInB.b);
				}
				else if (numberSegmentEndingInA != 1 && numberStartingInB == 1)
				{
					Point3D fakeStartingPoint = 2.0 * currentSegment.a - currentSegment.b;
					midPoints = SubdivideCatmullRomSpline<D - 1>(fakeStartingPoint, currentSegment.a, currentSegment.b, lastStartingInB.b);
				}
				else if (numberSegmentEndingInA == 1 && numberStartingInB != 1)
				{
					Point3D fakeEndingPoint = 2.0 * currentSegment.b - currentSegment.a;
					midPoints = SubdivideCatmullRomSpline<D - 1>(lastEndingInA.a, currentSegment.a, currentSegment.b, fakeEndingPoint);
				}
			}
			
			SegmentChainFromPoints(currentSegment.a, midPoints, currentSegment.b, subdividedSegments[i - 1][j - 1]);
		}
	}
}

template <typename I>
template <size_t N, size_t D>
void Noise<I>::DisplaceSegments(double displacementFactor, const Cell& cell, Segment3DChainArray<N, D>& segments) const
{
	// Ensure that segments are subdivided.
	static_assert(D > 1, "Segments should be subdivided in more than 1 part.");

	// Subdivide segments
	for (unsigned int i = 0; i < segments.size(); i++)
	{
		for (unsigned int j = 0; j < segments[i].size(); j++)
		{
			// First point of the segment chain
			const Point2D a = ProjectionZ(segments[i][j].front().a);
			// Last point of the segment chain
			const Point2D b = ProjectionZ(segments[i][j].back().b);

			const Vec2D ab(a, b);
			const Vec3D displacementVector(rotateCCW90(ab), 0.0);

			// Generate random numbers according to the position of the first point of the segment chain
			Cell aCell = GetCell(a.x, a.y, cell.resolution);
			RandomGenerator generator = InitRandomGenerator(
                robust_mod(aCell.x, 32 * cell.resolution), robust_mod(aCell.y, 32 * cell.resolution));
			std::uniform_real_distribution<double> distribution(-displacementFactor, displacementFactor);

			for (unsigned int k = 0; k < segments[i][j].size() - 1; k++)
			{
				const double factor = distribution(generator);
				segments[i][j][k].b += factor * displacementVector;
				segments[i][j][k + 1].a += factor * displacementVector;
			}
		}
	}
}

template <typename I>
template <size_t N2, size_t N1, size_t D1>
void Noise<I>::CheckEnoughSegmentInVicinity(const Point2DArray<N2>& points, const Cell& cell, const Segment3DChainArray<N1, D1>& segments) const
{
	// Ensure that there is enough segments around to connect sub points
	static_assert(N1 >= (2 * ((N2 + 1) / 4) + 3), "Not enough segments in the vicinity to connect sub points.");
}

template <typename I>
template <size_t N2, size_t N1, size_t D1, typename ...Tail>
void Noise<I>::CheckEnoughSegmentInVicinity(const Point2DArray<N2>& points, const Cell& cell, const Segment3DChainArray<N1, D1>& segments, Tail&&... tail) const
{
	CheckEnoughSegmentInVicinity(points, cell, segments);
	CheckEnoughSegmentInVicinity(points, std::forward<Tail>(tail)...);
}

template <typename I>
template <size_t N, size_t D, typename ...Tail>
typename Noise<I>::template Segment3DChainArray<N , D> Noise<I>::GenerateSubSegments(const ConnectionStrategy& connectionStrategy, double minSlope, const Point2DArray<N>& points, Tail&&... tail) const
{
	// Ensure that there is enough segments around to connect sub points
	CheckEnoughSegmentInVicinity(points, std::forward<Tail>(tail)...);

	// Connect each point to the nearest segment
	Segment3DChainArray<N, D> subSegments;
	for (unsigned int i = 0; i < points.size(); i++)
	{
		for (unsigned int j = 0; j < points[i].size(); j++)
		{
			// The current point
			const Point2D point = points[i][j];

			// Find the nearest segment
			Segment3D nearestSegment;
			double nearestSegmentDist = NearestSegmentProjectionZ(1, point, nearestSegment, std::forward<Tail>(tail)...);

			const double u = pointLineSegmentProjection(point, ProjectionZ(nearestSegment));
			const Point3D nearestPointOnSegment = lerp(nearestSegment, u);

			// Compute elevation of the point on the control function
			const double elevationControlFunction = EvaluateControlFunction(point);
			// Compute elevation with a constraint on slope
			// Warning, the actual slope may change if the connection point is changed in ConnectPointToSegment
			const double elevationWithMinSlope = nearestPointOnSegment.z + minSlope * std::min(1.0, EvaluateReliefSlope(point) / 0.5) * nearestSegmentDist;

			const double elevation = std::max(elevationWithMinSlope, elevationControlFunction);

			const Point3D p(point.x, point.y, elevation);

			const Segment3DChain<D> segmentChain = ConnectPointToSegment<D>(connectionStrategy, p, nearestSegmentDist, nearestSegment);

			if (length_sq(nearestSegment) > 0.0 && InsideDomain(segmentChain.front().a) && InsideDomain(segmentChain.back().b))
			{
				subSegments[i][j] = segmentChain;
			}
			else
			{
				// Warning, in some cases, even if length_sq(nearestSegment) == 0.0, we would want to connect the point to the segment
				// It happens we a point is generated exactly on a segment. The segment starting from this point has a null length.
				subSegments[i][j] = SubdivideInSegments<D>(Segment3D(p, p));
			}
		}
	}

	return subSegments;
}

template <typename I>
template <size_t N>
double Noise<I>::ComputeColorPoints(double x, double y, const Point2DArray<N>& points, double radius) const
{
	double value = 0.0;

	const int center = int(points.size()) / 2;

	// White when near to a control point
	for (int i = center - 1; i <= center + 1; i++)
	{
		for (int j = center - 1; j <= center + 1; j++)
		{
			value = std::max(value, ComputeColorPoint(x, y, points[i][j], radius));
		}
	}

	return value;
}

template <typename I>
template <size_t N, size_t D>
double Noise<I>::ComputeColorPoints(double x, double y, const Segment3DChainArray<N, D>& segments, double radius) const
{
	double value = 0.0;

	// White when near to a segment
	for (unsigned int i = 0; i < segments.size(); i++)
	{
		for (unsigned int j = 0; j < segments[i].size(); j++)
		{
			for (unsigned int k = 0; k < segments[i][j].size(); k++)
			{
				value = std::max(value, ComputeColorPoint(x, y, ProjectionZ(segments[i][j][k].a), radius));
				value = std::max(value, ComputeColorPoint(x, y, ProjectionZ(segments[i][j][k].b), radius));
			}
		}
	}

	return value;
}

template <typename I>
template <size_t N, size_t D>
double Noise<I>::ComputeColorSegments(const Cell& cell, const Segment3DChainArray<N, D>& segments, int neighborhood, double x, double y, double radius) const
{
	double value = 0.0;

	// White when near to a segment
	Segment3D nearestSegment;
	const double nearestSegmentDistance = NearestSegmentProjectionZ(neighborhood, Point2D(x, y), nearestSegment, cell, segments);
	
	/*
	 * // Make level 1 segments thicker near to the origin of the Lichtenberg figure
	 * if (cell.resolution == 1)
	 * {
	 *     // const double d = dist(Point2D(-0.75, -1.8), Point2D(x, y));
	 *     const double d = dist(Point2D(0.0, -4.0), Point2D(x, y));
	 *     radius *= 1.0 - smootherstep(0.0, 3.0, d) / 2.0;
	 * }
	 * 
	 */

	// If the segment has a length greater than zero
	if (length_sq(nearestSegment) > 0.0)
	{
		value = ComputeColorBase(nearestSegmentDistance, radius);
	}
	
	return value;
}

template <typename I>
template <size_t N1, size_t D1, size_t N2>
double Noise<I>::ComputeColor(double x, double y, const Cell& cell, const Segment3DChainArray<N1, D1>& segments, const Point2DArray<N2>& points) const
{
	double value = 0.0;

	const double radius = 1.0 / (26 * std::exp(0.085 * cell.resolution));

	if (m_displayPoints)
	{
		value = std::max(value, ComputeColorPoints(x, y, points, radius));
		value = std::max(value, ComputeColorPoints(x, y, segments, radius / 2.0));
	}

	if (m_displaySegments)
	{
		value = std::max(value, ComputeColorSegments(cell, segments, 2, x, y, radius / 4.0));
	}

	if (m_displayGrid)
	{
		for (int i = 0; i <= cell.resolution; i++)
		{
			const double grid = double(i) / cell.resolution;
			value = std::max(value, ComputeColorGrid(x, y, grid, grid, radius / 8.0));
		}
	}

	return value;
}

template <typename I>
template <size_t N1, size_t D1, size_t N2, typename ...Tail>
double Noise<I>::ComputeColor(double x, double y, const Cell& cell, const Segment3DChainArray<N1, D1>& segments, const Point2DArray<N2>& points, Tail&&... tail) const
{
	const double valueCurrentLevel = ComputeColor(x, y, cell, segments, points);
	const double valueTail = ComputeColor(x, y, std::forward<Tail>(tail)...);

	return std::max(valueCurrentLevel, valueTail);
}

template <typename I>
template <size_t N, typename ...Tail>
double Noise<I>::ComputeColorPrimitives(double x, double y, const Cell& higherResCell, const Point2DArray<N>& higherResPoints, Tail&&... tail) const
{
	const Point2D point(x, y);

	// Generate higher resolution points, which are going to be the centers of primitives
	Cell highestResCell = higherResCell;
	Point2DArray<N> highestResPoints = higherResPoints;
	for (int i = 0; i < m_primitivesResolutionSteps; i++)
	{
		Cell newCell = GetCell(x, y, 2 * highestResCell.resolution);
		Point2DArray<N> newPoints = GenerateNeighboringPoints<N>(newCell);
		ReplaceNeighboringPoints(highestResCell, highestResPoints, newCell, newPoints);

		highestResCell = newCell;
		highestResPoints = newPoints;
	}

	// Radius of primitives
	const double R = 2.0 / highestResCell.resolution;
	// Power to the Wyvill-Galin function
	const double P = 3.0;

	// Numerator and denominator used to compute the blend of primitives
	double numerator = 0.0;
	double denominator = 0.0;

	for (unsigned int i = 0; i < highestResPoints.size(); i++)
	{
		for (unsigned int j = 0; j < highestResPoints[i].size(); j++)
		{
			// Nearest segment to points[i][j] and nearest point on this segment
			Cell primitiveNearestSegmentCell;
			Segment3D primitiveNearestSegment;
			const double distancePrimitiveCenter = NearestSegmentAndCellProjectionZ(1, highestResPoints[i][j], primitiveNearestSegmentCell, primitiveNearestSegment, std::forward<Tail>(tail)...);
			double uPrimitive = pointLineSegmentProjection(highestResPoints[i][j], ProjectionZ(primitiveNearestSegment));

			double distancePrimitive = dist(point, highestResPoints[i][j]);

			const double alphaPrimitive = WyvillGalinFunction(distancePrimitive, R, P);
			const double nearestPointOnSegmentHeight = lerp(primitiveNearestSegment.a.z, primitiveNearestSegment.b.z, uPrimitive);

			// Adaptive slope depending on the mountain height
			const double controlFunctionMinimum = ControlFunctionMinimum();
			const double controlFunctionMaximum = ControlFunctionMaximum();
			const double adaptiveSlope = EvaluateReliefSlope(highestResPoints[i][j]);

			// Noise
			const double amplitudeMax = m_noiseAmplitudeProportion * (controlFunctionMaximum - controlFunctionMinimum) / higherResCell.resolution;
			const double periodPerCell = 4.0;
			const double terrainSizeX = m_noiseBottomRight.x - m_noiseTopLeft.x;
			const double terrainSizeY = m_noiseBottomRight.y - m_noiseTopLeft.y;
			const double higherResCellSize = std::max(terrainSizeX, terrainSizeY) / higherResCell.resolution;
			const double highestResCellSizeX = terrainSizeX / highestResCell.resolution;
			const double highestResCellSizeY = terrainSizeY / highestResCell.resolution;
			const double amplitude = amplitudeMax * smootherstep(0.0, higherResCellSize / 4.0, distancePrimitiveCenter);
			const double wavelengthX = highestResCellSizeX / periodPerCell;
			const double wavelengthY = highestResCellSizeY / periodPerCell;
			const double noise = amplitude * Perlin(x / wavelengthX, y / wavelengthY)
							   + 0.5 * amplitude * Perlin(x / (2.0 * wavelengthX), y / (2.0 * wavelengthY))
							   + 0.25 * amplitude * Perlin(x / (4.0 * wavelengthX), y / (4.0 * wavelengthY));

			// Final elevation
			const double elevation = nearestPointOnSegmentHeight + adaptiveSlope * distancePrimitiveCenter + noise;

			numerator += alphaPrimitive * elevation;
			denominator += alphaPrimitive;
		}
	}

	// denominator shouldn't be equal to zero if there is enough primitives around the point.
	assert(denominator != 0.0);

	return numerator / denominator;
}

template <typename I>
template <typename ...Tail>
double Noise<I>::ComputeColorControlFunction(double x, double y, Tail&&... tail) const
{
	const Point2D point(x, y);

	// nearest segment
	Segment3D nearestSegment;
	const double d = NearestSegmentProjectionZ(1, point, nearestSegment, std::forward<Tail>(tail)...);

	double value;

	if (d < (1.0 / 32.0))
	{
		const double u = pointLineSegmentProjection(point, ProjectionZ(nearestSegment));
		// Elevation of the nearest point
		value = lerp(nearestSegment.a.z, nearestSegment.b.z, u);
	}
	else
	{
		value = EvaluateControlFunction(point);
	}

	return value;
}

template <typename I>
template <typename ... Tail>
double Noise<I>::ComputeColorDistance(double x, double y, Tail&&... tail) const
{
	const Point2D point(x, y);

	// nearest segment
	Segment3D nearestSegment;
	return NearestSegmentProjectionZ(1, point, nearestSegment, std::forward<Tail>(tail)...);
}

#endif // NOISE_H
