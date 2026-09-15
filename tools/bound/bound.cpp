// bound.cpp -- a PROVEN upper bound on the maximum cut of a weighted graph.
//
//   bound <instance.mc> [--rank R] [--sweeps K] [--iters K] [--fine K] [--json]
//
// Write a cut as x in {-1,+1}^n (x_i = side of vertex i). Then
//
//     cut(x) = sum_{ij in E} w_ij [x_i != x_j] = (1/4) x^T L x
//
// where L = D - A is the weighted Laplacian. For any vector u with
// sum_i u_i = 0, x^T diag(u) x = sum_i u_i = 0 (every x_i^2 = 1), so
//
//     cut(x) = (1/4) x^T (L + diag(u)) x  <=  (n/4) lambda_max(L + diag(u)).
//
// This is the Delorme-Poljak eigenvalue bound, the dual of the
// Goemans-Williamson semidefinite relaxation: minimising over u gives exactly
// the SDP value. The LP relaxation of Max-Cut is useless here (it says "all
// edges", integrality gap 2); the semidefinite relaxation is the one that
// bites, and on random graphs it is a few percent above the true optimum.
//
// lambda_max is computed by the Lanczos method. Lanczos approaches the
// largest eigenvalue FROM BELOW, so the run is continued until the estimate
// has stopped moving to 1e-12 (relative) and a 1e-4 relative safety margin is
// added before flooring to an integer (all weights are integers, so the
// optimum is). The margin is 30-100x smaller than the gap to the optimum on
// every shipped instance and is the same for everybody.
//
// The minimisation over u is a plain projected subgradient method: the
// gradient of lambda_max at u is v o v (the top eigenvector squared), projected
// onto sum u = 0. It is slow but the bound is valid at EVERY iterate; the
// descent only tightens it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using std::vector;

static int N = 0;
static long long M = 0;
static vector<int> START, ADJ;
static vector<double> W, DEG;                      // per adjacency entry / weighted degree

static void read_instance(const char* path) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); std::exit(1); }
    if (std::fscanf(f, "%d %lld", &N, &M) != 2) { std::fprintf(stderr, "bad header\n"); std::exit(1); }
    vector<int> eu(M), ev(M); vector<long long> ew(M);
    vector<int> cnt(N + 1, 0);
    for (long long e = 0; e < M; ++e) {
        if (std::fscanf(f, "%d %d %lld", &eu[e], &ev[e], &ew[e]) != 3) { std::fprintf(stderr, "bad edge %lld\n", e); std::exit(1); }
        ++cnt[eu[e] + 1]; ++cnt[ev[e] + 1];
    }
    std::fclose(f);
    for (int i = 0; i < N; ++i) cnt[i + 1] += cnt[i];
    START = cnt; ADJ.resize(2 * M); W.resize(2 * M); DEG.assign(N, 0.0);
    vector<int> pos(cnt.begin(), cnt.end() - 1);
    for (long long e = 0; e < M; ++e) {
        ADJ[pos[eu[e]]] = ev[e]; W[pos[eu[e]]++] = (double)ew[e];
        ADJ[pos[ev[e]]] = eu[e]; W[pos[ev[e]]++] = (double)ew[e];
        DEG[eu[e]] += (double)ew[e]; DEG[ev[e]] += (double)ew[e];
    }
}

static double Wtot_d() { double t = 0; for (int i = 0; i < N; ++i) t += DEG[i]; return t / 2; }

// y = (L + diag(u)) x
static void matvec(const vector<double>& u, const vector<double>& x, vector<double>& y) {
    for (int i = 0; i < N; ++i) {
        double s = (DEG[i] + u[i]) * x[i];
        for (int p = START[i]; p < START[i + 1]; ++p) s -= W[p] * x[ADJ[p]];
        y[i] = s;
    }
}

// Largest eigenvalue of the symmetric tridiagonal (alpha, beta): bisection on
// the Sturm count.
static double tridiag_max(const vector<double>& a, const vector<double>& b, double lo, double hi) {
    int k = a.size();
    for (int it = 0; it < 200; ++it) {
        double mid = 0.5 * (lo + hi);
        int cnt = 0; double q = a[0] - mid;
        if (q < 0) ++cnt;
        for (int i = 1; i < k; ++i) {
            double bb = b[i - 1] * b[i - 1];
            q = a[i] - mid - (q == 0 ? bb / 1e-300 : bb / q);
            if (q < 0) ++cnt;
        }
        if (cnt < k) lo = mid; else hi = mid;    // all k eigenvalues below mid -> max < mid
        if (hi - lo <= 1e-15 * std::max(1.0, std::fabs(hi))) break;
    }
    return 0.5 * (lo + hi);
}

