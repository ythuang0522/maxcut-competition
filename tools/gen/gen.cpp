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
// Families (an edge appears once, u < v; weights are positive integers unless
// a sign option below is given):
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
//   tritail   L= [wmax=100] [tail=3] [relabel_seed= weight_seed=]
//                                 the same triangular lattice (degree 6), with
//                                 heavy-tailed weights w = 1 + floor(wmax u^tail),
//                                 u uniform with 20 bits: most edges light, a few
//                                 heavy. Built in three stages, each reproducible
//                                 on its own: (1) the tri graph with the same
//                                 seed, L and wmax; (2) its labels shuffled again
//                                 by a stream seeded with relabel_seed; (3) one
//                                 20-bit draw per edge, in output order, from a
//                                 stream seeded with weight_seed. Without the two
//                                 sub-seeds they are drawn from the main stream.
//   dlat      L= m= [pdel=250] [pglob=500] [R=4] [wmax=100] [tail=3]
//                                 irregular frustrated lattice: the triangular
//                                 lattice on an L x L torus, each lattice edge
//                                 deleted with probability pdel/1000, then extra
//                                 edges until there are m: a fraction pglob/1000
//                                 join two uniform random vertices, the rest join
//                                 a vertex to a random vertex of its (2R+1)^2
//                                 box. Heavy-tailed weights as in tritail.
//   geo       n= deg= [wmax=100]  random geometric graph in a square: points
//                                 within radius r are joined, heavier when
//                                 closer. deg is the target average degree.
//   cubic     L= [wmax=1]       simple cubic lattice on an L x L x L torus
//                                 (degree 6). Bipartite for even L, so it is
//                                 only interesting with signed weights: with
//                                 neg=500 it is the 3D Edwards-Anderson spin
//                                 glass.
//   community n= m= K= pin= [wmax=1]   planted partition: a fraction pin/1000
//                                 of the edges lie inside one of K hidden
//                                 communities; vertex labels are shuffled.
//
// Sign options (any family, applied after the family's own weights, before
// the labels are shuffled):
//   gauss=S    replace every weight by the sum of four uniform draws in
//              [-S, S], drawn again while it is 0 (a rounded bell curve,
//              about half the edges negative)
//   neg=P      then negate each weight with probability P/1000
//
// Output format (<out.mc>):
//     n m
//     u v w         (0 <= u < v < n, integer w != 0, one edge per line)

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
    // uniform in [0, k) from the top 53 bits by modulo (bias < 2^-30 for
    // k < 2^23); the tritail relabelling stage uses this one
    u64 below_mod(u64 k) { return (next() >> 11) % k; }
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

