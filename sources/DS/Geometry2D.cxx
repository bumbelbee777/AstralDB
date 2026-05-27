#include <DS/Geometry2D.hxx>

#include <DS/JSON.hxx>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <unordered_set>
#include <map>
#include <sstream>
#include <unordered_map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace AstralDB {
namespace DS {
namespace Geometry2D {

double RingAreaSigned(const Ring &R);
Vec2 RingCentroid(const Ring &R);
bool PointInPolygon(const Vec2 &P, const Polygon &Poly);
std::optional<Polygon> NormalizePolygon(Polygon P);

namespace {

constexpr double kEps = 1e-12;
constexpr double kQuantGrid = 1e9;

struct Edge {
	std::uint64_t A = 0;
	std::uint64_t B = 0;
};

struct Tri {
	Vec2 A, B, C;
};

std::string Trim(std::string_view V) {
	size_t L = 0, R = V.size();
	while(L < R && std::isspace(static_cast<unsigned char>(V[L])))
		++L;
	while(R > L && std::isspace(static_cast<unsigned char>(V[R - 1])))
		--R;
	return std::string(V.substr(L, R - L));
}

bool StartsWith(std::string_view S, std::string_view P) {
	return S.size() >= P.size() && S.substr(0, P.size()) == P;
}

std::optional<Vec2> ParseCoordPair(const JSON &J) {
	if(!J.IsArray() || J.AsArray().size() < 2)
		return std::nullopt;
	if(!J.AsArray()[0].IsNumber() || !J.AsArray()[1].IsNumber())
		return std::nullopt;
	return Vec2{J.AsArray()[0].AsNumber(), J.AsArray()[1].AsNumber()};
}

std::optional<Ring> RingFromJsonArray(const JSONArray &Arr) {
	Ring R;
	for(const auto &Pt : Arr) {
		const auto V = ParseCoordPair(Pt);
		if(!V)
			return std::nullopt;
		R.Vertices.push_back(*V);
	}
	return R.Vertices.size() >= 3 ? std::optional<Ring>(std::move(R)) : std::nullopt;
}

void CloseRingIfNeeded(Ring &R) {
	if(R.Vertices.size() < 2)
		return;
	const auto &F = R.Vertices.front();
	const auto &L = R.Vertices.back();
	if(std::abs(F.X - L.X) < kEps && std::abs(F.Y - L.Y) < kEps)
		R.Vertices.pop_back();
}

void EnsureClosedRing(Ring &R) {
	if(R.Vertices.size() < 3)
		return;
	const auto &F = R.Vertices.front();
	const auto &L = R.Vertices.back();
	if(std::abs(F.X - L.X) > kEps || std::abs(F.Y - L.Y) > kEps)
		R.Vertices.push_back(F);
}

double Cross(const Vec2 &O, const Vec2 &A, const Vec2 &B) {
	return (A.X - O.X) * (B.Y - O.Y) - (A.Y - O.Y) * (B.X - O.X);
}

double Dot(const Vec2 &A, const Vec2 &B) { return A.X * B.X + A.Y * B.Y; }

Vec2 Add(const Vec2 &A, const Vec2 &B) { return {A.X + B.X, A.Y + B.Y}; }
Vec2 Sub(const Vec2 &A, const Vec2 &B) { return {A.X - B.X, A.Y - B.Y}; }
Vec2 Mul(const Vec2 &A, double S) { return {A.X * S, A.Y * S}; }
double Len(const Vec2 &V) { return std::sqrt(Dot(V, V)); }
Vec2 Norm(const Vec2 &V) {
	const double L = Len(V);
	return L > kEps ? Vec2{V.X / L, V.Y / L} : Vec2{};
}

std::uint64_t EdgeKey(const Vec2 &A, const Vec2 &B) {
	auto Q = [](double V) { return static_cast<std::int64_t>(std::llround(V * 1e9)); };
	const std::int64_t Ax = Q(A.X), Ay = Q(A.Y), Bx = Q(B.X), By = Q(B.Y);
	const std::uint64_t K1 = (static_cast<std::uint64_t>(Ax) << 32) ^ static_cast<std::uint64_t>(Ay);
	const std::uint64_t K2 = (static_cast<std::uint64_t>(Bx) << 32) ^ static_cast<std::uint64_t>(By);
	return K1 < K2 ? (K1 << 1) ^ K2 : (K2 << 1) ^ K1;
}

bool PointOnSegment(const Vec2 &P, const Vec2 &A, const Vec2 &B) {
	if(std::abs(Cross(A, B, P)) > 1e-8)
		return false;
	const double DotAB = Dot(Sub(B, A), Sub(P, A));
	return DotAB >= -kEps && DotAB <= Dot(Sub(B, A), Sub(B, A)) + kEps;
}

bool SegIntersect(const Vec2 &P1, const Vec2 &P2, const Vec2 &Q1, const Vec2 &Q2) {
	const double D1 = Cross(P1, P2, Q1);
	const double D2 = Cross(P1, P2, Q2);
	const double D3 = Cross(Q1, Q2, P1);
	const double D4 = Cross(Q1, Q2, P2);
	if(((D1 > kEps && D2 < -kEps) || (D1 < -kEps && D2 > kEps)) && ((D3 > kEps && D4 < -kEps) || (D3 < -kEps && D4 > kEps)))
		return true;
	if(PointOnSegment(Q1, P1, P2) || PointOnSegment(Q2, P1, P2) || PointOnSegment(P1, Q1, Q2)
	   || PointOnSegment(P2, Q1, Q2))
		return true;
	return false;
}

bool RingSelfIntersects(const Ring &R) {
	const size_t N = R.Vertices.size();
	if(N < 4)
		return false;
	for(size_t I = 0; I < N; ++I) {
		const size_t I2 = (I + 1) % N;
		for(size_t J = I + 1; J < N; ++J) {
			const size_t J2 = (J + 1) % N;
			if(I2 == J || I == J2)
				continue;
			if(SegIntersect(R.Vertices[I], R.Vertices[I2], R.Vertices[J], R.Vertices[J2]))
				return true;
		}
	}
	return false;
}

bool PointInTriangle(const Vec2 &P, const Vec2 &A, const Vec2 &B, const Vec2 &C) {
	const double S1 = Cross(A, B, P);
	const double S2 = Cross(B, C, P);
	const double S3 = Cross(C, A, P);
	const bool HasNeg = (S1 < -kEps) || (S2 < -kEps) || (S3 < -kEps);
	const bool HasPos = (S1 > kEps) || (S2 > kEps) || (S3 > kEps);
	return !(HasNeg && HasPos);
}

std::vector<Tri> EarClipTriangulate(Ring RingIn) {
	CloseRingIfNeeded(RingIn);
	std::vector<Tri> Out;
	auto &V = RingIn.Vertices;
	if(V.size() < 3)
		return Out;
	double Area2 = 0.0;
	for(size_t I = 0; I < V.size(); ++I) {
		const size_t J = (I + 1) % V.size();
		Area2 += V[I].X * V[J].Y - V[J].X * V[I].Y;
	}
	const bool Ccw = Area2 > 0.0;
	std::vector<size_t> Idx(V.size());
	for(size_t I = 0; I < V.size(); ++I)
		Idx[I] = I;
	auto IsEar = [&](size_t Prev, size_t Cur, size_t Next) {
		const Vec2 &A = V[Idx[Prev]], &B = V[Idx[Cur]], &C = V[Idx[Next]];
		const double Turn = Cross(A, B, C);
		if(Ccw ? Turn <= kEps : Turn >= -kEps)
			return false;
		for(size_t K = 0; K < Idx.size(); ++K) {
			if(K == Prev || K == Cur || K == Next)
				continue;
			const Vec2 &P = V[Idx[K]];
			if(PointInTriangle(P, A, B, C))
				return false;
		}
		return true;
	};
	size_t Guard = 0;
	while(Idx.size() > 3 && Guard < Idx.size() * Idx.size()) {
		bool Cut = false;
		for(size_t I = 0; I < Idx.size(); ++I) {
			const size_t Prev = (I + Idx.size() - 1) % Idx.size();
			const size_t Next = (I + 1) % Idx.size();
			if(!IsEar(Prev, I, Next))
				continue;
			Out.push_back({V[Idx[Prev]], V[Idx[I]], V[Idx[Next]]});
			Idx.erase(Idx.begin() + static_cast<std::ptrdiff_t>(I));
			Cut = true;
			break;
		}
		if(!Cut)
			++Guard;
		else
			Guard = 0;
	}
	if(Idx.size() == 3)
		Out.push_back({V[Idx[0]], V[Idx[1]], V[Idx[2]]});
	if(Out.empty() && V.size() >= 3) {
		for(size_t I = 1; I + 1 < V.size(); ++I)
			Out.push_back({V[0], V[I], V[I + 1]});
	}
	return Out;
}

std::vector<Tri> TriangulatePolygon(const Polygon &P) {
	std::vector<Tri> Tris = EarClipTriangulate(P.Outer);
	for(const auto &Hole : P.Holes) {
		const auto HoleTris = EarClipTriangulate(Hole);
		for(const auto &T : HoleTris) {
			Tris.push_back({T.A, T.C, T.B});
		}
	}
	return Tris;
}

Vec2 TriCentroid(const Tri &T) {
	return {(T.A.X + T.B.X + T.C.X) / 3.0, (T.A.Y + T.B.Y + T.C.Y) / 3.0};
}

Vec2 SnapVertex(const Vec2 &V) {
	return {static_cast<double>(std::llround(V.X * kQuantGrid)) / kQuantGrid,
	        static_cast<double>(std::llround(V.Y * kQuantGrid)) / kQuantGrid};
}

Tri SnapTri(const Tri &T) { return {SnapVertex(T.A), SnapVertex(T.B), SnapVertex(T.C)}; }

bool TriDegenerate(const Tri &T) { return std::abs(Cross(T.A, T.B, T.C)) <= kEps; }

Polygon RebuildFromTriangles(const std::vector<Tri> &Tris) {
	std::vector<Tri> Clean;
	Clean.reserve(Tris.size());
	for(const auto &T : Tris) {
		const Tri S = SnapTri(T);
		if(!TriDegenerate(S))
			Clean.push_back(S);
	}
	if(Clean.empty())
		return Polygon{};

	std::unordered_map<std::uint64_t, int> EdgeCount;
	for(const auto &T : Clean) {
		EdgeCount[EdgeKey(T.A, T.B)]++;
		EdgeCount[EdgeKey(T.B, T.C)]++;
		EdgeCount[EdgeKey(T.C, T.A)]++;
	}
	std::unordered_map<std::string, std::vector<std::string>> Adj;
	auto VKey = [](const Vec2 &V) {
		const auto Qx = static_cast<long long>(std::llround(V.X * kQuantGrid));
		const auto Qy = static_cast<long long>(std::llround(V.Y * kQuantGrid));
		return std::to_string(Qx) + "," + std::to_string(Qy);
	};
	auto AddUndirected = [&](const Vec2 &A, const Vec2 &B) {
		if(EdgeCount[EdgeKey(A, B)] != 1)
			return;
		const auto Ka = VKey(A);
		const auto Kb = VKey(B);
		Adj[Ka].push_back(Kb);
		Adj[Kb].push_back(Ka);
	};
	std::unordered_map<std::string, Vec2> KeyToVec;
	auto Register = [&](const Vec2 &V) { KeyToVec.emplace(VKey(V), SnapVertex(V)); };
	for(const auto &T : Clean) {
		Register(T.A);
		Register(T.B);
		Register(T.C);
		AddUndirected(T.A, T.B);
		AddUndirected(T.B, T.C);
		AddUndirected(T.C, T.A);
	}
	if(Adj.empty())
		return Polygon{};

	std::unordered_set<std::string> UsedEdge;
	auto EdgeId = [](const std::string &A, const std::string &B) {
		return A < B ? A + "|" + B : B + "|" + A;
	};

	std::vector<Ring> Loops;
	for(const auto &[StartKey, _] : Adj) {
		for(const auto &NextKey : Adj.at(StartKey)) {
			const std::string E0 = EdgeId(StartKey, NextKey);
			if(UsedEdge.contains(E0))
				continue;
			Ring Loop;
			std::string Cur = StartKey;
			std::string Prev;
			for(size_t Step = 0; Step < Adj.size() + 5; ++Step) {
				const auto CurIt = KeyToVec.find(Cur);
				if(CurIt == KeyToVec.end())
					break;
				Loop.Vertices.push_back(CurIt->second);
				const auto It = Adj.find(Cur);
				if(It == Adj.end() || It->second.empty())
					break;
				std::string Next = It->second[0];
				for(const auto &Cand : It->second) {
					if(Cand != Prev) {
						Next = Cand;
						break;
					}
				}
				UsedEdge.insert(EdgeId(Cur, Next));
				Prev = Cur;
				Cur = Next;
				if(Cur == StartKey)
					break;
			}
			CloseRingIfNeeded(Loop);
			if(Loop.Vertices.size() >= 3)
				Loops.push_back(std::move(Loop));
		}
	}
	if(Loops.empty())
		return Polygon{};

	size_t OuterIdx = 0;
	double BestArea = 0.0;
	for(size_t I = 0; I < Loops.size(); ++I) {
		const double A = std::abs(RingAreaSigned(Loops[I]));
		if(A > BestArea) {
			BestArea = A;
			OuterIdx = I;
		}
	}
	Polygon Out;
	Out.Outer = std::move(Loops[OuterIdx]);
	for(size_t I = 0; I < Loops.size(); ++I) {
		if(I != OuterIdx)
			Out.Holes.push_back(std::move(Loops[I]));
	}
	return Out;
}

std::vector<Tri> FilterTrianglesUnion(const std::vector<Tri> &Tris, const Polygon &A, const Polygon &B) {
	std::vector<Tri> Out;
	for(const auto &T : Tris) {
		const Vec2 C = TriCentroid(T);
		if(PointInPolygon(C, A) || PointInPolygon(C, B))
			Out.push_back(T);
	}
	return Out;
}

std::vector<Tri> CollectTris(const Polygon &A, const Polygon &B) {
	std::vector<Tri> All = TriangulatePolygon(A);
	const auto Bt = TriangulatePolygon(B);
	All.insert(All.end(), Bt.begin(), Bt.end());
	return All;
}

Ring OffsetRing(const Ring &R, double Dist, int Segments) {
	Ring Out;
	if(R.Vertices.size() < 3)
		return Out;
	const size_t N = R.Vertices.size();
	for(size_t I = 0; I < N; ++I) {
		const Vec2 &Prev = R.Vertices[(I + N - 1) % N];
		const Vec2 &Cur = R.Vertices[I];
		const Vec2 &Next = R.Vertices[(I + 1) % N];
		Vec2 E1 = Norm(Sub(Cur, Prev));
		Vec2 E2 = Norm(Sub(Next, Cur));
		Vec2 N1{-E1.Y, E1.X};
		Vec2 N2{-E2.Y, E2.X};
		Vec2 Bis = Norm(Add(N1, N2));
		if(Len(Bis) < kEps)
			Bis = N1;
		const double Den = Dot(Bis, N1);
		const double Off = Den > kEps ? Dist / Den : Dist;
		Out.Vertices.push_back(Add(Cur, Mul(Bis, Off)));
	}
	(void)Segments;
	EnsureClosedRing(Out);
	return Out;
}

void DouglasPeucker(const std::vector<Vec2> &Pts, size_t First, size_t Last, double Tol,
                    std::vector<bool> &Keep) {
	if(Last <= First + 1)
		return;
	double MaxD = 0.0;
	size_t Index = First;
	const Vec2 &A = Pts[First];
	const Vec2 &B = Pts[Last];
	for(size_t I = First + 1; I < Last; ++I) {
		const double D = std::abs(Cross(A, B, Pts[I])) / (Len(Sub(B, A)) + kEps);
		if(D > MaxD) {
			MaxD = D;
			Index = I;
		}
	}
	if(MaxD > Tol) {
		DouglasPeucker(Pts, First, Index, Tol, Keep);
		DouglasPeucker(Pts, Index, Last, Tol, Keep);
	} else {
		for(size_t I = First + 1; I < Last; ++I)
			Keep[I] = false;
	}
}

bool Vec2Ok(const Vec2 &V) { return std::isfinite(V.X) && std::isfinite(V.Y); }

Ring RepairRingImpl(Ring R, bool WantCcw) {
	CloseRingIfNeeded(R);
	if(R.Vertices.size() < 3)
		return R;
	Ring Clean;
	Clean.Vertices.reserve(R.Vertices.size());
	for(const auto &V : R.Vertices) {
		if(!Vec2Ok(V))
			continue;
		if(!Clean.Vertices.empty()) {
			const auto &L = Clean.Vertices.back();
			if(std::abs(L.X - V.X) < kEps && std::abs(L.Y - V.Y) < kEps)
				continue;
		}
		Clean.Vertices.push_back(V);
	}
	CloseRingIfNeeded(Clean);
	if(Clean.Vertices.size() >= 3) {
		Ring NoCol;
		NoCol.Vertices.reserve(Clean.Vertices.size());
		const size_t N = Clean.Vertices.size();
		for(size_t I = 0; I < N; ++I) {
			const Vec2 &Prev = Clean.Vertices[(I + N - 1) % N];
			const Vec2 &Cur = Clean.Vertices[I];
			const Vec2 &Next = Clean.Vertices[(I + 1) % N];
			if(std::abs(Cross(Prev, Cur, Next)) > kEps)
				NoCol.Vertices.push_back(Cur);
		}
		if(NoCol.Vertices.size() >= 3)
			Clean = std::move(NoCol);
	}
	double Area2 = 0.0;
	for(size_t I = 0; I < Clean.Vertices.size(); ++I) {
		const size_t J = (I + 1) % Clean.Vertices.size();
		Area2 += Clean.Vertices[I].X * Clean.Vertices[J].Y - Clean.Vertices[J].X * Clean.Vertices[I].Y;
	}
	const bool Ccw = Area2 > 0.0;
	if((WantCcw && !Ccw) || (!WantCcw && Ccw))
		std::reverse(Clean.Vertices.begin(), Clean.Vertices.end());
	return Clean;
}

ValidationReport ValidatePolygonImpl(const Polygon &P) {
	ValidationReport R;
	if(P.Outer.Vertices.size() < 3) {
		R.Reason = "outer ring requires at least 3 vertices";
		return R;
	}
	if(!RingWithinLimits(P.Outer)) {
		R.Reason = "outer ring exceeds vertex limit";
		return R;
	}
	for(const auto &V : P.Outer.Vertices) {
		if(!Vec2Ok(V)) {
			R.Reason = "non-finite coordinate in outer ring";
			return R;
		}
	}
	if(std::abs(RingAreaSigned(P.Outer)) < kEps) {
		R.Reason = "outer ring area is zero";
		return R;
	}
	if(RingSelfIntersects(P.Outer)) {
		R.Reason = "outer ring self-intersects";
		return R;
	}
	for(size_t Hi = 0; Hi < P.Holes.size(); ++Hi) {
		const auto &H = P.Holes[Hi];
		if(H.Vertices.size() < 3) {
			R.Reason = "hole ring requires at least 3 vertices";
			return R;
		}
		if(!RingWithinLimits(H)) {
			R.Reason = "hole ring exceeds vertex limit";
			return R;
		}
		for(const auto &V : H.Vertices) {
			if(!Vec2Ok(V)) {
				R.Reason = "non-finite coordinate in hole ring";
				return R;
			}
		}
		if(std::abs(RingAreaSigned(H)) < kEps) {
			R.Reason = "hole ring area is zero";
			return R;
		}
		if(RingSelfIntersects(H)) {
			R.Reason = "hole ring self-intersects";
			return R;
		}
		const auto C = RingCentroid(H);
		if(!PointInPolygon(C, P)) {
			R.Reason = "hole centroid lies outside outer ring";
			return R;
		}
	}
	R.Ok = true;
	return R;
}

Polygon PrepForOp(Polygon P) {
	if(auto N = NormalizePolygon(std::move(P)))
		return *N;
	return Polygon{};
}

double TolForRing(const Ring &R) {
	double Span = 0.0;
	for(const auto &V : R.Vertices) {
		Span = std::max(Span, std::abs(V.X));
		Span = std::max(Span, std::abs(V.Y));
	}
	return std::max(kEps, Span * 1e-12);
}

Ring ClipConvexHalfPlane(const Ring &Subject, const Vec2 &A, const Vec2 &B, bool KeepLeft, double Tol) {
	Ring Out;
	if(Subject.Vertices.empty())
		return Out;
	if(Subject.Vertices.size() == 1) {
		const double D = Cross(A, B, Subject.Vertices[0]);
		const bool In = KeepLeft ? D >= -Tol : D <= Tol;
		return In ? Subject : Ring{};
	}
	if(Subject.Vertices.size() == 2) {
		const Vec2 &S = Subject.Vertices[0];
		const Vec2 &E = Subject.Vertices[1];
		const double Ds = Cross(A, B, S);
		const double De = Cross(A, B, E);
		const bool SIn = KeepLeft ? Ds >= -Tol : Ds <= Tol;
		const bool EIn = KeepLeft ? De >= -Tol : De <= Tol;
		if(SIn)
			Out.Vertices.push_back(S);
		if(SIn != EIn) {
			const double Den = Ds - De;
			if(std::abs(Den) >= Tol)
				Out.Vertices.push_back(
				    {S.X + (Ds / Den) * (E.X - S.X), S.Y + (Ds / Den) * (E.Y - S.Y)});
		}
		if(EIn)
			Out.Vertices.push_back(E);
		return Out;
	}
	const size_t N = Subject.Vertices.size();
	Out.Vertices.reserve(N + 2);
	for(size_t I = 0; I < N; ++I) {
		const Vec2 &S = Subject.Vertices[I];
		const Vec2 &E = Subject.Vertices[(I + 1) % N];
		const double Ds = Cross(A, B, S);
		const double De = Cross(A, B, E);
		const bool SIn = KeepLeft ? Ds >= -Tol : Ds <= Tol;
		const bool EIn = KeepLeft ? De >= -Tol : De <= Tol;
		if(SIn)
			Out.Vertices.push_back(S);
		if(SIn != EIn) {
			const double Den = Ds - De;
			if(std::abs(Den) >= Tol) {
				const double T = Ds / Den;
				Out.Vertices.push_back({S.X + T * (E.X - S.X), S.Y + T * (E.Y - S.Y)});
			}
		}
	}
	return Out;
}

Ring ClipConvexToTriangle(const Ring &Subject, const Tri &T, bool KeepInside, double Tol) {
	if(Subject.Vertices.size() < 2)
		return {};
	Ring R = Subject;
	R = ClipConvexHalfPlane(R, T.A, T.B, KeepInside, Tol);
	if(R.Vertices.size() < 2)
		return {};
	R = ClipConvexHalfPlane(R, T.B, T.C, KeepInside, Tol);
	if(R.Vertices.size() < 2)
		return {};
	R = ClipConvexHalfPlane(R, T.C, T.A, KeepInside, Tol);
	return R;
}

std::vector<Tri> FanTriangulateRing(const Ring &R) {
	std::vector<Tri> Out;
	if(R.Vertices.size() < 3)
		return Out;
	for(size_t I = 1; I + 1 < R.Vertices.size(); ++I)
		Out.push_back({R.Vertices[0], R.Vertices[I], R.Vertices[I + 1]});
	return Out;
}

std::vector<Tri> IntersectTris(const Tri &A, const Tri &B, double Tol) {
	Ring Subj{{A.A, A.B, A.C}};
	const Ring Clipped = ClipConvexToTriangle(Subj, B, true, Tol);
	return FanTriangulateRing(Clipped);
}

std::vector<Tri> FilterTrianglesDifference(const std::vector<Tri> &Tris, const Polygon &B) {
	std::vector<Tri> Out;
	for(const auto &T : Tris) {
		if(!PointInPolygon(TriCentroid(T), B))
			Out.push_back(T);
	}
	return Out;
}

bool CanExactBoolean(const Polygon &P) { return P.Holes.empty() && P.Outer.Vertices.size() >= 3; }

Polygon BooleanIntersectionExact(const Polygon &A, const Polygon &B) {
	const double Tol = std::max(TolForRing(A.Outer), TolForRing(B.Outer));
	const auto TrA = TriangulatePolygon(A);
	const auto TrB = TriangulatePolygon(B);
	std::vector<Tri> Acc;
	for(const auto &Ta : TrA) {
		for(const auto &Tb : TrB) {
			const auto Part = IntersectTris(Ta, Tb, Tol);
			Acc.insert(Acc.end(), Part.begin(), Part.end());
		}
	}
	if(Acc.empty()) {
		const auto All = CollectTris(A, B);
		for(const auto &T : All) {
			const Vec2 C = TriCentroid(T);
			if(PointInPolygon(C, A) && PointInPolygon(C, B))
				Acc.push_back(T);
		}
	}
	auto Out = RebuildFromTriangles(Acc);
	if(auto N = NormalizePolygon(std::move(Out)))
		return *N;
	return Polygon{};
}

Polygon BooleanDifferenceExact(const Polygon &A, const Polygon &B) {
	const auto TrA = TriangulatePolygon(A);
	auto Out = RebuildFromTriangles(FilterTrianglesDifference(TrA, B));
	if(auto N = NormalizePolygon(std::move(Out)))
		return *N;
	return Polygon{};
}

std::vector<Tri> FilterTrianglesStrict(const std::vector<Tri> &Tris, const Polygon &Ref, bool KeepInside) {
	std::vector<Tri> Out;
	for(const auto &T : Tris) {
		const bool A = PointInPolygon(T.A, Ref);
		const bool B = PointInPolygon(T.B, Ref);
		const bool C = PointInPolygon(T.C, Ref);
		const bool All = A && B && C;
		const bool Any = A || B || C;
		const bool Keep = KeepInside ? All : !Any;
		if(Keep)
			Out.push_back(T);
	}
	return Out;
}

} // namespace

bool IsFiniteCoord(double V) { return std::isfinite(V); }

bool RingWithinLimits(const Ring &R) { return R.Vertices.size() <= kMaxRingVertices; }

bool PolygonWithinLimits(const Polygon &P) {
	if(!RingWithinLimits(P.Outer))
		return false;
	for(const auto &H : P.Holes) {
		if(!RingWithinLimits(H))
			return false;
	}
	return true;
}

Ring RepairRing(Ring R, bool WantCcw) { return RepairRingImpl(std::move(R), WantCcw); }

Polygon RepairPolygon(Polygon P) {
	P.Outer = RepairRingImpl(std::move(P.Outer), true);
	for(auto &H : P.Holes)
		H = RepairRingImpl(std::move(H), false);
	return P;
}

ValidationReport ValidatePolygon(const Polygon &P) { return ValidatePolygonImpl(P); }

std::optional<Polygon> NormalizePolygon(Polygon P) {
	if(!PolygonWithinLimits(P))
		return std::nullopt;
	P = RepairPolygon(std::move(P));
	const auto Report = ValidatePolygonImpl(P);
	if(!Report.Ok)
		return std::nullopt;
	return P;
}

double RingAreaSigned(const Ring &R) {
	if(R.Vertices.size() < 3)
		return 0.0;
	double A = 0.0;
	for(size_t I = 0; I < R.Vertices.size(); ++I) {
		const size_t J = (I + 1) % R.Vertices.size();
		A += R.Vertices[I].X * R.Vertices[J].Y - R.Vertices[J].X * R.Vertices[I].Y;
	}
	return A * 0.5;
}

std::optional<Polygon> ParsePolygonCell(std::string_view Cell) {
	if(Cell.size() < 6 || !Cell.starts_with("P2{") || Cell.back() != '}')
		return std::nullopt;
	const auto Root = TryDecodeJSON(Cell.substr(2));
	if(!Root || !Root->IsObject())
		return std::nullopt;
	const auto &Obj = Root->AsObject();
	auto ItO = Obj.find("o");
	if(ItO == Obj.end() || !ItO->second.IsArray())
		return std::nullopt;
	Polygon P;
	const auto Outer = RingFromJsonArray(ItO->second.AsArray());
	if(!Outer)
		return std::nullopt;
	P.Outer = *Outer;
	auto ItH = Obj.find("h");
	if(ItH != Obj.end() && ItH->second.IsArray()) {
		for(const auto &HoleArr : ItH->second.AsArray()) {
			if(!HoleArr.IsArray())
				return std::nullopt;
			const auto Hole = RingFromJsonArray(HoleArr.AsArray());
			if(!Hole)
				return std::nullopt;
			P.Holes.push_back(*Hole);
		}
	}
	return NormalizePolygon(std::move(P));
}

std::string FormatPolygonCell(const Polygon &P) {
	JSONObject Root;
	JSONArray Outer;
	for(const auto &V : P.Outer.Vertices)
		Outer.emplace_back(JSONArray{JSON(V.X), JSON(V.Y)});
	Root.emplace("o", JSON(std::move(Outer)));
	if(!P.Holes.empty()) {
		JSONArray Holes;
		for(const auto &H : P.Holes) {
			JSONArray Hole;
			for(const auto &V : H.Vertices)
				Hole.emplace_back(JSONArray{JSON(V.X), JSON(V.Y)});
			Holes.emplace_back(JSON(std::move(Hole)));
		}
		Root.emplace("h", JSON(std::move(Holes)));
	}
	return "P2" + SerializeJSON(JSON(std::move(Root)));
}

std::optional<Polygon> ParseWktPolygon(std::string_view Wkt) {
	std::string U = Trim(Wkt);
	for(char &C : U)
		C = static_cast<char>(std::toupper(static_cast<unsigned char>(C)));
	if(!StartsWith(U, "POLYGON"))
		return std::nullopt;
	const size_t L = U.find('(');
	const size_t R = U.rfind(')');
	if(L == std::string::npos || R <= L)
		return std::nullopt;
	std::string Inner = U.substr(L + 1, R - L - 1);
	while(!Inner.empty() && Inner.front() == '(')
		Inner.erase(Inner.begin());
	while(!Inner.empty() && Inner.back() == ')')
		Inner.pop_back();
	Polygon P;
	std::vector<std::string> Rings;
	if(Inner.find('(') == std::string::npos) {
		Rings.push_back(Inner);
	} else {
		int Depth = 0;
		std::string CurRing;
		for(char C : Inner) {
			if(C == '(') {
				if(Depth == 0 && !CurRing.empty()) {
					Rings.push_back(CurRing);
					CurRing.clear();
				}
				++Depth;
				continue;
			}
			if(C == ')') {
				--Depth;
				if(Depth == 0) {
					Rings.push_back(CurRing);
					CurRing.clear();
				}
				continue;
			}
			if(Depth >= 1)
				CurRing.push_back(C);
		}
		if(!CurRing.empty())
			Rings.push_back(CurRing);
	}
	if(Rings.empty())
		return std::nullopt;
	for(size_t I = 0; I < Rings.size(); ++I) {
		Ring R;
		std::string S = Rings[I];
		for(char &C : S) {
			if(C == ',')
				C = ' ';
		}
		std::istringstream Iss(S);
		double X = 0.0, Y = 0.0;
		while(Iss >> X >> Y)
			R.Vertices.push_back({X, Y});
		CloseRingIfNeeded(R);
		if(R.Vertices.size() < 3)
			return std::nullopt;
		if(I == 0)
			P.Outer = std::move(R);
		else
			P.Holes.push_back(std::move(R));
	}
	return NormalizePolygon(std::move(P));
}

std::string FormatWktPolygon(const Polygon &P) {
	std::ostringstream O;
	O << "POLYGON((";
	for(size_t I = 0; I < P.Outer.Vertices.size(); ++I) {
		if(I)
			O << ',';
		O << P.Outer.Vertices[I].X << ' ' << P.Outer.Vertices[I].Y;
	}
	if(!P.Outer.Vertices.empty())
		O << ',' << P.Outer.Vertices[0].X << ' ' << P.Outer.Vertices[0].Y;
	O << ')';
	for(const auto &H : P.Holes) {
		O << ",(";
		for(size_t I = 0; I < H.Vertices.size(); ++I) {
			if(I)
				O << ',';
			O << H.Vertices[I].X << ' ' << H.Vertices[I].Y;
		}
		if(!H.Vertices.empty())
			O << ',' << H.Vertices[0].X << ' ' << H.Vertices[0].Y;
		O << ')';
	}
	O << ')';
	return O.str();
}

std::optional<Polygon> ImportGeoJsonPolygon(std::string_view JsonText) {
	const auto Doc = TryDecodeJSON(JsonText);
	if(!Doc || !Doc->IsObject())
		return std::nullopt;
	const auto &Obj = Doc->AsObject();
	auto ItType = Obj.find("type");
	auto ItCoords = Obj.find("coordinates");
	if(ItType == Obj.end() || ItCoords == Obj.end() || !ItType->second.IsString() || !ItCoords->second.IsArray())
		return std::nullopt;
	const std::string Type = ItType->second.AsString();
	if(Type != "Polygon")
		return std::nullopt;
	const auto &RingsArr = ItCoords->second.AsArray();
	if(RingsArr.empty() || !RingsArr[0].IsArray())
		return std::nullopt;
	Polygon P;
	const auto Outer = RingFromJsonArray(RingsArr[0].AsArray());
	if(!Outer)
		return std::nullopt;
	P.Outer = *Outer;
	for(size_t I = 1; I < RingsArr.size(); ++I) {
		if(!RingsArr[I].IsArray())
			return std::nullopt;
		const auto Hole = RingFromJsonArray(RingsArr[I].AsArray());
		if(!Hole)
			return std::nullopt;
		P.Holes.push_back(*Hole);
	}
	return NormalizePolygon(std::move(P));
}

std::string ExportGeoJsonPolygon(const Polygon &P) {
	JSONObject Root;
	Root.emplace("type", JSON("Polygon"));
	JSONArray Rings;
	JSONArray Outer;
	for(const auto &V : P.Outer.Vertices)
		Outer.emplace_back(JSONArray{JSON(V.X), JSON(V.Y)});
	if(!P.Outer.Vertices.empty())
		Outer.emplace_back(JSONArray{JSON(P.Outer.Vertices[0].X), JSON(P.Outer.Vertices[0].Y)});
	Rings.emplace_back(JSON(std::move(Outer)));
	for(const auto &H : P.Holes) {
		JSONArray Hole;
		for(const auto &V : H.Vertices)
			Hole.emplace_back(JSONArray{JSON(V.X), JSON(V.Y)});
		if(!H.Vertices.empty())
			Hole.emplace_back(JSONArray{JSON(H.Vertices[0].X), JSON(H.Vertices[0].Y)});
		Rings.emplace_back(JSON(std::move(Hole)));
	}
	Root.emplace("coordinates", JSON(std::move(Rings)));
	return SerializeJSON(JSON(std::move(Root)));
}

Polygon PolygonFromRing(const std::vector<double> &FlatCoords) {
	Polygon P;
	for(size_t I = 0; I + 1 < FlatCoords.size(); I += 2) {
		if(!IsFiniteCoord(FlatCoords[I]) || !IsFiniteCoord(FlatCoords[I + 1]))
			continue;
		P.Outer.Vertices.push_back({FlatCoords[I], FlatCoords[I + 1]});
	}
	CloseRingIfNeeded(P.Outer);
	if(auto N = NormalizePolygon(std::move(P)))
		return *N;
	return Polygon{};
}

Bbox2D ComputeBbox(const Polygon &P) {
	Bbox2D B;
	auto Acc = [&](const Ring &R) {
		for(const auto &V : R.Vertices) {
			if(!B.Valid) {
				B.MinX = B.MaxX = V.X;
				B.MinY = B.MaxY = V.Y;
				B.Valid = true;
			} else {
				B.MinX = std::min(B.MinX, V.X);
				B.MinY = std::min(B.MinY, V.Y);
				B.MaxX = std::max(B.MaxX, V.X);
				B.MaxY = std::max(B.MaxY, V.Y);
			}
		}
	};
	Acc(P.Outer);
	for(const auto &H : P.Holes)
		Acc(H);
	return B;
}

double RingArea(const Ring &R) { return std::abs(RingAreaSigned(R)); }

double PolygonArea(const Polygon &P) {
	double A = RingArea(P.Outer);
	for(const auto &H : P.Holes)
		A -= RingArea(H);
	return std::max(0.0, A);
}

double RingPerimeter(const Ring &R) {
	double P = 0.0;
	for(size_t I = 0; I < R.Vertices.size(); ++I) {
		const size_t J = (I + 1) % R.Vertices.size();
		P += Len(Sub(R.Vertices[J], R.Vertices[I]));
	}
	return P;
}

double PolygonPerimeter(const Polygon &P) {
	double Pm = RingPerimeter(P.Outer);
	for(const auto &H : P.Holes)
		Pm += RingPerimeter(H);
	return Pm;
}

Vec2 RingCentroid(const Ring &R) {
	if(R.Vertices.empty())
		return {};
	double A = 0.0, Cx = 0.0, Cy = 0.0;
	for(size_t I = 0; I < R.Vertices.size(); ++I) {
		const size_t J = (I + 1) % R.Vertices.size();
		const double Cross = R.Vertices[I].X * R.Vertices[J].Y - R.Vertices[J].X * R.Vertices[I].Y;
		A += Cross;
		Cx += (R.Vertices[I].X + R.Vertices[J].X) * Cross;
		Cy += (R.Vertices[I].Y + R.Vertices[J].Y) * Cross;
	}
	if(std::abs(A) < kEps) {
		double Sx = 0.0, Sy = 0.0;
		for(const auto &V : R.Vertices) {
			Sx += V.X;
			Sy += V.Y;
		}
		const double N = static_cast<double>(R.Vertices.size());
		return {Sx / N, Sy / N};
	}
	A *= 0.5;
	return {Cx / (6.0 * A), Cy / (6.0 * A)};
}

Vec2 PolygonCentroid(const Polygon &P) { return RingCentroid(P.Outer); }

bool PointInRing(const Vec2 &P, const Ring &R) {
	if(R.Vertices.size() < 3)
		return false;
	bool Inside = false;
	for(size_t I = 0, J = R.Vertices.size() - 1; I < R.Vertices.size(); J = I++) {
		const auto &A = R.Vertices[I];
		const auto &B = R.Vertices[J];
		if(PointOnSegment(P, A, B))
			return true;
		const bool Intersect = ((A.Y > P.Y) != (B.Y > P.Y))
		                    && (P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y + kEps) + A.X);
		if(Intersect)
			Inside = !Inside;
	}
	return Inside;
}

bool PointInPolygon(const Vec2 &P, const Polygon &Poly) {
	if(!PointInRing(P, Poly.Outer))
		return false;
	for(const auto &H : Poly.Holes) {
		if(PointInRing(P, H))
			return false;
	}
	return true;
}

bool RingsIntersect(const Ring &A, const Ring &B) {
	for(size_t I = 0; I < A.Vertices.size(); ++I) {
		const size_t In = (I + 1) % A.Vertices.size();
		for(size_t J = 0; J < B.Vertices.size(); ++J) {
			const size_t Jn = (J + 1) % B.Vertices.size();
			if(SegIntersect(A.Vertices[I], A.Vertices[In], B.Vertices[J], B.Vertices[Jn]))
				return true;
		}
	}
	for(const auto &V : A.Vertices) {
		if(PointInRing(V, B))
			return true;
	}
	for(const auto &V : B.Vertices) {
		if(PointInRing(V, A))
			return true;
	}
	return false;
}

bool PolygonsIntersect(const Polygon &A, const Polygon &B) {
	const auto BA = ComputeBbox(A);
	const auto BB = ComputeBbox(B);
	if(BA.Valid && BB.Valid
	   && (BA.MaxX < BB.MinX || BA.MinX > BB.MaxX || BA.MaxY < BB.MinY || BA.MinY > BB.MaxY))
		return false;
	if(RingsIntersect(A.Outer, B.Outer))
		return true;
	for(const auto &V : A.Outer.Vertices) {
		if(PointInPolygon(V, B))
			return true;
	}
	for(const auto &V : B.Outer.Vertices) {
		if(PointInPolygon(V, A))
			return true;
	}
	return false;
}

bool PolygonContains(const Polygon &Outer, const Polygon &Inner) {
	for(const auto &V : Inner.Outer.Vertices) {
		if(!PointInPolygon(V, Outer))
			return false;
	}
	return true;
}

bool PolygonWithin(const Polygon &Inner, const Polygon &Outer) { return PolygonContains(Outer, Inner); }

Polygon Union(const Polygon &A, const Polygon &B) {
	const Polygon Pa = PrepForOp(A);
	const Polygon Pb = PrepForOp(B);
	if(Pa.Outer.Vertices.empty() || Pb.Outer.Vertices.empty())
		return Pa.Outer.Vertices.empty() ? Pb : Pa;
	if(CanExactBoolean(Pa) && CanExactBoolean(Pb)) {
		const auto I = BooleanIntersectionExact(Pa, Pb);
		const auto Ab = BooleanDifferenceExact(Pa, Pb);
		const auto Ba = BooleanDifferenceExact(Pb, Pa);
		std::vector<Tri> Acc;
		const auto AddPoly = [&](const Polygon &P) {
			if(P.Outer.Vertices.empty())
				return;
			const auto T = TriangulatePolygon(P);
			Acc.insert(Acc.end(), T.begin(), T.end());
		};
		AddPoly(Ab);
		AddPoly(Ba);
		AddPoly(I);
		if(!Acc.empty()) {
			auto Out = RebuildFromTriangles(Acc);
			if(auto N = NormalizePolygon(std::move(Out))) {
				if(PolygonArea(*N) > kEps)
					return *N;
			}
		}
	}
	const auto All = CollectTris(Pa, Pb);
	auto Out = RebuildFromTriangles(FilterTrianglesUnion(All, Pa, Pb));
	if(auto N = NormalizePolygon(std::move(Out)))
		return *N;
	return Polygon{};
}

Polygon Intersection(const Polygon &A, const Polygon &B) {
	const Polygon Pa = PrepForOp(A);
	const Polygon Pb = PrepForOp(B);
	if(Pa.Outer.Vertices.empty() || Pb.Outer.Vertices.empty())
		return Polygon{};
	if(CanExactBoolean(Pa) && CanExactBoolean(Pb))
		return BooleanIntersectionExact(Pa, Pb);
	const auto All = CollectTris(Pa, Pb);
	std::vector<Tri> Keep;
	for(const auto &T : All) {
		if(PointInPolygon(T.A, Pa) && PointInPolygon(T.B, Pa) && PointInPolygon(T.C, Pa)
		   && PointInPolygon(T.A, Pb) && PointInPolygon(T.B, Pb) && PointInPolygon(T.C, Pb))
			Keep.push_back(T);
	}
	auto Out = RebuildFromTriangles(Keep);
	if(auto N = NormalizePolygon(std::move(Out)))
		return *N;
	return Polygon{};
}

Polygon Difference(const Polygon &A, const Polygon &B) {
	const Polygon Pa = PrepForOp(A);
	const Polygon Pb = PrepForOp(B);
	if(Pa.Outer.Vertices.empty())
		return Polygon{};
	if(Pb.Outer.Vertices.empty())
		return Pa;
	if(CanExactBoolean(Pa) && CanExactBoolean(Pb))
		return BooleanDifferenceExact(Pa, Pb);
	const auto At = TriangulatePolygon(Pa);
	auto Out = RebuildFromTriangles(FilterTrianglesStrict(At, Pb, false));
	if(auto N = NormalizePolygon(std::move(Out)))
		return *N;
	return Polygon{};
}

Polygon SymDifference(const Polygon &A, const Polygon &B) {
	const auto Da = Difference(A, B);
	const auto Db = Difference(B, A);
	return Union(Da, Db);
}

bool PolygonsOverlap(const Polygon &A, const Polygon &B) {
	if(!PolygonsIntersect(A, B))
		return false;
	if(PolygonContains(A, B) || PolygonContains(B, A))
		return true;
	const double Ai = PolygonArea(Intersection(A, B));
	return Ai > 0.0 && Ai < std::min(PolygonArea(A), PolygonArea(B)) * 0.999;
}

bool PolygonsTouch(const Polygon &A, const Polygon &B) {
	if(!PolygonsIntersect(A, B))
		return false;
	const double Ai = PolygonArea(Intersection(A, B));
	return Ai <= std::min(PolygonArea(A), PolygonArea(B)) * 1e-6;
}

Polygon Buffer(const Polygon &P, double Distance, int Segments) {
	const auto Base = PrepForOp(P);
	if(Base.Outer.Vertices.empty())
		return Polygon{};
	Polygon Out;
	Out.Outer = OffsetRing(Base.Outer, Distance, Segments);
	for(const auto &H : Base.Holes)
		Out.Holes.push_back(OffsetRing(H, -Distance, Segments));
	if(auto N = NormalizePolygon(std::move(Out)))
		return *N;
	return Polygon{};
}

Polygon Simplify(const Polygon &P, double Tolerance) {
	auto SimpRing = [&](const Ring &R) {
		if(R.Vertices.size() < 3)
			return R;
		std::vector<bool> Keep(R.Vertices.size(), true);
		DouglasPeucker(R.Vertices, 0, R.Vertices.size() - 1, Tolerance, Keep);
		Ring Out;
		for(size_t I = 0; I < R.Vertices.size(); ++I) {
			if(Keep[I])
				Out.Vertices.push_back(R.Vertices[I]);
		}
		return Out.Vertices.size() >= 3 ? Out : R;
	};
	Polygon Out;
	Out.Outer = SimpRing(P.Outer);
	for(const auto &H : P.Holes)
		Out.Holes.push_back(SimpRing(H));
	if(auto N = NormalizePolygon(std::move(Out)))
		return *N;
	return Polygon{};
}

Polygon ConvexHull(const Polygon &P) {
	std::vector<Vec2> Pts = P.Outer.Vertices;
	for(const auto &H : P.Holes)
		Pts.insert(Pts.end(), H.Vertices.begin(), H.Vertices.end());
	return ConvexHullPoints(Pts);
}

Polygon ConvexHullPoints(const std::vector<Vec2> &Pts) {
	std::vector<Vec2> S = Pts;
	if(S.size() < 3) {
		Polygon P;
		P.Outer.Vertices = S;
		return P;
	}
	std::sort(S.begin(), S.end(), [](const Vec2 &A, const Vec2 &B) {
		return A.X < B.X || (A.X == B.X && A.Y < B.Y);
	});
	std::vector<Vec2> H;
	auto Build = [&](int Sign) {
		H.clear();
		for(const auto &P : S) {
			while(H.size() >= 2 && Sign * Cross(H[H.size() - 2], H.back(), P) <= kEps)
				H.pop_back();
			H.push_back(P);
		}
	};
	Build(1);
	const std::vector<Vec2> Lower = H;
	Build(-1);
	if(!H.empty())
		H.pop_back();
	H.insert(H.end(), Lower.begin(), Lower.end());
	Polygon Out;
	Out.Outer = Ring{H};
	CloseRingIfNeeded(Out.Outer);
	return Out;
}

std::vector<Tri2> Triangulate(const Polygon &P) {
	const auto Tris = TriangulatePolygon(P);
	std::vector<Tri2> Out;
	Out.reserve(Tris.size());
	for(const auto &T : Tris)
		Out.push_back({T.A, T.B, T.C});
	return Out;
}

} // namespace Geometry2D
} // namespace DS
} // namespace AstralDB