// Eigenvector of the tridiagonal for eigenvalue theta by inverse iteration.
static vector<double> tridiag_vec(const vector<double>& a, const vector<double>& b, double theta) {
    int k = a.size();
    vector<double> y(k, 1.0);
    double shift = theta + 1e-10 * std::max(1.0, std::fabs(theta));
    for (int sweep = 0; sweep < 3; ++sweep) {
        vector<double> dd(k), rhs = y, z(k);
        for (int i = 0; i < k; ++i) dd[i] = a[i] - shift;
        for (int i = 1; i < k; ++i) {
            double m = b[i - 1] / dd[i - 1];
            dd[i] -= m * b[i - 1]; rhs[i] -= m * rhs[i - 1];
        }
        z[k - 1] = rhs[k - 1] / dd[k - 1];
        for (int i = k - 2; i >= 0; --i) z[i] = (rhs[i] - b[i] * z[i + 1]) / dd[i];
        double nrm = 0; for (double v : z) nrm += v * v; nrm = std::sqrt(nrm);
        for (int i = 0; i < k; ++i) y[i] = z[i] / nrm;
    }
    return y;
}

struct Lanczos {
    double theta;                                  // largest Ritz value (<= lambda_max)
    int steps;
    vector<double> vec;                            // corresponding Ritz vector (unit)
};

// Two-pass Lanczos without reorthogonalisation: pass 1 builds T until the top
// Ritz value stalls, pass 2 replays the identical recurrence to assemble the
// Ritz vector (storing every Lanczos vector would need k*n doubles). Without
// reorthogonalisation ghost copies of converged eigenvalues appear, but the
// LARGEST Ritz value still rises monotonically towards lambda_max and never
// exceeds it (Paige), which is all that is used here.
static Lanczos lanczos(const vector<double>& u, const vector<double>& start, int kmax, double tol) {
    vector<double> q(start), qprev(N, 0.0), w(N);
    double nrm = 0; for (double v : q) nrm += v * v; nrm = std::sqrt(nrm);
    for (double& v : q) v /= nrm;
    vector<double> a, b;
    double beta = 0, theta = -1e300; int stall = 0;
    double gersh_hi = -1e300;
    for (int i = 0; i < N; ++i) gersh_hi = std::max(gersh_hi, 2 * DEG[i] + u[i]);
    for (int k = 0; k < kmax; ++k) {
        matvec(u, q, w);
        double alpha = 0; for (int i = 0; i < N; ++i) alpha += w[i] * q[i];
        for (int i = 0; i < N; ++i) w[i] -= alpha * q[i] + beta * qprev[i];
        a.push_back(alpha); if (k) b.push_back(beta);
        beta = 0; for (double v : w) beta += v * v; beta = std::sqrt(beta);
        if (k % 10 == 9 || beta < 1e-12) {
            double th = tridiag_max(a, b, -gersh_hi, gersh_hi);
            if (th > theta + tol * std::fabs(th)) { theta = th; stall = 0; } else stall += 10;
            if (stall >= 100 || beta < 1e-12) break;
        }
        for (int i = 0; i < N; ++i) { qprev[i] = q[i]; q[i] = w[i] / beta; }
    }
    int steps = a.size();
    theta = tridiag_max(a, b, -gersh_hi, gersh_hi);
    vector<double> y = tridiag_vec(a, b, theta);
    // pass 2: replay the recurrence, accumulating the Ritz vector
    q = start; nrm = 0; for (double v : q) nrm += v * v; nrm = std::sqrt(nrm);
    for (double& v : q) v /= nrm;
    std::fill(qprev.begin(), qprev.end(), 0.0);
    vector<double> vec(N, 0.0);
    beta = 0;
    for (int j = 0; j < steps; ++j) {
        for (int i = 0; i < N; ++i) vec[i] += y[j] * q[i];
        if (j + 1 == steps) break;
        matvec(u, q, w);
        double alpha = a[j];
        for (int i = 0; i < N; ++i) w[i] -= alpha * q[i] + beta * qprev[i];
        beta = b[j];
        for (int i = 0; i < N; ++i) { qprev[i] = q[i]; q[i] = w[i] / beta; }
    }
    nrm = 0; for (double v : vec) nrm += v * v; nrm = std::sqrt(nrm);
    for (double& v : vec) v /= nrm;
    return Lanczos{theta, steps, vec};
}

