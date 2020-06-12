#include "wide_query_operations.h"

namespace fcpw {

template<int WIDTH, int DIM>
inline int Mbvh<WIDTH, DIM>::collapseSbvh(const std::shared_ptr<Sbvh<DIM>>& sbvh,
										  int sbvhNodeIndex, int parent, int depth)
{
	const SbvhNode<DIM>& sbvhNode = sbvh->flatTree[sbvhNodeIndex];
	maxDepth = std::max(depth, maxDepth);

	// create mbvh node
	MbvhNode<WIDTH, DIM> mbvhNode;
	int mbvhNodeIndex = nNodes;

	nNodes++;
	mbvhNode.parent = parent;
	flatTree.emplace_back(mbvhNode);

	if (sbvhNode.rightOffset == 0) {
		// sbvh node is a leaf node; assign mbvh node its reference indices
		for (int p = 0; p < sbvhNode.nReferences; p++) {
			flatTree[mbvhNodeIndex].child[p] = -(references[sbvhNode.start + p] + 1);
		}

		nLeafs++;

	} else {
		// sbvh node is an inner node, flatten it
		int stackPtr = 0;
		int nNodesCollapsed = 0;
		int stackSbvhNodes[WIDTH][2];
		stackSbvhNodes[stackPtr][0] = sbvhNodeIndex;
		stackSbvhNodes[stackPtr][1] = 0;

		while (stackPtr >= 0) {
			int sbvhNodeIndex = stackSbvhNodes[stackPtr][0];
			int level = stackSbvhNodes[stackPtr][1];
			stackPtr--;

			const SbvhNode<DIM>& sbvhNode = sbvh->flatTree[sbvhNodeIndex];
			if (level < maxLevel && sbvhNode.rightOffset != 0) {
				// enqueue sbvh children nodes till max level or leaf node is reached
				stackPtr++;
				stackSbvhNodes[stackPtr][0] = sbvhNodeIndex + 1;
				stackSbvhNodes[stackPtr][1] = level + 1;

				stackPtr++;
				stackSbvhNodes[stackPtr][0] = sbvhNodeIndex + sbvhNode.rightOffset;
				stackSbvhNodes[stackPtr][1] = level + 1;

			} else {
				// assign mbvh node this sbvh node's bounding box and index
				for (int i = 0; i < DIM; i++) {
					flatTree[mbvhNodeIndex].boxMin[i][nNodesCollapsed] = sbvhNode.box.pMin[i];
					flatTree[mbvhNodeIndex].boxMax[i][nNodesCollapsed] = sbvhNode.box.pMax[i];
				}

				flatTree[mbvhNodeIndex].child[nNodesCollapsed] = collapseSbvh(sbvh,
											sbvhNodeIndex, mbvhNodeIndex, depth + 1);
				nNodesCollapsed++;
			}
		}
	}

	return mbvhNodeIndex;
}

template<int WIDTH, int DIM>
inline bool Mbvh<WIDTH, DIM>::isLeafNode(const MbvhNode<WIDTH, DIM>& node) const
{
	return node.child[0] < 0;
}

template<int WIDTH, int DIM>
inline void Mbvh<WIDTH, DIM>::populateLeafNode(const MbvhNode<WIDTH, DIM>& node, int leafIndex)
{
	if (objectType == ObjectType::LineSegments) {
		// populate leaf node with line segments
		for (int w = 0; w < WIDTH; w++) {
			if (node.child[w] != maxInt) {
				int index = -node.child[w] - 1;
				const LineSegment *lineSegment = dynamic_cast<const LineSegment *>(primitives[index].get());
				int paIndex = lineSegment->soup->indices[lineSegment->index + 0];
				int pbIndex = lineSegment->soup->indices[lineSegment->index + 1];
				const Vector3& pa = lineSegment->soup->positions[paIndex];
				const Vector3& pb = lineSegment->soup->positions[pbIndex];

				for (int i = 0; i < DIM; i++) {
					leafNodes[leafIndex + 0][i][w] = pa[i];
					leafNodes[leafIndex + 1][i][w] = pb[i];
				}
			}
		}

	} else if (objectType == ObjectType::Triangles) {
		// populate leaf node with triangles
		for (int w = 0; w < WIDTH; w++) {
			if (node.child[w] != maxInt) {
				int index = -node.child[w] - 1;
				const Triangle *triangle = static_cast<const Triangle *>(primitives[index].get());
				int paIndex = triangle->soup->indices[triangle->index + 0];
				int pbIndex = triangle->soup->indices[triangle->index + 1];
				int pcIndex = triangle->soup->indices[triangle->index + 2];
				const Vector3& pa = triangle->soup->positions[paIndex];
				const Vector3& pb = triangle->soup->positions[pbIndex];
				const Vector3& pc = triangle->soup->positions[pcIndex];

				for (int i = 0; i < DIM; i++) {
					leafNodes[leafIndex + 0][i][w] = pa[i];
					leafNodes[leafIndex + 1][i][w] = pb[i];
					leafNodes[leafIndex + 2][i][w] = pc[i];
				}
			}
		}
	}
}

template<int WIDTH, int DIM>
inline void Mbvh<WIDTH, DIM>::populateLeafNodes()
{
	if (objectType != ObjectType::Generic) {
		// populate leaf nodes
		int leafIndex = 0;
		int shift = objectType == ObjectType::LineSegments ? 2 : 3;
		leafNodes.resize(nLeafs*shift);

		for (int i = 0; i < nNodes; i++) {
			MbvhNode<WIDTH, DIM>& node = flatTree[i];

			if (isLeafNode(node)) {
				populateLeafNode(node, leafIndex);
				node.leafIndex = leafIndex;
				leafIndex += shift;
			}
		}
	}
}

template<int WIDTH, int DIM>
inline Mbvh<WIDTH, DIM>::Mbvh(const std::shared_ptr<Sbvh<DIM>>& sbvh_):
primitives(sbvh_->primitives),
references(sbvh_->references),
nNodes(0),
nLeafs(0),
maxDepth(0),
maxLevel(std::log2(WIDTH)),
objectType(sbvh_->objectType)
{
#ifdef PROFILE
	PROFILE_SCOPED();
#endif

	LOG_IF(FATAL, sbvh_->leafSize != WIDTH) << "Sbvh leaf size must equal mbvh width";

	using namespace std::chrono;
	high_resolution_clock::time_point t1 = high_resolution_clock::now();

	// collapse sbvh
	collapseSbvh(sbvh_, 0, 0xfffffffc, 0);

	// populate leaf nodes if primitive type is supported
	populateLeafNodes();
	this->setNormals = false;

	// compute empty nodes
	float nEmptyLeafs = 0;
	for (int i = 0; i < nNodes; i++) {
		if (isLeafNode(flatTree[i])) {
			for (int w = 0; w < WIDTH; w++) {
				if (flatTree[i].child[w] == maxInt) {
					nEmptyLeafs += 1;
				}
			}
		}
	}

	high_resolution_clock::time_point t2 = high_resolution_clock::now();
	duration<double> timeSpan = duration_cast<duration<double>>(t2 - t1);
	std::cout << "Built " << WIDTH << "-bvh with "
			  << nNodes << " nodes, "
			  << (nEmptyLeafs/nLeafs) << " empty leafs, "
			  << nLeafs << " leaves, "
			  << maxDepth << " max depth, "
			  << primitives.size() << " primitives, "
			  << references.size() << " references in "
			  << timeSpan.count() << " seconds" << std::endl;
}

template<int WIDTH, int DIM>
inline BoundingBox<DIM> Mbvh<WIDTH, DIM>::boundingBox() const
{
	BoundingBox<DIM> box;
	if (flatTree.size() == 0) return box;

	box.pMin = enoki::hmin_inner(flatTree[0].boxMin);
	box.pMax = enoki::hmax_inner(flatTree[0].boxMax);
	return box;
}

template<int WIDTH, int DIM>
inline Vector<DIM> Mbvh<WIDTH, DIM>::centroid() const
{
	Vector<DIM> c = zeroVector<DIM>();
	int nPrimitives = (int)primitives.size();

	for (int p = 0; p < nPrimitives; p++) {
		c += primitives[p]->centroid();
	}

	return c/nPrimitives;
}

template<int WIDTH, int DIM>
inline float Mbvh<WIDTH, DIM>::surfaceArea() const
{
	float area = 0.0f;
	for (int p = 0; p < (int)primitives.size(); p++) {
		area += primitives[p]->surfaceArea();
	}

	return area;
}

template<int WIDTH, int DIM>
inline float Mbvh<WIDTH, DIM>::signedVolume() const
{
	float volume = 0.0f;
	for (int p = 0; p < (int)primitives.size(); p++) {
		volume += primitives[p]->signedVolume();
	}

	return volume;
}

template<int WIDTH, int DIM>
inline int Mbvh<WIDTH, DIM>::intersectLineSegment(const MbvhNode<WIDTH, DIM>& node, int nodeIndex,
												  Ray<DIM>& r, std::vector<Interaction<DIM>>& is,
												  bool countHits) const
{
#ifdef PROFILE
	PROFILE_SCOPED();
#endif

	// perform vectorized intersection query
	FloatP<WIDTH> d;
	VectorP<WIDTH, DIM> pt;
	FloatP<WIDTH> t;
	const VectorP<WIDTH, DIM>& pa = leafNodes[node.leafIndex + 0];
	const VectorP<WIDTH, DIM>& pb = leafNodes[node.leafIndex + 1];
	MaskP<WIDTH> mask = intersectWideLineSegment<WIDTH, DIM>(r, pa, pb, d, pt, t);

	int hits = 0;
	if (countHits) {
		// record all interactions
		for (int w = 0; w < WIDTH; w++) {
			if (node.child[w] != maxInt && mask[w]) {
				int index = -node.child[w] - 1;
				if (this->ignorePrimitive(primitives[index].get())) continue;

				const LineSegment *lineSegment = static_cast<const LineSegment *>(primitives[index].get());
				auto it = is.emplace(is.end(), Interaction<DIM>());
				it->d = d[w];
				it->p[0] = pt[0][w];
				it->p[1] = pt[1][w];
				it->p[2] = pt[2][w];
				it->uv[0] = t[w];
				it->uv[1] = -1;
				it->n = lineSegment->normal(true);
				it->nodeIndex = nodeIndex;
				it->primitive = lineSegment;
				hits++;
			}
		}

	} else {
		// determine closest primitive
		int W = maxInt;
		for (int w = 0; w < WIDTH; w++) {
			if (node.child[w] != maxInt && mask[w] && d[w] <= r.tMax) {
				int index = -node.child[w] - 1;
				if (this->ignorePrimitive(primitives[index].get())) continue;

				r.tMax = d[w];
				W = w;
			}
		}

		// update interaction
		if (W != maxInt) {
			int index = -node.child[W] - 1;
			const LineSegment *lineSegment = static_cast<const LineSegment *>(primitives[index].get());
			is[0].d = d[W];
			is[0].p[0] = pt[0][W];
			is[0].p[1] = pt[1][W];
			is[0].p[2] = pt[2][W];
			is[0].uv[0] = t[W];
			is[0].uv[1] = -1;
			is[0].n = lineSegment->normal(true);
			is[0].nodeIndex = nodeIndex;
			is[0].primitive = lineSegment;
			hits = 1;
		}
	}

	return hits;
}

template<int WIDTH, int DIM>
inline int Mbvh<WIDTH, DIM>::intersectTriangle(const MbvhNode<WIDTH, DIM>& node, int nodeIndex,
											   Ray<DIM>& r, std::vector<Interaction<DIM>>& is,
											   bool countHits) const
{
#ifdef PROFILE
	PROFILE_SCOPED();
#endif

	// perform vectorized intersection query
	FloatP<WIDTH> d;
	VectorP<WIDTH, DIM> pt;
	VectorP<WIDTH, DIM - 1> t;
	const VectorP<WIDTH, DIM>& pa = leafNodes[node.leafIndex + 0];
	const VectorP<WIDTH, DIM>& pb = leafNodes[node.leafIndex + 1];
	const VectorP<WIDTH, DIM>& pc = leafNodes[node.leafIndex + 2];
	MaskP<WIDTH> mask = intersectWideTriangle<WIDTH, DIM>(r, pa, pb, pc, d, pt, t);

	int hits = 0;
	if (countHits) {
		// record all interactions
		for (int w = 0; w < WIDTH; w++) {
			if (node.child[w] != maxInt && mask[w]) {
				int index = -node.child[w] - 1;
				if (this->ignorePrimitive(primitives[index].get())) continue;

				const Triangle *triangle = static_cast<const Triangle *>(primitives[index].get());
				auto it = is.emplace(is.end(), Interaction<DIM>());
				it->d = d[w];
				it->p[0] = pt[0][w];
				it->p[1] = pt[1][w];
				it->p[2] = pt[2][w];
				it->uv[0] = t[0][w];
				it->uv[1] = t[1][w];
				it->n = triangle->normal(true);
				it->nodeIndex = nodeIndex;
				it->primitive = triangle;
				hits++;
			}
		}

	} else {
		// determine closest primitive
		int W = maxInt;
		for (int w = 0; w < WIDTH; w++) {
			if (node.child[w] != maxInt && mask[w] && d[w] <= r.tMax) {
				int index = -node.child[w] - 1;
				if (this->ignorePrimitive(primitives[index].get())) continue;

				r.tMax = d[w];
				W = w;
			}
		}

		// update interaction
		if (W != maxInt) {
			int index = -node.child[W] - 1;
			const Triangle *triangle = static_cast<const Triangle *>(primitives[index].get());
			is[0].d = d[W];
			is[0].p[0] = pt[0][W];
			is[0].p[1] = pt[1][W];
			is[0].p[2] = pt[2][W];
			is[0].uv[0] = t[0][W];
			is[0].uv[1] = t[1][W];
			is[0].n = triangle->normal(true);
			is[0].nodeIndex = nodeIndex;
			is[0].primitive = triangle;
			hits = 1;
		}
	}

	return hits;
}

template<int WIDTH, int DIM>
inline int Mbvh<WIDTH, DIM>::intersectFromNode(Ray<DIM>& r, std::vector<Interaction<DIM>>& is,
											   int nodeStartIndex, int& nodesVisited,
											   bool checkOcclusion, bool countHits) const
{
#ifdef PROFILE
	PROFILE_SCOPED();
#endif

	// TODO: start from nodeStartIndex
	LOG_IF(FATAL, nodeStartIndex < 0 || nodeStartIndex >= nNodes) << "Start node index: "
								 << nodeStartIndex << " out of range [0, " << nNodes << ")";
	int hits = 0;
	if (!countHits) is.resize(1);
	std::vector<BvhTraversal> subtree((maxDepth + 1)*(WIDTH - 1));
	FloatP<WIDTH> tMin, tMax;

	// push root node
	subtree[0].node = 0;
	subtree[0].distance = minFloat;
	int stackPtr = 0;

	while (stackPtr >= 0) {
		// pop off the next node to work on
		int nodeIndex = subtree[stackPtr].node;
		float near = subtree[stackPtr].distance;
		stackPtr--;

		// if this node is further than the closest found intersection, continue
		if (!countHits && near > r.tMax) continue;
		const MbvhNode<WIDTH, DIM>& node(flatTree[nodeIndex]);

		if (isLeafNode(node)) {
			if (objectType != ObjectType::Generic) {
				// perform vectorized intersection query
				hits += objectType == ObjectType::LineSegments ?
						intersectLineSegment(node, nodeIndex, r, is, countHits) :
						intersectTriangle(node, nodeIndex, r, is, countHits);
				nodesVisited++;
				if (hits > 0 && checkOcclusion) return 1;

			} else {
				// primitive type does not support vectorized intersection query,
				// perform query to each primitive one by one
				for (int w = 0; w < WIDTH; w++) {
					if (node.child[w] != maxInt) {
						int index = -node.child[w] - 1;
						const std::shared_ptr<Primitive<DIM>>& prim = primitives[index];
						if (this->ignorePrimitive(prim.get())) continue;

						// check if primitive has already been seen
						bool seenPrim = false;
						int nInteractions = (int)is.size();
						for (int sp = 0; sp < nInteractions; sp++) {
							if (prim.get() == is[sp].primitive) {
								seenPrim = true;
								break;
							}
						}

						if (!seenPrim) {
							std::vector<Interaction<DIM>> cs;
							int hit = prim->intersect(r, cs, checkOcclusion, countHits);
							nodesVisited++;

							// keep the closest intersection only
							if (hit > 0) {
								hits += hit;
								if (countHits) {
									is.insert(is.end(), cs.begin(), cs.end());
									for (int sp = nInteractions; sp < (int)is.size(); sp++) {
										is[sp].nodeIndex = nodeIndex;
									}

								} else {
									r.tMax = std::min(r.tMax, cs[0].d);
									is[0] = cs[0];
									is[0].nodeIndex = nodeIndex;
								}

								if (checkOcclusion) return 1;
							}
						}
					}
				}
			}

		} else {
			// intersect ray with boxes
			MaskP<WIDTH> mask = intersectWideBox<WIDTH, DIM>(r, node.boxMin,
													node.boxMax, tMin, tMax);

			// find closest intersecting node
			int closestIndex = -1;
			float minHit = r.tMax;

			for (int w = 0; w < WIDTH; w++) {
				if (node.child[w] != maxInt && mask[w] && tMin[w] < minHit) {
					closestIndex = w;
					minHit = tMin[w];
				}
			}

			// enqueue remaining intersecting nodes first
			for (int w = 0; w < WIDTH; w++) {
				if (node.child[w] != maxInt && mask[w] && w != closestIndex) {
					stackPtr++;
					subtree[stackPtr].node = node.child[w];
					subtree[stackPtr].distance = tMin[w];
				}
			}

			// enqueue closest intersecting node
			if (closestIndex != -1) {
				stackPtr++;
				subtree[stackPtr].node = node.child[closestIndex];
				subtree[stackPtr].distance = minHit;
			}

			nodesVisited++;
		}
	}

	if (hits > 0) {
		// sort by distance and remove duplicates
		if (countHits) {
			std::sort(is.begin(), is.end(), compareInteractions<DIM>);
			is = removeDuplicates<DIM>(is);
			hits = (int)is.size();
		}

		// set normals
		if (this->setNormals) {
			for (int i = 0; i < (int)is.size(); i++) {
				is[i].n = is[i].primitive->normal(is[i].uv);
			}
		}

		return hits;
	}

	return 0;
}

template<int WIDTH, int DIM>
inline bool Mbvh<WIDTH, DIM>::findClosestPointLineSegment(const MbvhNode<WIDTH, DIM>& node,
														  int nodeIndex, BoundingSphere<DIM>& s,
														  Interaction<DIM>& i) const
{
#ifdef PROFILE
	PROFILE_SCOPED();
#endif

	// perform vectorized closest point query
	VectorP<WIDTH, DIM> pt;
	FloatP<WIDTH> t;
	const VectorP<WIDTH, DIM>& pa = leafNodes[node.leafIndex + 0];
	const VectorP<WIDTH, DIM>& pb = leafNodes[node.leafIndex + 1];
	FloatP<WIDTH> d = findClosestPointWideLineSegment<WIDTH, DIM>(s.c, pa, pb, pt, t);
	FloatP<WIDTH> d2 = d*d;

	// determine closest primitive
	int W = maxInt;
	for (int w = 0; w < WIDTH; w++) {
		if (node.child[w] != maxInt && d2[w] <= s.r2) {
			int index = -node.child[w] - 1;
			if (this->ignorePrimitive(primitives[index].get())) continue;

			s.r2 = d2[w];
			W = w;
		}
	}

	// update interaction
	if (W != maxInt) {
		int index = -node.child[W] - 1;
		const LineSegment *lineSegment = static_cast<const LineSegment *>(primitives[index].get());
		i.d = d[W];
		i.p[0] = pt[0][W];
		i.p[1] = pt[1][W];
		i.p[2] = pt[2][W];
		i.uv[0] = t[W];
		i.uv[1] = -1;
		i.nodeIndex = nodeIndex;
		i.primitive = lineSegment;

		return true;
	}

	return false;
}

template<int WIDTH, int DIM>
inline bool Mbvh<WIDTH, DIM>::findClosestPointTriangle(const MbvhNode<WIDTH, DIM>& node,
													   int nodeIndex, BoundingSphere<DIM>& s,
													   Interaction<DIM>& i) const
{
#ifdef PROFILE
	PROFILE_SCOPED();
#endif

	// perform vectorized closest point query
	VectorP<WIDTH, DIM> pt;
	VectorP<WIDTH, DIM - 1> t;
	const VectorP<WIDTH, DIM>& pa = leafNodes[node.leafIndex + 0];
	const VectorP<WIDTH, DIM>& pb = leafNodes[node.leafIndex + 1];
	const VectorP<WIDTH, DIM>& pc = leafNodes[node.leafIndex + 2];
	FloatP<WIDTH> d = findClosestPointWideTriangle<WIDTH, DIM>(s.c, pa, pb, pc, pt, t);
	FloatP<WIDTH> d2 = d*d;

	// determine closest primitive
	int W = maxInt;
	for (int w = 0; w < WIDTH; w++) {
		if (node.child[w] != maxInt && d2[w] <= s.r2) {
			int index = -node.child[w] - 1;
			if (this->ignorePrimitive(primitives[index].get())) continue;

			s.r2 = d2[w];
			W = w;
		}
	}

	// update interaction
	if (W != maxInt) {
		int index = -node.child[W] - 1;
		const Triangle *triangle = static_cast<const Triangle *>(primitives[index].get());
		i.d = d[W];
		i.p[0] = pt[0][W];
		i.p[1] = pt[1][W];
		i.p[2] = pt[2][W];
		i.uv[0] = t[0][W];
		i.uv[1] = t[1][W];
		i.nodeIndex = nodeIndex;
		i.primitive = triangle;

		return true;
	}

	return false;
}

template<int WIDTH, int DIM>
inline bool Mbvh<WIDTH, DIM>::findClosestPointFromNode(BoundingSphere<DIM>& s, Interaction<DIM>& i,
													   int nodeStartIndex, const Vector<DIM>& dirGuess,
													   int& nodesVisited) const
{
#ifdef PROFILE
	PROFILE_SCOPED();
#endif

	// TODO: start from nodeStartIndex & use direction to boundary guess
	LOG_IF(FATAL, nodeStartIndex < 0 || nodeStartIndex >= nNodes) << "Start node index: "
								 << nodeStartIndex << " out of range [0, " << nNodes << ")";
	bool notFound = true;
	std::vector<BvhTraversal> subtree((maxDepth + 1)*(WIDTH - 1));
	FloatP<WIDTH> d2Min, d2Max;

	// push root node
	subtree[0].node = 0;
	subtree[0].distance = minFloat;
	int stackPtr = 0;

	while (stackPtr >= 0) {
		// pop off the next node to work on
		int nodeIndex = subtree[stackPtr].node;
		float near = subtree[stackPtr].distance;
		stackPtr--;

		// if this node is further than the closest found primitive, continue
		if (near > s.r2) continue;
		const MbvhNode<WIDTH, DIM>& node(flatTree[nodeIndex]);

		if (isLeafNode(node)) {
			if (objectType != ObjectType::Generic) {
				// perform vectorized closest point query to triangle
				bool found = objectType == ObjectType::LineSegments ?
							 findClosestPointLineSegment(node, nodeIndex, s, i) :
							 findClosestPointTriangle(node, nodeIndex, s, i);
				if (found) notFound = false;
				nodesVisited++;

			} else {
				// primitive type does not support vectorized closest point query,
				// perform query to each primitive one by one
				for (int w = 0; w < WIDTH; w++) {
					if (node.child[w] != maxInt) {
						int index = -node.child[w] - 1;
						const std::shared_ptr<Primitive<DIM>>& prim = primitives[index];
						if (this->ignorePrimitive(prim.get())) continue;

						if (prim.get() != i.primitive) {
							Interaction<DIM> c;
							bool found = prim->findClosestPoint(s, c);
							nodesVisited++;

							// keep the closest point only
							if (found) {
								notFound = false;
								s.r2 = std::min(s.r2, c.d*c.d);
								i = c;
								i.nodeIndex = nodeIndex;
							}
						}
					}
				}
			}

		} else {
			// overlap sphere with boxes
			MaskP<WIDTH> mask = overlapWideBox<WIDTH, DIM>(s, node.boxMin,
												node.boxMax, d2Min, d2Max);

			// find closest overlapping node
			int closestIndex = -1;
			float minDist = s.r2;

			for (int w = 0; w < WIDTH; w++) {
				if (node.child[w] != maxInt && mask[w] && d2Min[w] < minDist) {
					closestIndex = w;
					minDist = d2Min[w];
				}
			}

			// enqueue remaining overlapping nodes first
			for (int w = 0; w < WIDTH; w++) {
				if (node.child[w] != maxInt && mask[w] && w != closestIndex) {
					if (this->ignoreList.size() == 0) s.r2 = std::min(s.r2, d2Max[w]);
					stackPtr++;
					subtree[stackPtr].node = node.child[w];
					subtree[stackPtr].distance = d2Min[w];
				}
			}

			// enqueue closest intersecting node
			if (closestIndex != -1) {
				if (this->ignoreList.size() == 0) s.r2 = std::min(s.r2, d2Max[closestIndex]);
				stackPtr++;
				subtree[stackPtr].node = node.child[closestIndex];
				subtree[stackPtr].distance = minDist;
			}

			nodesVisited++;
		}
	}

	if (!notFound) {
		// set normal
		if (this->setNormals) {
			i.n = i.primitive->normal(i.uv);
		}

		return true;
	}

	return false;
}

} // namespace fcpw
