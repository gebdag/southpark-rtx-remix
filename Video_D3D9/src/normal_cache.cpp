#include "normal_cache.h"

#include <math.h>
#include <string.h>

#include <unordered_map>
#include <vector>

namespace {

// Corners closer than this are the same point. The engine's world vertices are integer model
// positions through the same matrices, so neighbours agree to far better than this.
const float kWeldCell = 0.25f;
// A face not drawn for this many frames is dropped.
const unsigned int kForgetAfter = 120;
const unsigned int kSweepEvery = 60;

struct Face {
    float unit[3];
    float angle[3];         // at each corner, radians
    unsigned long long corners[3];
    unsigned int lastSeen;
    bool live;
    // The smoothed normal at each corner, valid while `stale` is false and the crease is the
    // one it was computed for.
    float smoothed[3][3];
    float smoothedCrease;
    bool stale;
    float p[3][3];          // the corners, world space
    float lo[2], hi[2];     // footprint in X and Z
};

std::vector<Face> g_faces;
std::vector<int> g_free;
std::unordered_map<unsigned long long, int> g_faceByKey;                 // face key -> index
std::unordered_map<unsigned long long, std::vector<int>> g_facesAt;       // corner -> faces
unsigned int g_frame = 0;

unsigned long long CornerKey(const float* p) {
    const long long x = (long long)floorf(p[0] / kWeldCell + 0.5f);
    const long long y = (long long)floorf(p[1] / kWeldCell + 0.5f);
    const long long z = (long long)floorf(p[2] / kWeldCell + 0.5f);
    return ((unsigned long long)(x & 0x1fffff) << 42) | ((unsigned long long)(y & 0x1fffff) << 21) |
           (unsigned long long)(z & 0x1fffff);
}

// The same three corners in either winding are different faces: a board drawn once per side
// is two faces with opposite normals.
unsigned long long FaceKey(const unsigned long long* c) {
    // Rotate so the smallest corner leads; rotating keeps the winding.
    int first = 0;
    for (int i = 1; i < 3; ++i) {
        if (c[i] < c[first]) {
            first = i;
        }
    }
    unsigned long long h = 1469598103934665603ull;
    for (int i = 0; i < 3; ++i) {
        h = (h ^ c[(first + i) % 3]) * 1099511628211ull;
    }
    return h;
}

float Length(const float* v) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

float Dot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The angle at `at` between the edges to `b` and `c`.
float Angle(const float* at, const float* b, const float* c) {
    const float u[3] = {b[0] - at[0], b[1] - at[1], b[2] - at[2]};
    const float v[3] = {c[0] - at[0], c[1] - at[1], c[2] - at[2]};
    const float lengths = Length(u) * Length(v);
    if (!(lengths > 1e-20f)) {
        return 0.0f;
    }
    float cosine = Dot(u, v) / lengths;
    cosine = cosine < -1.0f ? -1.0f : (cosine > 1.0f ? 1.0f : cosine);
    return acosf(cosine);
}

// Two faces around one corner share an edge there when they have another corner in common.
bool ShareEdge(const Face& f, const Face& g, unsigned long long corner) {
    for (unsigned long long a : f.corners) {
        if (a == corner) {
            continue;
        }
        for (unsigned long long b : g.corners) {
            if (a == b) {
                return true;
            }
        }
    }
    return false;
}

float AngleAt(const Face& f, unsigned long long corner) {
    for (int k = 0; k < 3; ++k) {
        if (f.corners[k] == corner) {
            return f.angle[k];
        }
    }
    return 0.0f;
}

// A face arrived at or left these points: the faces around them walk again.
void MarkAround(const unsigned long long* corners) {
    for (int k = 0; k < 3; ++k) {
        auto it = g_facesAt.find(corners[k]);
        if (it == g_facesAt.end()) {
            continue;
        }
        for (int index : it->second) {
            g_faces[index].stale = true;
        }
    }
}

void Unlink(int index) {
    Face& f = g_faces[index];
    for (unsigned long long corner : f.corners) {
        auto it = g_facesAt.find(corner);
        if (it == g_facesAt.end()) {
            continue;
        }
        std::vector<int>& list = it->second;
        for (size_t i = 0; i < list.size(); ++i) {
            if (list[i] == index) {
                list[i] = list.back();
                list.pop_back();
                break;
            }
        }
        if (list.empty()) {
            g_facesAt.erase(it);
        }
    }
    g_faceByKey.erase(FaceKey(f.corners));
    f.live = false;
    g_free.push_back(index);
    MarkAround(f.corners);
}

} // namespace