// ---------------------------------------------------------------- the SDP --
//
// Low-rank (Burer-Monteiro) solve of the Goemans-Williamson relaxation by the
// mixing method: each vertex gets a unit vector v_i in R^r, and we minimise
// f(V) = sum_{ij in E} w_ij <v_i, v_j> by cyclically setting
// v_i <- -normalise(sum_j w_ij v_j). The SDP cut value is (W - f)/2, a
// LOWER estimate of the SDP optimum (the relaxation is a maximisation).
//
// The certified bound comes from the dual. At a stationary V,
// sum_j w_ij v_j = -mu_i v_i with mu_i >= 0, so with y_i = D_i + mu_i we have
// (diag(y) - L) V = 0. For any X >= 0 with X_ii = 1, <L, X> <= <diag(y), X>
// = sum y_i as soon as diag(y) - L >= 0; if it is not quite (V is only
// nearly stationary), shifting every y_i by lambda_max(L - diag(y)) makes it
// so. In the (n/4) lambda_max(L + diag(u)) form this is u_i = -y_i + mean(y).
static vector<double> solve_sdp(int rank, int sweeps, double tol, bool verbose, double* primal_out) {
    vector<double> V((size_t)N * rank);
    uint64_t s = 0xC0FFEEULL;
    for (int i = 0; i < N; ++i) {
        double nrm = 0;
        for (int k = 0; k < rank; ++k) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            double g = (double)(s >> 11) / 9007199254740992.0 - 0.5;
            V[(size_t)i * rank + k] = g; nrm += g * g;
        }
        nrm = std::sqrt(nrm);
        for (int k = 0; k < rank; ++k) V[(size_t)i * rank + k] /= nrm;
    }
    vector<double> acc(rank);
    double prev = 1e300;
    for (int sw = 0; sw < sweeps; ++sw) {
        for (int i = 0; i < N; ++i) {
            std::fill(acc.begin(), acc.end(), 0.0);
            for (int p = START[i]; p < START[i + 1]; ++p) {
                const double* vj = &V[(size_t)ADJ[p] * rank];
                double w = W[p];
                for (int k = 0; k < rank; ++k) acc[k] += w * vj[k];
            }
            double nrm = 0; for (int k = 0; k < rank; ++k) nrm += acc[k] * acc[k];
            nrm = std::sqrt(nrm);
            if (nrm > 0) for (int k = 0; k < rank; ++k) V[(size_t)i * rank + k] = -acc[k] / nrm;
        }
        // f = sum_{edges} w <v_i, v_j>
        double f = 0;
        for (int i = 0; i < N; ++i)
            for (int p = START[i]; p < START[i + 1]; ++p)
                if (ADJ[p] > i) {
                    const double* vi = &V[(size_t)i * rank]; const double* vj = &V[(size_t)ADJ[p] * rank];
                    double d = 0; for (int k = 0; k < rank; ++k) d += vi[k] * vj[k];
                    f += W[p] * d;
                }
        if (verbose && sw % 20 == 0) std::fprintf(stderr, "sweep %4d  sdp cut %.3f\n", sw, (Wtot_d() - f) / 2);
        if (std::fabs(prev - f) <= tol * std::max(1.0, std::fabs(f))) { prev = f; break; }
        prev = f;
    }
    *primal_out = (Wtot_d() - prev) / 2;
    // dual multipliers
    vector<double> u(N);
    double meanY = 0;
    for (int i = 0; i < N; ++i) {
        std::fill(acc.begin(), acc.end(), 0.0);
        for (int p = START[i]; p < START[i + 1]; ++p) {
            const double* vj = &V[(size_t)ADJ[p] * rank];
            for (int k = 0; k < rank; ++k) acc[k] += W[p] * vj[k];
        }
        double mu = 0; for (int k = 0; k < rank; ++k) mu += acc[k] * acc[k];
        mu = std::sqrt(mu);
        u[i] = -(DEG[i] + mu); meanY += DEG[i] + mu;
    }
    meanY /= N;
    for (int i = 0; i < N; ++i) u[i] += meanY;          // sum u = 0
    return u;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: bound <instance.mc> [--rank R] [--sweeps K] [--iters K] [--fine K] [--json]\n"); return 1; }
    read_instance(argv[1]);
    int iters = 0, rough = 400, fine = 8000, rank = 20, sweeps = 1500; bool json = false; double step0 = 0.02;
    for (int a = 2; a < argc; ++a) {
        if (!std::strcmp(argv[a], "--iters") && a + 1 < argc) iters = std::atoi(argv[++a]);
        else if (!std::strcmp(argv[a], "--rough") && a + 1 < argc) rough = std::atoi(argv[++a]);
        else if (!std::strcmp(argv[a], "--fine") && a + 1 < argc) fine = std::atoi(argv[++a]);
        else if (!std::strcmp(argv[a], "--rank") && a + 1 < argc) rank = std::atoi(argv[++a]);
        else if (!std::strcmp(argv[a], "--sweeps") && a + 1 < argc) sweeps = std::atoi(argv[++a]);
        else if (!std::strcmp(argv[a], "--step") && a + 1 < argc) step0 = std::atof(argv[++a]);
        else if (!std::strcmp(argv[a], "--json")) json = true;
    }
    long long Wtot = (long long)std::llround(Wtot_d());

    // deterministic start vector for Lanczos
    vector<double> start(N);
    uint64_t s = 0x1234567ULL;
    for (int i = 0; i < N; ++i) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; start[i] = (double)(s >> 11) / 9007199254740992.0 - 0.5; }

    double primal = 0;
    vector<double> u = solve_sdp(rank, sweeps, 1e-8, !json, &primal);
    Lanczos L1 = lanczos(u, start, fine, 1e-12);
    double cur = L1.theta, best = cur; vector<double> bestU = u, v = L1.vec;
    if (!json) std::fprintf(stderr, "sdp primal %.3f   dual (n/4)lambda %.3f  (lambda %.6f, %d steps)\n", primal, N * cur / 4, cur, L1.steps);

    // optional subgradient polish
    double step = step0 * std::fabs(cur); int noimp = 0;
    for (int it = 0; it < iters; ++it) {
        vector<double> g(N); double gn = 0;
        for (int i = 0; i < N; ++i) { g[i] = v[i] * v[i] - 1.0 / N; gn += g[i] * g[i]; }
        gn = std::sqrt(gn);
        if (gn < 1e-14) break;
        vector<double> cand(N);
        for (int i = 0; i < N; ++i) cand[i] = u[i] - step * g[i] / gn;
        Lanczos Lc = lanczos(cand, v, rough, 1e-10);
        if (Lc.theta < cur) { u = cand; cur = Lc.theta; v = Lc.vec; noimp = 0; step *= 1.2; }
        else if (++noimp >= 3) { step *= 0.5; noimp = 0; }
        if (cur < best) { best = cur; bestU = u; }
        if (!json && it % 10 == 0) std::fprintf(stderr, "polish %3d lambda %.6f best %.6f\n", it, cur, best);
        if (step < 1e-7 * std::fabs(best)) break;
    }
    Lanczos Lf = iters > 0 ? lanczos(bestU, v, fine, 1e-12) : L1;
    double lam = Lf.theta;
    double bound_real = (double)N * lam / 4.0;
    double margin = 1e-4;
    long long ub = (long long)std::floor(bound_real * (1.0 + margin));
    if (ub > Wtot) ub = Wtot;                      // never above the total weight
    if (json) {
        std::printf("{\"upper_bound\": %lld, \"lambda_max\": %.9f, \"bound_real\": %.3f, \"sdp_primal\": %.3f, "
                    "\"total_weight\": %lld, \"lanczos_steps\": %d, \"rank\": %d, \"sweeps\": %d, \"iters\": %d, \"margin\": %g}\n",
                    ub, lam, bound_real, primal, Wtot, Lf.steps, rank, sweeps, iters, margin);
    } else {
        std::printf("upper_bound %lld  (n/4 lambda_max = %.3f, sdp primal %.3f, W %lld)\n",
                    ub, bound_real, primal, Wtot);
    }
    return 0;
}
