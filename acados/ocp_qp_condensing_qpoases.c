#include <stdlib.h>
#include "ocp_qp_condensing_qpoases.h"
#include "condensing.h"
#include "hpmpc/include/aux_d.h"

/* qpOASES specifics */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtypedef-redefinition"
#pragma clang diagnostic ignored "-Wtautological-pointer-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
#include "qpOASES_e/QProblem.h"
#pragma clang diagnostic pop
QProblem    QP;
real_t      *A_row_major;
real_t      cput;
int_t       nwsr;
real_t      primal_solution[NVC]                        = {0};  // QP primal solution vector
real_t      dual_solution[(NNN+1)*NX+NNN*(NX+NU)+NX]    = {0};  // QP dual solution vector
condensing_in in;
condensing_out out;
condensing_workspace ws;

// static int_t get_num_opt_vars(int_t N, int_t *nx, int_t *nu) {
//     int_t num_opt_vars = 0;
//     for (int_t i = 0; i < N; i++)
//         num_opt_vars += nx[i] + nu[i];
//     num_opt_vars += nx[N];
//     return num_opt_vars;
// }

static int_t get_num_condensed_vars(int_t N, int_t *nx, int_t *nu) {
    int_t num_condensed_vars = 0;
    // TODO(robin): this only holds for MPC, not MHE
    num_condensed_vars += 0*nx[1];
    for (int_t i = 0; i < N; i++)
        num_condensed_vars += nu[i];
    return num_condensed_vars;
}

static int_t get_num_constraints(int_t N, int_t *nx, int_t *nc) {
    int_t num_constraints = 0;
    for (int_t i = 0; i < N; i++) {
        // TODO(robin): count actual simple bounds on states
        num_constraints += nc[i] + nx[i];
    }
    num_constraints += nc[N];
    return num_constraints;
}

void write_array_to_file(FILE *outputFile, real_t *array, int_t size) {
    for (int_t i = 0; i < size; i++) fprintf(outputFile, "%g ", array[i]);
    fprintf(outputFile, "\n");
}

void write_QP_data_to_file() {
    FILE *outFile = fopen("../experimental/robin/QP_data.txt", "w");
    if (outFile == NULL) {
        fprintf(stderr, "%s\n", "OPEN FILE FAILED!");
    }
    write_array_to_file(outFile, out.H, NVC*NVC);
    write_array_to_file(outFile, out.h, NVC);
    write_array_to_file(outFile, out.A, 169*NVC);
    write_array_to_file(outFile, out.lb, NVC);
    write_array_to_file(outFile, out.ub, NVC);
    write_array_to_file(outFile, out.lbA, 169);
    write_array_to_file(outFile, out.ubA, 169);
    fclose(outFile);
}

static void fill_in_condensing_structs(int_t N, int_t *nx, int_t *nu, int_t *nb, int_t *nc,
    real_t **A, real_t **B, real_t **b,
    real_t **Q, real_t **S, real_t **R, real_t **q, real_t **r,
    int_t **idxb, real_t **lb, real_t **ub,
    real_t **Cx, real_t **Cu, real_t **lc, real_t **uc) {

    // Input
    in.N = N;
    in.nx = nx;
    in.nu = nu;
    in.nb = nb;
    in.nc = nc;
    in.A = A;
    in.B = B;
    in.b = b;
    in.Q = Q;
    in.S = S;
    in.R = R;
    in.q = q;
    in.r = r;
    in.idxb = idxb;
    in.lb = lb;
    in.ub = ub;
    in.Cu = Cu;
    in.Cx = Cx;
    in.lc = lc;
    in.uc = uc;

    // Output
    int_t num_condensed_vars = get_num_condensed_vars(N, nx, nu);
    int_t num_constraints = get_num_constraints(N, nx, nc);
    d_zeros(&out.H, num_condensed_vars, num_condensed_vars);
    d_zeros(&out.h, num_condensed_vars, 1);
    d_zeros(&out.lb, num_condensed_vars, 1);
    d_zeros(&out.ub, num_condensed_vars, 1);
    d_zeros(&out.A, num_constraints, num_condensed_vars);
    d_zeros(&out.lbA, num_constraints, 1);
    d_zeros(&out.ubA, num_constraints, 1);

    // Workspace
    ws.G = malloc(sizeof(*ws.G) * N);
    ws.g = malloc(sizeof(*ws.g) * N);
    ws.D = malloc(sizeof(*ws.D) * (N+1));
    for (int_t i = 0; i < N; i++) {
        ws.G[i] = malloc(sizeof(*(ws.G[i])) * (i+1));
        ws.D[i] = malloc(sizeof(*(ws.D[i])) * (i+1));
        d_zeros(&ws.g[i], NX, 1);
        for (int_t j = 0; j <= i; j++) {
            d_zeros(&ws.G[i][j], NX, NU);
            d_zeros(&ws.D[i][j], in.nc[i], NU);
        }
    }
    ws.D[N] = malloc(sizeof(*(ws.D[N])) * N);
    for (int_t i = 0; i < N; i++) {
        d_zeros(&ws.D[N][i], in.nc[N], NU);
    }
    d_zeros(&ws.W1_x, NX, NX);
    d_zeros(&ws.W2_x, NX, NX);
    d_zeros(&ws.W1_u, NX, NU);
    d_zeros(&ws.W2_u, NX, NU);
    d_zeros(&ws.w1, NX, 1);
    d_zeros(&ws.w2, NX, 1);
}

