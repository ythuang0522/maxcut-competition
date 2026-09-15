// maxcut_foundation.cpp
//
// Foundation solver for the Max-Cut Competition. It is the randomized
// 1/2-approximation from the lecture -- put every vertex on a random side,
// so each edge is cut with probability 1/2 and the expected cut is half the
// total weight -- followed by the obvious repair: while some vertex has more
// weight to its own side than to the other, move it. That local search never
// makes the cut worse and ends at a cut that is at least half the total weight
// deterministically. Students start from this file and make its cuts LARGER.
//
// Usage:
//     ./solver <instance.mc> <output.cut> <time_limit_seconds>
//
// The third argument is the wall-clock budget the grader will enforce. This
// foundation ignores it (it finishes in a couple of seconds); a better solver
// keeps improving its cut until the budget is nearly used up.
//
// File formats:
//
//   <instance.mc>
//     n m                          (vertices 0..n-1, number of edges)
//     u v w                        (one edge per line, u < v, integer w >= 1)
//     ...
//
//   <output.cut>
//     s_0
//     s_1
//     ...                          (n lines, each 0 or 1: the side of vertex i)
//
// The cut's value is the total weight of the edges whose endpoints are on
// different sides. The grader recomputes it exactly in 64-bit integers.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

using std::vector;

static int32_t N = 0;
static int64_t M = 0;
static vector<int32_t> START;                // vertex i's neighbours are ADJ[START[i] .. START[i+1])
static vector<int32_t> ADJ;
static vector<int64_t> W;                    // weight of the corresponding edge

static void read_instance(const char* path) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) { std::fprintf(stderr, "cannot open instance: %s\n", path); std::exit(1); }
    long long m;
    if (std::fscanf(f, "%d %lld", &N, &m) != 2 || N <= 0 || m < 0) {
        std::fprintf(stderr, "bad instance header\n"); std::exit(1);
    }
    M = m;
    vector<int32_t> eu(M), ev(M); vector<int64_t> ew(M);
    vector<int32_t> cnt(N + 1, 0);
    for (int64_t e = 0; e < M; ++e) {
        int u, v; long long w;
        if (std::fscanf(f, "%d %d %lld", &u, &v, &w) != 3 || u < 0 || v < 0 || u >= N || v >= N) {
            std::fprintf(stderr, "bad edge %lld\n", (long long)e); std::exit(1);
        }
        eu[e] = u; ev[e] = v; ew[e] = w;
        ++cnt[u + 1]; ++cnt[v + 1];
    }
    std::fclose(f);
    for (int32_t i = 0; i < N; ++i) cnt[i + 1] += cnt[i];
    START = cnt; ADJ.resize(2 * M); W.resize(2 * M);
    vector<int32_t> pos(cnt.begin(), cnt.end() - 1);
    for (int64_t e = 0; e < M; ++e) {
        ADJ[pos[eu[e]]] = ev[e]; W[pos[eu[e]]++] = ew[e];
        ADJ[pos[ev[e]]] = eu[e]; W[pos[ev[e]]++] = ew[e];
    }
}

// A tiny fixed-seed generator, so the foundation's cut is reproducible.
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t rng() {
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <instance.mc> <output.cut> <time_limit_seconds>\n", argv[0]);
        return 1;
    }
    read_instance(argv[1]);

    // 1. The randomized 1/2-approximation: a coin flip per vertex.
    vector<char> side(N);
    for (int32_t i = 0; i < N; ++i) side[i] = (char)(rng() & 1);

    // gain[i] = how much the cut grows if vertex i changes side
    //         = (weight to its own side) - (weight to the other side)
    vector<int64_t> gain(N, 0);
    for (int32_t i = 0; i < N; ++i)
        for (int32_t p = START[i]; p < START[i + 1]; ++p)
            gain[i] += side[ADJ[p]] == side[i] ? W[p] : -W[p];

    // 2. Local improvement: move any vertex whose gain is positive, until none
    //    is. Each move raises the cut by at least 1, so this terminates.
    vector<int32_t> queue; queue.reserve(N);
    vector<char> queued(N, 1);
    for (int32_t i = 0; i < N; ++i) queue.push_back(i);
    size_t head = 0;
    while (head < queue.size()) {
        int32_t i = queue[head++]; queued[i] = 0;
        if (gain[i] <= 0) continue;
        side[i] ^= 1; gain[i] = -gain[i];
        for (int32_t p = START[i]; p < START[i + 1]; ++p) {
            int32_t j = ADJ[p];
            gain[j] += side[j] == side[i] ? 2 * W[p] : -2 * W[p];
            if (gain[j] > 0 && !queued[j]) { queued[j] = 1; queue.push_back(j); }
        }
    }

    // 3. Write the cut and report its value.
    std::FILE* out = std::fopen(argv[2], "w");
    if (!out) { std::fprintf(stderr, "cannot open output: %s\n", argv[2]); return 1; }
    for (int32_t i = 0; i < N; ++i) std::fputs(side[i] ? "1\n" : "0\n", out);
    std::fclose(out);

    int64_t cut = 0, total = 0;
    for (int32_t i = 0; i < N; ++i)
        for (int32_t p = START[i]; p < START[i + 1]; ++p)
            if (ADJ[p] > i) { total += W[p]; if (side[i] != side[ADJ[p]]) cut += W[p]; }
    std::fprintf(stderr, "cut %lld of total weight %lld (%.2f%%)\n",
                 (long long)cut, (long long)total, 100.0 * cut / total);
    return 0;
}