void NormalCache::BeginFrame(unsigned int frame) {
    g_frame = frame;
    if (frame % kSweepEvery != 0) {
        return;
    }
    for (size_t i = 0; i < g_faces.size(); ++i) {
        if (g_faces[i].live && frame - g_faces[i].lastSeen > kForgetAfter) {
            Unlink((int)i);
        }
    }
}

void NormalCache::AddFace(const float* a, const float* b, const float* c) {
    Face f;
    f.corners[0] = CornerKey(a);
    f.corners[1] = CornerKey(b);
    f.corners[2] = CornerKey(c);
    if (f.corners[0] == f.corners[1] || f.corners[1] == f.corners[2] || f.corners[0] == f.corners[2]) {
        return;
    }
    const unsigned long long key = FaceKey(f.corners);
    auto found = g_faceByKey.find(key);
    if (found != g_faceByKey.end()) {
        g_faces[found->second].lastSeen = g_frame;
        return;
    }
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    const float normal[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                             e1[0] * e2[1] - e1[1] * e2[0]};
    const float length = Length(normal);
    if (!(length > 1e-10f)) {
        return;
    }
    for (int k = 0; k < 3; ++k) {
        f.unit[k] = normal[k] / length;
    }
    const float* p[3] = {a, b, c};
    for (int k = 0; k < 3; ++k) {
        f.angle[k] = Angle(p[k], p[(k + 1) % 3], p[(k + 2) % 3]);
    }
    f.lastSeen = g_frame;
    f.live = true;
    f.stale = true;
    const float* corner[3] = {a, b, c};
    for (int k = 0; k < 3; ++k) {
        f.p[k][0] = corner[k][0]; f.p[k][1] = corner[k][1]; f.p[k][2] = corner[k][2];
    }
    f.lo[0] = fminf(a[0], fminf(b[0], c[0])); f.hi[0] = fmaxf(a[0], fmaxf(b[0], c[0]));
    f.lo[1] = fminf(a[2], fminf(b[2], c[2])); f.hi[1] = fmaxf(a[2], fmaxf(b[2], c[2]));
    f.smoothedCrease = 0.0f;

    int index;
    if (!g_free.empty()) {
        index = g_free.back();
        g_free.pop_back();
        g_faces[index] = f;
    } else {
        index = (int)g_faces.size();
        g_faces.push_back(f);
    }
    g_faceByKey[key] = index;
    for (unsigned long long corner : f.corners) {
        g_facesAt[corner].push_back(index);
    }
    MarkAround(f.corners);
}

namespace {

// The walk around one point of face `own`: the faces reached without crossing a sharp edge,
// angle-weighted.
void WalkCorner(int own, unsigned long long point, float creaseCos, float* out) {
    out[0] = out[1] = out[2] = 0.0f;
    auto fan = g_facesAt.find(point);
    if (fan == g_facesAt.end()) {
        return;
    }
    const std::vector<int>& around = fan->second;
    const int kMaxFan = 64;
    int reached[kMaxFan];
    int count = 0;
    reached[count++] = own;
    for (int next = 0; next < count; ++next) {
        const Face& f = g_faces[reached[next]];
        const float w = AngleAt(f, point);
        out[0] += f.unit[0] * w;
        out[1] += f.unit[1] * w;
        out[2] += f.unit[2] * w;
        for (int index : around) {
            if (count == kMaxFan) {
                break;
            }
            bool seen = false;
            for (int i = 0; i < count && !seen; ++i) {
                seen = reached[i] == index;
            }
            const Face& g = g_faces[index];
            if (!seen && Dot(f.unit, g.unit) >= creaseCos && ShareEdge(f, g, point)) {
                reached[count++] = index;
            }
        }
    }
    const float length = Length(out);
    if (!(length > 1e-10f) || Dot(out, g_faces[own].unit) < creaseCos * length) {
        out[0] = g_faces[own].unit[0];
        out[1] = g_faces[own].unit[1];
        out[2] = g_faces[own].unit[2];
    }
}

} // namespace

