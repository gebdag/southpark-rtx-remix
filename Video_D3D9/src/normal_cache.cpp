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
}

void NormalCache::CornerNormal(const float* a, const float* b, const float* c, int corner,
                               float creaseCos, float* out) {
    unsigned long long corners[3] = {CornerKey(a), CornerKey(b), CornerKey(c)};
    auto own = g_faceByKey.find(FaceKey(corners));
    auto fan = g_facesAt.find(corners[corner]);
    if (own == g_faceByKey.end() || fan == g_facesAt.end()) {
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        out[0] = e1[1] * e2[2] - e1[2] * e2[1];
        out[1] = e1[2] * e2[0] - e1[0] * e2[2];
        out[2] = e1[0] * e2[1] - e1[1] * e2[0];
        return;
    }
    const unsigned long long point = corners[corner];
    const std::vector<int>& around = fan->second;

    // A walk over the faces around the point, stepping only across smooth edges.
    const int kMaxFan = 64;
    int reached[kMaxFan];
    int count = 0;
    reached[count++] = own->second;
    float sum[3] = {};
    for (int next = 0; next < count; ++next) {
        const Face& f = g_faces[reached[next]];
        const float w = AngleAt(f, point);
        sum[0] += f.unit[0] * w;
        sum[1] += f.unit[1] * w;
        sum[2] += f.unit[2] * w;
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

    const float* unit = g_faces[own->second].unit;
    const float length = Length(sum);
    if (length > 1e-10f && Dot(sum, unit) >= creaseCos * length) {
        out[0] = sum[0] / length; out[1] = sum[1] / length; out[2] = sum[2] / length;
    } else {
        out[0] = unit[0]; out[1] = unit[1]; out[2] = unit[2];
    }
}