static Graph gen_cubic(uint32_t L) {
    if (L < 3) { std::fprintf(stderr, "need L >= 3\n"); std::exit(2); }
    Graph g; g.n = L * L * L;
    for (uint32_t x = 0; x < L; ++x)
        for (uint32_t y = 0; y < L; ++y)
            for (uint32_t z = 0; z < L; ++z) {
                uint32_t v = (x * L + y) * L + z;
                g.add(v, (((x + 1) % L) * L + y) * L + z, 1);
                g.add(v, (x * L + (y + 1) % L) * L + z, 1);
                g.add(v, (x * L + y) * L + (z + 1) % L, 1);
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

// 1 + floor(wmax * u^tail) with u = k / 2^20, k a 20-bit draw (the top 20
// bits of one splitmix64 output). tail=1 is uniform; tail=3 makes most edges
// light and a few heavy.
static i64 tail_weight(Rng& rng, i64 wmax, int tail) {
    __int128 u = (__int128)rng.below((u64)1 << 20), p = 1;
    for (int k = 0; k < tail; ++k) p *= u;
    return 1 + (i64)(((__int128)wmax * p) >> (20 * tail));
}

static void check_tail(int tail) {
    if (tail < 1 || tail > 4) { std::fprintf(stderr, "need 1 <= tail <= 4\n"); std::exit(2); }
}

// Stage 1 of tritail: exactly what  gen tri <seed> L=<L> wmax=<wmax>  writes
// (its own stream, its own weights, relabelled and sorted). Returns the main
// stream positioned after the tri stage, for drawing default sub-seeds.
static Graph tri_stage(u64 seed, uint32_t L, i64 wmax, Rng& after) {
    std::string tag = "tri|" + std::to_string(seed) + "|L=" + std::to_string(L) + "|wmax=" + std::to_string(wmax);
    Rng rng(fnv(tag) ^ (seed * 0x9E3779B97F4A7C15ULL));
    Graph g = gen_tri(rng, L);
    assign_weights(g, rng, wmax);
    relabel(g, rng);
    finish(g);
    after = rng;
    return g;
}

static Graph gen_tritail(u64 seed, uint32_t L, i64 wmax, int tail, bool have_rs, u64 relabel_seed,
                         bool have_ws, u64 weight_seed) {
    check_tail(tail);
    Rng main_rng(0);
    Graph g = tri_stage(seed, L, wmax, main_rng);
    if (!have_rs) relabel_seed = main_rng.next();
    if (!have_ws) weight_seed = main_rng.next();
    // stage 2: Fisher-Yates permutation from splitmix64(relabel_seed)
    Rng rr(relabel_seed);
    vector<uint32_t> perm(g.n);
    for (uint32_t i = 0; i < g.n; ++i) perm[i] = i;
    for (uint32_t i = g.n - 1; i > 0; --i) std::swap(perm[i], perm[rr.below_mod(i + 1)]);
    for (Edge& e : g.edges) {
        e.u = perm[e.u]; e.v = perm[e.v];
        if (e.u > e.v) std::swap(e.u, e.v);
    }
    finish(g);
    // stage 3: one draw per edge in output order from splitmix64(weight_seed)
    Rng wr(weight_seed);
    for (Edge& e : g.edges) e.w = tail_weight(wr, wmax, tail);
    return g;
}

static Graph gen_dlat(Rng& rng, uint32_t L, u64 m, i64 pdel, i64 pglob, int R, i64 wmax, int tail) {
    check_tail(tail);
    if (L < 4) { std::fprintf(stderr, "dlat: need L >= 4\n"); std::exit(2); }
    Graph g; g.n = L * L;
    for (uint32_t i = 0; i < L; ++i)
        for (uint32_t j = 0; j < L; ++j) {
            uint32_t v = i * L + j;
            uint32_t nb[3] = {i * L + (j + 1) % L, ((i + 1) % L) * L + j, ((i + 1) % L) * L + (j + 1) % L};
            for (uint32_t x : nb) {
                if ((i64)rng.below(1000) < pdel) continue;
                g.add(v, x, tail_weight(rng, wmax, tail));
            }
        }
    if (m < g.edges.size()) {
        std::fprintf(stderr, "dlat: m=%llu is below the %zu lattice edges kept\n", (unsigned long long)m, g.edges.size());
        std::exit(2);
    }
    while (g.edges.size() < m) {
        uint32_t u = (uint32_t)rng.below(g.n), v;
        bool glob = (i64)rng.below(1000) < pglob;
        if (glob) v = (uint32_t)rng.below(g.n);
        else {
            int i = (int)(u / L), j = (int)(u % L);
            int di = (int)rng.range(-R, R), dj = (int)rng.range(-R, R);
            v = (uint32_t)(((i + di + (int)L) % (int)L) * (int)L + (j + dj + (int)L) % (int)L);
        }
        g.add(u, v, tail_weight(rng, wmax, tail));
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

static bool has_arg(const std::vector<std::pair<std::string, std::string>>& kv, const char* k) {
    for (auto& p : kv) if (p.first == k) return true;
    return false;
}

static u64 uarg(const std::vector<std::pair<std::string, std::string>>& kv, const char* k) {
    for (auto& p : kv) if (p.first == k) return std::strtoull(p.second.c_str(), nullptr, 10);
    return 0;
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
    } else if (family == "tritail") {
        wmax = arg(kv, "wmax", 100);
        g = gen_tritail(seed, (uint32_t)arg(kv, "L", 0, true), wmax, (int)arg(kv, "tail", 3),
                        has_arg(kv, "relabel_seed"), uarg(kv, "relabel_seed"),
                        has_arg(kv, "weight_seed"), uarg(kv, "weight_seed"));
        wmax = 0;                                  // weights and labels already final
    } else if (family == "dlat") {
        wmax = arg(kv, "wmax", 100);
        g = gen_dlat(rng, (uint32_t)arg(kv, "L", 0, true), (u64)arg(kv, "m", 0, true), arg(kv, "pdel", 250),
                     arg(kv, "pglob", 500), (int)arg(kv, "R", 4), wmax, (int)arg(kv, "tail", 3));
        wmax = 0;                                  // weights already set
    } else if (family == "geo") {
        wmax = arg(kv, "wmax", 100);
        g = gen_geo(rng, (uint32_t)arg(kv, "n", 0, true), (uint32_t)arg(kv, "deg", 0, true), wmax);
        wmax = 0;                                  // weights already set by distance
    } else if (family == "cubic") {
        g = gen_cubic((uint32_t)arg(kv, "L", 0, true));
        wmax = arg(kv, "wmax", 1);
    } else if (family == "community") {
        g = gen_community(rng, (uint32_t)arg(kv, "n", 0, true), (u64)arg(kv, "m", 0, true),
                          (uint32_t)arg(kv, "K", 0, true), (uint32_t)arg(kv, "pin", 800));
        wmax = arg(kv, "wmax", 1);
    } else {
        std::fprintf(stderr, "unknown family %s\n", family.c_str());
        return 2;
    }
    if (wmax > 0) assign_weights(g, rng, wmax);
    if (has_arg(kv, "gauss")) {
        i64 S = arg(kv, "gauss", 0);
        if (S < 1) { std::fprintf(stderr, "need gauss >= 1\n"); return 2; }
        for (Edge& e : g.edges) {
            do { e.w = 0; for (int k = 0; k < 4; ++k) e.w += rng.range(-S, S); } while (e.w == 0);
        }
    }
    if (i64 neg = arg(kv, "neg", 0); neg > 0)
        for (Edge& e : g.edges) if ((i64)rng.below(1000) < neg) e.w = -e.w;
    // geo has no index structure to hide; tritail is relabelled by its own stages
    if (family != "geo" && family != "tritail") relabel(g, rng);
    finish(g);

    std::FILE* f = std::fopen(out, "w");
    if (!f) { std::fprintf(stderr, "cannot write %s\n", out); return 1; }
    std::fprintf(f, "%u %zu\n", g.n, g.edges.size());
    i64 W = 0, wmin = 1LL << 62, wmx = -(1LL << 62);
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
