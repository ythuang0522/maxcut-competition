// gen.cpp -- deterministic instance generator for the Max-Cut competition.
//
//   gen <family> <seed> <out.mc> [key=value ...]
//
// Prints one JSON line with the graph's statistics on stdout. The graphs are
// large (up to a few million edges), so they are NOT stored in git: every
// student regenerates them from the seeds in tools/manifest/public.json and
// checks the SHA-256 against checksums/scored.sha256. To make that work the
// generator uses integer arithmetic only (a splitmix64 stream, no libm, no
// floating point), so the bytes are identical on every platform and compiler.
//
// Families (all weights are positive integers; an edge appears once, u < v):
//
//   regular   n= d= [wmax=1]      random d-regular simple graph (pairing model
//                                 with repairs). Unweighted by default.
//   gnm       n= m= [wmax=100]    Erdos-Renyi G(n, m): m distinct random edges,
//                                 weights uniform in [1, wmax].
//   powerlaw  n= k= [wmax=1]      preferential attachment: each new vertex
//                                 joins k earlier ones, chosen proportional to
//                                 degree. A few hubs, many leaves.
//   tri       L= [wmax=100]       triangular lattice on an L x L torus (square
//                                 grid plus one diagonal), random weights: a
//                                 frustrated antiferromagnet -- every triangle
//                                 must leave one edge uncut.
//   geo       n= deg= [wmax=100]  random geometric graph in a square: points
//                                 within radius r are joined, heavier when
//                                 closer. deg is the target average degree.
//   community n= m= K= pin= [wmax=1]   planted partition: a fraction pin/1000
//                                 of the edges lie inside one of K hidden
//                                 communities; vertex labels are shuffled.
//
// Output format (<out.mc>):
//     n m
//     u v w         (0 <= u < v < n, integer w >= 1, one edge per line)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

using std::vector;
typedef uint64_t u64;
typedef int64_t i64;

// ------------------------------------------------------------- randomness --