static int_t solve_QP(QProblem QP, real_t* primal_solution, real_t* dual_solution) {
    nwsr = 1000;
    cput = 100.0;

    int_t return_flag = QProblem_initW(&QP, out.H, out.h, A_row_major, out.lb,
                        out.ub, out.lbA, out.ubA,
                        &nwsr, &cput, NULL, dual_solution, NULL, NULL, NULL);
    QProblem_getPrimalSolution(&QP, primal_solution);
    QProblem_getDualSolution(&QP, dual_solution);
    return return_flag;
}

static void recover_state_trajectory(int_t N, real_t **x, real_t **u,
    real_t *primal_solution, real_t *x0) {

    #if FIXED_INITIAL_STATE == 1
    for (int_t i = 0; i < NX; i++) {
        x[0][i] = x0[i];
    }
    #endif
    for (int_t i = 0; i < N; i++) {
        for (int_t j = 0; j < NX; j++) {
            x[i+1][j] = ws.g[i][j];
            for (int_t k = 0; k <= i; k++) {
                for (int_t l = 0; l < NU; l++) {
                    x[i+1][j] += ws.G[i][k][j+NX*l]*primal_solution[NU*k+l];
                }
            }
        }
        #if FIXED_INITIAL_STATE == 1
        for (int_t j = 0; j < NU; j++) u[i][j] = primal_solution[i*NU+j];
        #else
        for (int_t j = 0; j < NU; j++) u[i][j] = primal_solution[NX+i*NU+j];
        #endif
    }
}

int_t ocp_qp_condensing_qpoases(int_t N, int_t *nx, int_t *nu, int_t *nb, int_t *nc,
    double **A, double **B, double **b,
    double **Q, double **S, double **R, double **q, double **r,
    int_t **idxb, double **lb, double **ub,
    double **Cx, double **Cu, double **lc, double **uc,
    double **x, double **u,
    struct ocp_qp_condensing_qpoases_args *args, double *work) {

    fill_in_condensing_structs(N, nx, nu, nb, nc, A, B, b, Q, S, R, q, r,
        idxb, lb, ub, Cx, Cu, lc, uc);
    condensingN2_fixed_initial_state(in, out, ws);

    // Process arguments
    args->dummy = 1.0;
    work = 0;

    // Symmetrize H
    int_t num_condensed_vars = get_num_condensed_vars(N, nx, nu);
    for (int_t i = 1; i < num_condensed_vars; i++) {
        for (int_t j = 0; j < i; j++) {
            out.H[i*num_condensed_vars+j] = out.H[j*num_condensed_vars+i];
        }
    }
    // Convert C to row major in A
    int_t num_constraints = get_num_constraints(N, nx, nc);
    d_zeros(&A_row_major, num_constraints, num_condensed_vars);
    for (int_t i = 0; i < num_constraints; i++) {
        for (int_t j = 0; j < num_condensed_vars; j++) {
            A_row_major[i*num_condensed_vars+j] = out.A[j*num_constraints+i];
        }
    }
    write_QP_data_to_file();
    int_t return_flag = solve_QP(QP, &primal_solution[0], &dual_solution[0]);
    recover_state_trajectory(N, x, u, &primal_solution[0], lb[0]);

    d_free(out.H);
    d_free(out.h);
    d_free(out.lb);
    d_free(out.ub);
    d_free(out.A);
    d_free(out.lbA);
    d_free(out.ubA);

    for (int_t i = 0; i < N; i++) {
        for (int_t j = 0; j <= i; j++) {
            d_free(ws.G[i][j]);
            d_free(ws.D[i][j]);
        }
        free(ws.G[i]);
        free(ws.D[i]);
        d_free(ws.g[i]);
    }
    for (int_t i = 0; i < N; i++) {
        d_free(ws.D[N][i]);
    }
    free(ws.D[N]);
    free(ws.G);
    free(ws.g);
    free(ws.D);
    d_free(ws.W1_x);
    d_free(ws.W2_x);
    d_free(ws.W1_u);
    d_free(ws.W2_u);
    d_free(ws.w1);
    d_free(ws.w2);

    return return_flag;
}

void initialise_qpoases() {
    QProblemCON(&QP, NVC, NCONSTRAINTS, HST_POSDEF);
    QProblem_setPrintLevel(&QP, PL_NONE);
    QProblem_printProperties(&QP);
}