bool NormalCache::FaceNormals(const float* a, const float* b, const float* c, float creaseCos,
                              float out[3][3]) {
    const unsigned long long corners[3] = {CornerKey(a), CornerKey(b), CornerKey(c)};
    auto found = g_faceByKey.find(FaceKey(corners));
    if (found == g_faceByKey.end()) {
        return false;
    }
    Face& f = g_faces[found->second];
    if (f.stale || f.smoothedCrease != creaseCos) {
        for (int k = 0; k < 3; ++k) {
            WalkCorner(found->second, f.corners[k], creaseCos, f.smoothed[k]);
        }
        f.stale = false;
        f.smoothedCrease = creaseCos;
    }
    // The cached face may have been recorded starting from another corner.
    for (int k = 0; k < 3; ++k) {
        int j = 0;
        while (j < 2 && f.corners[j] != corners[k]) {
            ++j;
        }
        out[k][0] = f.smoothed[j][0];
        out[k][1] = f.smoothed[j][1];
        out[k][2] = f.smoothed[j][2];
    }
    return true;
}

namespace {

// Geometry further above a point than this is sky or cloud layer, not a roof.
const float kCoverReach = 2048.0f;
const int kMaxCoverPoints = 16;

// Where the vertical line through (x, z) meets face f, if it does.
bool HeightAt(const Face& f, float x, float z, float* y) {
    const float* a = f.p[0];
    const float* b = f.p[1];
    const float* c = f.p[2];
    const float det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
    if (fabsf(det) < 1e-6f) {
        return false;   // a wall: no footprint
    }
    const float u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det;
    const float v = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det;
    const float w = 1.0f - u - v;
    if (u < 0.0f || v < 0.0f || w < 0.0f) {
        return false;
    }
    *y = u * a[1] + v * b[1] + w * c[1];
    return true;
}

} // namespace

int NormalCache::CoverAbove(const float* points, int count) {
    count = count < kMaxCoverPoints ? count : kMaxCoverPoints;
    if (count <= 0) {
        return -1;
    }
    float lo[2] = {points[0], points[2]};
    float hi[2] = {points[0], points[2]};
    for (int i = 1; i < count; ++i) {
        lo[0] = fminf(lo[0], points[i * 3]);     hi[0] = fmaxf(hi[0], points[i * 3]);
        lo[1] = fminf(lo[1], points[i * 3 + 2]); hi[1] = fmaxf(hi[1], points[i * 3 + 2]);
    }
    bool covered[kMaxCoverPoints] = {};
    bool known = false;
    for (const Face& f : g_faces) {
        if (!f.live || f.hi[0] < lo[0] || f.lo[0] > hi[0] || f.hi[1] < lo[1] || f.lo[1] > hi[1]) {
            continue;
        }
        for (int i = 0; i < count; ++i) {
            const float* q = points + i * 3;
            float y;
            if (covered[i] || q[0] < f.lo[0] || q[0] > f.hi[0] || q[2] < f.lo[1] || q[2] > f.hi[1] ||
                !HeightAt(f, q[0], q[2], &y)) {
                continue;
            }
            known = true;
            if (y > q[1] + 1.0f && y < q[1] + kCoverReach) {
                covered[i] = true;
            }
        }
    }
    if (!known) {
        return -1;
    }
    int n = 0;
    for (int i = 0; i < count; ++i) {
        n += covered[i] ? 1 : 0;
    }
    return n;
}