struct Rng {
    u64 s;
    explicit Rng(u64 seed) : s(seed) {}
    u64 next() {                                  // splitmix64
        u64 z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // uniform in [0, k) for k < 2^32, no division, platform independent
    u64 below(u64 k) { return ((next() >> 32) * k) >> 32; }
    // uniform in [lo, hi]
    i64 range(i64 lo, i64 hi) { return lo + (i64)below((u64)(hi - lo + 1)); }
};

static u64 fnv(const std::string& s) {
    u64 h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

// -------------------------------------------------------------- the graph --

struct Edge { uint32_t u, v; i64 w; };

static inline u64 key(u64 u, u64 v) { return u < v ? (u << 32) | v : (v << 32) | u; }

struct Graph {
    uint32_t n = 0;
    vector<Edge> edges;
    std::unordered_set<u64> seen;
    bool add(uint32_t u, uint32_t v, i64 w) {
        if (u == v) return false;
        if (u > v) std::swap(u, v);
        if (!seen.insert(key(u, v)).second) return false;
        edges.push_back({u, v, w});
        return true;
    }
    bool has(uint32_t u, uint32_t v) const { return seen.count(key(u, v)) != 0; }
};

static void assign_weights(Graph& g, Rng& rng, i64 wmax) {
    if (wmax <= 1) { for (Edge& e : g.edges) e.w = 1; return; }
    for (Edge& e : g.edges) e.w = rng.range(1, wmax);
}

// Shuffle vertex labels so no family leaks structure through the numbering.
static void relabel(Graph& g, Rng& rng) {
    vector<uint32_t> perm(g.n);
    for (uint32_t i = 0; i < g.n; ++i) perm[i] = i;
    for (uint32_t i = g.n - 1; i > 0; --i) std::swap(perm[i], perm[rng.below(i + 1)]);
    for (Edge& e : g.edges) {
        e.u = perm[e.u]; e.v = perm[e.v];
        if (e.u > e.v) std::swap(e.u, e.v);
    }
}

static void finish(Graph& g) {
    std::sort(g.edges.begin(), g.edges.end(),
              [](const Edge& a, const Edge& b) { return a.u != b.u ? a.u < b.u : a.v < b.v; });
}

// --------------------------------------------------------------- families --

static Graph gen_regular(Rng& rng, uint32_t n, uint32_t d) {
    Graph g; g.n = n;
    if ((u64)n * d % 2) { std::fprintf(stderr, "n*d must be even\n"); std::exit(1); }
    vector<uint32_t> stubs((size_t)n * d);
    for (size_t i = 0; i < stubs.size(); ++i) stubs[i] = (uint32_t)(i / d);
    // pair up the stubs; put the members of bad pairs back into the pool
    vector<uint32_t> pool = stubs;
    int stall = 0;
    while (!pool.empty()) {
        for (size_t i = pool.size() - 1; i > 0; --i) std::swap(pool[i], pool[rng.below(i + 1)]);
        vector<uint32_t> bad;
        for (size_t i = 0; i + 1 < pool.size(); i += 2) {
            if (!g.add(pool[i], pool[i + 1], 1)) { bad.push_back(pool[i]); bad.push_back(pool[i + 1]); }
        }
        if (bad.size() == pool.size()) ++stall; else stall = 0;
        pool.swap(bad);
        if (stall >= 5 && !pool.empty()) {
            // the leftovers cannot pair among themselves: switch with a random
            // existing edge (a,b) and stubs (x,y): remove (a,b), add (a,x),(b,y)
            vector<uint32_t> keep;
            for (size_t i = 0; i + 1 < pool.size(); i += 2) {
                uint32_t x = pool[i], y = pool[i + 1];
                bool done = false;
                for (int t = 0; t < 200 && !done; ++t) {
                    size_t ei = rng.below(g.edges.size());
                    uint32_t a = g.edges[ei].u, b = g.edges[ei].v;
                    if (a == x || a == y || b == x || b == y) continue;
                    if (g.has(a, x) || g.has(b, y)) continue;
                    g.seen.erase(key(a, b));
                    g.edges[ei] = g.edges.back(); g.edges.pop_back();
                    g.add(a, x, 1); g.add(b, y, 1);
                    done = true;
                }
                if (!done) { keep.push_back(x); keep.push_back(y); }
            }
            pool.swap(keep);
            stall = 0;
        }
    }
    return g;
}

static Graph gen_gnm(Rng& rng, uint32_t n, u64 m) {
    Graph g; g.n = n;
    while (g.edges.size() < m) g.add((uint32_t)rng.below(n), (uint32_t)rng.below(n), 1);
    return g;
}

static Graph gen_powerlaw(Rng& rng, uint32_t n, uint32_t k) {
    Graph g; g.n = n;
    vector<uint32_t> ends;                        // every edge endpoint once: degree-proportional sampling
    // seed: a clique on k+1 vertices
    for (uint32_t u = 0; u <= k; ++u)
        for (uint32_t v = u + 1; v <= k; ++v) { g.add(u, v, 1); ends.push_back(u); ends.push_back(v); }
    for (uint32_t v = k + 1; v < n; ++v) {
        uint32_t added = 0;
        while (added < k) {
            uint32_t u = ends[rng.below(ends.size())];
            if (g.add(u, v, 1)) { ends.push_back(u); ends.push_back(v); ++added; }
        }
    }
    return g;
}

static Graph gen_tri(Rng& rng, uint32_t L) {
    (void)rng;
    Graph g; g.n = L * L;
    for (uint32_t i = 0; i < L; ++i)
        for (uint32_t j = 0; j < L; ++j) {
            uint32_t v = i * L + j;
            g.add(v, i * L + (j + 1) % L, 1);
            g.add(v, ((i + 1) % L) * L + j, 1);
            g.add(v, ((i + 1) % L) * L + (j + 1) % L, 1);
        }
    return g;
}

// Points on a 2^20 x 2^20 square; join pairs within radius r, weight grows
// with proximity: w = 1 + wmax * (r^2 - d^2) / r^2 (integer arithmetic).
static Graph gen_geo(Rng& rng, uint32_t n, uint32_t deg, i64 wmax) {
    Graph g; g.n = n;
    const i64 S = 1 << 20;
    // expected degree = n * pi * r^2 / S^2  ->  r^2 = deg * S^2 / (pi n); pi ~ 355/113
    i64 r2 = (i64)((__int128)deg * S * S * 113 / ((__int128)355 * n));
    i64 r = 1; while ((r + 1) * (r + 1) <= r2) ++r;
    vector<i64> X(n), Y(n);
    for (uint32_t i = 0; i < n; ++i) { X[i] = (i64)rng.below(S); Y[i] = (i64)rng.below(S); }
    i64 G = S / r + 1;                             // grid cells of side r
    vector<vector<uint32_t>> cell((size_t)G * G);
    for (uint32_t i = 0; i < n; ++i) cell[(size_t)(X[i] / r) * G + (Y[i] / r)].push_back(i);
    for (uint32_t i = 0; i < n; ++i) {
        i64 cx = X[i] / r, cy = Y[i] / r;
        for (i64 dx = -1; dx <= 1; ++dx) for (i64 dy = -1; dy <= 1; ++dy) {
            i64 gx = cx + dx, gy = cy + dy;
            if (gx < 0 || gy < 0 || gx >= G || gy >= G) continue;
            for (uint32_t j : cell[(size_t)gx * G + gy]) {
                if (j <= i) continue;
                i64 ddx = X[i] - X[j], ddy = Y[i] - Y[j];
                i64 d2 = ddx * ddx + ddy * ddy;
                if (d2 > r2) continue;
                i64 w = 1 + (i64)((__int128)wmax * (r2 - d2) / r2);
                g.add(i, j, w);
            }
        }
    }
    return g;
}

static Graph gen_community(Rng& rng, uint32_t n, u64 m, uint32_t K, uint32_t pin_permille) {
    Graph g; g.n = n;
    uint32_t size = n / K;                         // community c = vertices [c*size, (c+1)*size)
    while (g.edges.size() < m) {
        uint32_t u, v;
        if (rng.below(1000) < pin_permille) {
            uint32_t c = (uint32_t)rng.below(K);
            u = c * size + (uint32_t)rng.below(size);
            v = c * size + (uint32_t)rng.below(size);
        } else {
            u = (uint32_t)rng.below(n); v = (uint32_t)rng.below(n);
        }
        g.add(u, v, 1);
    }
    return g;
}

// -------------------------------------------------------------------- main --

static i64 arg(const std::vector<std::pair<std::string, std::string>>& kv, const char* k, i64 dflt, bool required = false) {
    for (auto& p : kv) if (p.first == k) return std::atoll(p.second.c_str());
    if (required) { std::fprintf(stderr, "missing parameter %s=\n", k); std::exit(2); }
    return dflt;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: gen <family> <seed> <out.mc> [key=value ...]\n");
        return 2;
    }
    std::string family = argv[1];
    u64 seed = std::strtoull(argv[2], nullptr, 10);
    const char* out = argv[3];
    std::vector<std::pair<std::string, std::string>> kv;
    for (int a = 4; a < argc; ++a) {
        std::string s = argv[a]; size_t eq = s.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "bad parameter %s\n", argv[a]); return 2; }
        kv.push_back({s.substr(0, eq), s.substr(eq + 1)});
    }
    // one stream per (family, seed, parameters): changing anything changes everything
    std::string tag = family + "|" + std::to_string(seed);
    for (auto& p : kv) tag += "|" + p.first + "=" + p.second;
    Rng rng(fnv(tag) ^ (seed * 0x9E3779B97F4A7C15ULL));

    Graph g;
    i64 wmax;
    if (family == "regular") {
        g = gen_regular(rng, (uint32_t)arg(kv, "n", 0, true), (uint32_t)arg(kv, "d", 0, true));
        wmax = arg(kv, "wmax", 1);
    } else if (family == "gnm") {
        g = gen_gnm(rng, (uint32_t)arg(kv, "n", 0, true), (u64)arg(kv, "m", 0, true));
        wmax = arg(kv, "wmax", 100);
    } else if (family == "powerlaw") {
        g = gen_powerlaw(rng, (uint32_t)arg(kv, "n", 0, true), (uint32_t)arg(kv, "k", 0, true));
        wmax = arg(kv, "wmax", 1);
    } else if (family == "tri") {
        g = gen_tri(rng, (uint32_t)arg(kv, "L", 0, true));
        wmax = arg(kv, "wmax", 100);
    } else if (family == "geo") {
        wmax = arg(kv, "wmax", 100);
        g = gen_geo(rng, (uint32_t)arg(kv, "n", 0, true), (uint32_t)arg(kv, "deg", 0, true), wmax);
        wmax = 0;                                  // weights already set by distance
    } else if (family == "community") {
        g = gen_community(rng, (uint32_t)arg(kv, "n", 0, true), (u64)arg(kv, "m", 0, true),
                          (uint32_t)arg(kv, "K", 0, true), (uint32_t)arg(kv, "pin", 800));
        wmax = arg(kv, "wmax", 1);
    } else {
        std::fprintf(stderr, "unknown family %s\n", family.c_str());
        return 2;
    }
    if (wmax > 0) assign_weights(g, rng, wmax);
    if (family != "geo") relabel(g, rng);          // geo has no index structure to hide
    finish(g);

    std::FILE* f = std::fopen(out, "w");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out); return 1; }
    std::fprintf(f, "%u %zu\n", g.n, g.edges.size());
    i64 W = 0, wmin = 1LL << 62, wmx = 0;
    vector<uint32_t> deg(g.n, 0);
    for (const Edge& e : g.edges) {
        std::fprintf(f, "%u %u %lld\n", e.u, e.v, (long long)e.w);
        W += e.w; wmin = std::min(wmin, e.w); wmx = std::max(wmx, e.w);
        ++deg[e.u]; ++deg[e.v];
    }
    std::fclose(f);
    uint32_t dmax = 0, dmin = ~0u;
    for (uint32_t d : deg) { dmax = std::max(dmax, d); dmin = std::min(dmin, d); }
    std::printf("{\"n\": %u, \"m\": %zu, \"total_weight\": %lld, \"wmin\": %lld, \"wmax\": %lld, "
                "\"deg_min\": %u, \"deg_max\": %u, \"family\": \"%s\"}\n",
                g.n, g.edges.size(), (long long)W, (long long)wmin, (long long)wmx, dmin, dmax, family.c_str());
    return 0;
}
