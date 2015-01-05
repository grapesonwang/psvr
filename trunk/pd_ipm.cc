/*
Copyright 2007 Google Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include <cfloat>
#include <cstring>
#include <vector>
#include <cmath>
#include <climits>

#include "pd_ipm.h"
#include "timer.h"
#include "common.h"
#include "document.h"
#include "kernel.h"
#include "pd_ipm_parm.h"
#include "model.h"
#include "matrix.h"
#include "matrix_manipulation.h"
#include "util.h"
#include "io.h"
#include "parallel_interface.h"

namespace psvr {
// The primal dual interior point method is based on the book below,
// Convex Optimization, Stephen Boyd and Lieven Vandenberghe,
// Cambridge University Press.
int PrimalDualIPM::Solve(const PrimalDualIPMParameter& parameter,
                         const ParallelMatrix& h,
                         const Document& doc,
                         Model* model,
                         bool failsafe) {
  TrainingTimeProfile::ipm_misc.Start();
  register int i, step;
  int local_num_rows = doc.GetLocalNumberRows();
  int num_doc_rows = doc.GetGlobalNumberRows();
  double c_pos = parameter.weight_positive * parameter.hyper_parm;
  double c_neg = parameter.weight_negative * parameter.hyper_parm;
  // Calculate total constraint
  //
  // Note  0 <= \alpha <= C, transform this
  // to 2 vector inequations, -\alpha <= 0 (code formulae 1) and
  // \alpha - C <= 0 (code formulae 2), this is standard
  // constraint form, refer to Convex
  // Optimiztion. so total inequality constraints is 2n.
  // note, n is length of vector \alpha.
  int num_constraints = num_doc_rows + num_doc_rows + num_doc_rows + num_doc_rows;

  // Allocating memory for variables
  //
  // Note x here means \alpha in paper. la here is
  // Langrange multiplier of (code formulae 2), \lambda in WG's paper.
  // xi here is Langrange multiplier of (code formulae 1), \xi in WG's paper.
  // nu here is Langrange multiplier of equality constraints, \nu in WG's
  // paper, here comes a little bit of explanation why \nu is a scalar instead
  // of a vector. Note, the equality constraint
  // coeffient matrix but 1-dim y^T,
  // substitute it to A in Convex Optimiztion (11.54), we know that \nu is a
  // scalar.
  double *x = new double[local_num_rows];
  double *x_star = new double[local_num_rows];
  double *la = new double[local_num_rows];
  double *xi = new double[local_num_rows];
  double *the = new double[local_num_rows];
  double *phi = new double[local_num_rows];
  double nu = 0.0;
  double *value = new double[local_num_rows];
  doc.GetLocalValues(value);
 
  // xiczstar, lacz, thecz, phizstar here are temporary vectors, used to store intermediate result.
  // Actually, xiczstar stores \frac{\xi}{C - \x_star},
  // lacz stores \frac{\la}{(C - \x)}
  // thecz stores \frac{\the}{(\x)}
  // phizstar stores \frac{\phi}{(\x_star)}
  //
  // tczm, tczp, tczstar, tzstar here are also temporary vectors.
  // tczm stores \frac{\1}{C - \x},
  // tczp stores \frac{\1}{\x}.
  // tczstar stores \frac{\1}{C - \x_star},
  // tzstar stores \frac{\}{\x_star}.
  //
  // Note all the division of vectors above is elements-wise division.
  double *xicxstar = new double[local_num_rows];
  double *lacx = new double[local_num_rows];
  double *thex = new double[local_num_rows];
  double *phixstar = new double[local_num_rows];

  double *tx = new double[local_num_rows];
  double *tcx = new double[local_num_rows];
  double *tcxstar = new double[local_num_rows];
  double *txstar = new double[local_num_rows];

  double *delta = new double[local_num_rows];
  double *delta_star = new double[local_num_rows];
  double *rho_x = new double[local_num_rows];
  double *rho_x_star = new double[local_num_rows];

  double *mult = new double[local_num_rows];

  // dla, dxi, dx, dnu are \lamba, \xi, \z, \nu in the Newton Step,
  // Note dnu is a scalar, all the other are vectors.
  double *dla = new double[local_num_rows];
  double *dxi = new double[local_num_rows];
  double *dthe = new double[local_num_rows];
  double *dphi = new double[local_num_rows];
  double dnu = 0.0;
  double *dx = new double[local_num_rows];
  double *dx_star = new double[local_num_rows];

  // d is a diagonal matrix,
  //   \diag(\frac{\la_i}{C - \z_i} + \frac{\the_i}{C + \z_i}).
  //
  // e is a diagonal matrix,
  //   \diag(\frac{\xi_i}{C - \z_star_i} + \frac{\phi_i}{\z_star_i}).
  //
  // Note in the code, z has two
  // phase of intue, the first result
  // is Q\z + 1_n + \nu y, part of formulae
  // (8) and (17), the last phase is to complete formulae (17)
  double *d = new double[local_num_rows];
  double *z = new double[local_num_rows];

  double t;     // step
  double eta;   // surrogate gap
  double resp;  // primal residual
  double resd;  // dual residual
  double resd_1;  // dual residual_1

  // initializes the primal-dual variables
  // last \lambda, \xi to accelerate Newton method.

  // initializes \lambda, \xi and \nu
  //   \lambda = \frac{C}{10}
  //   \xi = \frac{C}{10}
  //   \nu = 0

  memset(x, 0, sizeof(x[0]) * local_num_rows);
  memset(x_star, 0, sizeof(x_star[0]) * local_num_rows);
  nu = 0.0;
  for (i = 0; i < local_num_rows; ++i) {
    double c = (value[i] > 0) ? c_pos : c_neg;
    la[i] = c / 10.0;
    xi[i] = c / 10.0;
    the[i] = c / 10.0;
    phi[i] = c / 10.0;
  }
  const ParallelMatrix& rbicf = h;
  int rank = rbicf.GetNumCols();
  ParallelInterface* mpi = ParallelInterface::GetParallelInterface();
  int myid = mpi->GetProcId();
  if (myid == 0) {
    cout << StringPrintf("Training SVR ... (H = %d x %d)\n",
              num_doc_rows, rank);
  }
  // Note icfA is p \times p Symetric Matrix, actually is I + H^T D H, refer
  // to WG's paper 4.3.2. We should compute (I + H^T D H)^{-1}, using linear
  // equations trick to get it later.
  LLMatrix icfA(rbicf.GetNumCols());
  // iterating IPM algorithm based on ICF
  mpi->Barrier(MPI_COMM_WORLD);
  TrainingTimeProfile::ipm_misc.Stop();

  // Load the values to resume an interrupted solving prcess.
  TrainingTimeProfile::ipm_misc.Start();
  step = 0;
  if (failsafe) {    
    LoadVariables(parameter, local_num_rows, num_doc_rows,
                  &step, &nu, x, x_star, la, xi, the, phi);

  }
  double time_last_save = Timer::GetCurrentTime();
  TrainingTimeProfile::ipm_misc.Stop();
  for (; step < parameter.max_iter; ++step) {
    TrainingTimeProfile::ipm_misc.Start();
    double time_current = Timer::GetCurrentTime();
    if (failsafe && time_current - time_last_save > parameter.save_interval) {
      SaveVariables(parameter, local_num_rows, num_doc_rows,
                    step, nu, x, x_star, la, xi, the, phi);
      time_last_save = time_current;
    }

    if (myid == 0) {
      cout << StringPrintf("========== Iteration %d ==========\n", step);
    }
    cout<<"x: "<<x[0]<<" "<<x[1]<<" "<<x[2]<<" "<<x[17]<<endl;
    cout<<"x_star: "<<x_star[0]<<" "<<x_star[1]<<" "<<x_star[2]<<" "<<x_star[17]<<endl;
    TrainingTimeProfile::ipm_misc.Stop();
    // Computing surrogate Gap
    // compute surrogate gap, for definition detail, refer to formulae (11.59)
    // in Convex Optimization. Note t and eta
    // have a relation, for more details,
    // refer to Algorithm 11.2 step 1. in Convext Optimization.
    TrainingTimeProfile::surrogate_gap.Start();
    eta = ComputeSurrogateGap(c_pos, c_neg, value, local_num_rows, x, x_star, la, xi, the, phi);
    // Note m is number of total constraints
    t = (parameter.mu_factor) * static_cast<double>(num_constraints) / eta;
    if (parameter.verb >= 1 && myid == 0) {
      cout << StringPrintf("sgap: %-.10le t: %-.10le\n", eta, t);
    }
    TrainingTimeProfile::surrogate_gap.Stop();


    ComputePartialRho(rbicf, x, x_star, local_num_rows, parameter.tradeoff, rho_x);

    // computes
    //    z = -z + y - \nu = H H^T \alpha - tradeoff \alpha + y - \nu
    //    r_{dual} = ||\lambda - \xi + z||_2
    //    r_{pri} = |y^T \alpha|
    // here resd coresponds to r_{dual}, resp coresponds to r_{pri},
    // refer to formulae (8) and (11) in WG's paper.
    TrainingTimeProfile::check_stop.Start();
    resp = 0.0;
    resd = 0.0;
    resd_1 = 0.0;
    for (i = 0; i < local_num_rows; ++i) {
      register double temp, temp_1;
      temp = rho_x[i] + parameter.epsilon_svr - value[i] + la[i] - the[i] + nu;
      temp_1 = -rho_x[i] + parameter.epsilon_svr + value[i] + xi[i] - phi[i] - nu;
      resd += temp * temp;
      resd_1 += temp_1 * temp_1;
      resp += x[i] - x_star[i];
    }

    double from_sum[3], to_sum[3];
    from_sum[0] = resp;
    from_sum[1] = resd;
    from_sum[2] = resd_1;
    mpi->AllReduce(from_sum, to_sum, 3, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
    resp = fabs(to_sum[0]);
    resd = sqrt(to_sum[1]);
    resd_1 = sqrt(to_sum[2]);
    if (parameter.verb >= 1 && myid == 0) {
      cout << StringPrintf("r_pri: %-.10le r_dual: %-.10le r_dual_1: %-.10le\n",
                                resp,
                                resd, 
                                resd_1);
    }

    cout<<"resp: "<<resp<<" resd: "<<resd<<" resd_1: "<<resd_1<<" eta: "<<eta<<endl;
    // Converge Stop Condition. For more details refer to Algorithm 11.2
    // in Convex Optimization.
    if ((resp <= parameter.feas_thresh) &&
        (resd <= parameter.feas_thresh) &&
        (resd_1 <= parameter.feas_thresh) &&
        (eta <= parameter.sgap)) {
      break;
    }
    TrainingTimeProfile::check_stop.Stop();

    // Update Variables
    //
    // computes
    //     tlx = \frac{1}{t \alpha}
    //     tux = \frac{1}{t (C - \alpha)}
    //     xilx = \frac{\xi}{\alpha}
    //     laux = \frac{\lambda}{C - \alpha}
    //     D^(-1) = \diag(\frac{\xi}{\alpha} + \frac{\lambda}{C - \alpha})
    // note D is a diagonal matrix and its inverse can be easily computed.
    TrainingTimeProfile::update_variables.Start();

    double m_lx, m_ux, m_lxstar, m_uxstar;
    for (i = 0; i < local_num_rows; ++i) {
      double c = (value[i] > 0) ? c_pos : c_neg;
      m_lx = std::max(x[i], parameter.epsilon_x);
      m_ux = std::max(c - x[i], parameter.epsilon_x);

      m_lxstar = std::max(x_star[i], parameter.epsilon_x);
      m_uxstar = std::max(c - x_star[i], parameter.epsilon_x);

      tx[i] = 1.0 / (t * m_lx);
      tcx[i] = 1.0 / (t * m_ux);
      tcxstar[i] = 1.0 / (t * m_uxstar);
      txstar[i] = 1.0 / (t * m_lxstar);

      xicxstar[i] = std::max(xi[i] / m_uxstar, parameter.epsilon_x);
      lacx[i] = std::max(la[i] / m_ux, parameter.epsilon_x);
      thex[i] = std::max(the[i] / m_lx, parameter.epsilon_x);
      phixstar[i] = std::max(phi[i] / m_lxstar, parameter.epsilon_x);

      delta[i] = lacx[i] + thex[i];
      delta_star[i] =  xicxstar[i] + phixstar[i];

      //rho = Q(x - x_star)
      rho_x_star[i] = rho_x[i] - parameter.epsilon_svr - value[i] + nu + txstar[i] - tcxstar[i];
      rho_x[i] = -rho_x[i] - parameter.epsilon_svr + value[i] - nu + tx[i] - tcx[i];

      mult[i] = 1 + (delta[i] / delta_star[i]);
      d[i] = 1.0 / (delta[i]);  // note here compute D^{-1} beforehand
    }

    TrainingTimeProfile::update_variables.Stop();

    // Check convergence
    // computes z = H H^T \alpha - tradeoff \alpha
    TrainingTimeProfile::partial_z.Start();
    ComputePartialZ(rbicf, rho_x, rho_x_star, delta_star, local_num_rows, z);
    TrainingTimeProfile::partial_z.Stop();



    // here z stores part of (17) except
    // the last term. Now complete z with
    // intermediates above, i.e. tlx and tux


    for (i = 0; i < local_num_rows; ++i)
      z[i] = rho_x[i] + z[i];

    // Newton Step
    //
    // calculate icfA as E = I + mult * H^T D H

    // ParallelMatrix::PrintMatrix(rbicf);

    TrainingTimeProfile::production.Start();
    MatrixManipulation::ProductMM(rbicf, d, mult, &icfA);
    TrainingTimeProfile::production.Stop();

    // matrix cholesky factorization
    // note, rank is dimension of E, i.e.
    TrainingTimeProfile::cf.Start();
    LLMatrix lra;
    if (myid == 0) {
      MatrixManipulation::CF(icfA, &lra);
    }
    TrainingTimeProfile::cf.Stop();

    // compute dnu = \Sigma^{-1}z, dx = \Sigma^{-1}(z - y \delta\nu), through
    // linear equations trick or Matrix Inversion Lemma
    TrainingTimeProfile::update_variables.Start();
    ComputeDeltaNu(rbicf, d, z, mult, x, x_star, delta, delta_star, rho_x, rho_x_star, lra,
                   local_num_rows, &dnu);
    ComputeDeltaX(rbicf, d, mult, value, dnu, z, lra, local_num_rows, dx);
    lra.Destroy();
    
    // update dxi, dphi, dthe and dla
    for (i = 0; i < local_num_rows; ++i) {
      dx_star[i] = (rho_x[i] + rho_x_star[i] - delta[i] * dx[i])/ delta_star[i];
      dla[i] = tcx[i] - la[i] + lacx[i] * dx[i];
      dxi[i] = tcxstar[i] - xi[i] + xicxstar[i] * dx_star[i];
      dthe[i] = tx[i] - the[i] - thex[i] * dx[i] ;
      dphi[i] = txstar[i] - phi[i] - phixstar[i] * dx_star[i];
    }

    // Line Search
    //
    // line search for primal and dual variable
    double ap = DBL_MAX;
    double ad = DBL_MAX;
    for (i = 0; i < local_num_rows; ++i) {
      // make sure \alpha + \delta\alpha \in [\epsilon, C - \epsilon],
      // note here deal with positive and negative
      // search directionsituations seperately.
      // Refer to chapter 11 in Convex Optimization for more details.
      double c = (value[i] > 0.0) ? c_pos : c_neg;
      if (dx[i]  > 0.0) {
        ap = std::min(ap, (c - x[i]) / dx[i]);
      }
      if (dx[i]  < 0.0) {
        ap = std::min(ap, -x[i]/dx[i]);
      }
      if (dx_star[i]  > 0.0) {
        ap = std::min(ap, (c - x_star[i]) / dx_star[i]);
      }
      if (dx_star[i]  < 0.0) {
        ap = std::min(ap, -x_star[i]/dx_star[i]);
      }
      // make sure \xi+ \delta\xi \in [\epsilon, +\inf), also
      // \lambda + \delta\lambda \in [\epsilon, +\inf).
      // deal with negative search direction.
      // Refer to chapter 11 in Convex Optimization for more details.
      if (dxi[i] < 0.0) {
        ad = std::min(ad, -xi[i] / dxi[i]);
      }
      if (dla[i] < 0.0) {
        ad = std::min(ad, -la[i] / dla[i]);
      }
      if (dthe[i] < 0.0) {
        ad = std::min(ad, -the[i] / dthe[i]);
      }
      if (dphi[i] < 0.0) {
        ad = std::min(ad, -phi[i] / dphi[i]);
      }
    }
    double from_step[2], to_step[2];
    from_step[0] = ap;
    from_step[1] = ad;
    mpi->AllReduce(from_step, to_step,
                   2, MPI_DOUBLE, MPI_MIN, MPI_COMM_WORLD);
    // According to Primal-Dual IPM, the solution must be strictly feasible
    // to inequality constraints, here we add some disturbation to avoid
    // equality, for more details refer to 11.7.3 in Convex Optimization.
    //
    // Note according to 11.7.3 in Convex Optimization, here lack the
    // backsearch phase, but that's not the case, because of linear inequality
    // constraints, we always satisfy f(x^+) \preccurlyeq 0, refer to 11.7.3
    // in Convex Optimization.
    //
    ap = std::min(to_step[0], 1.0) * 0.99;
    ad = std::min(to_step[1], 1.0) * 0.99;
    // Update
    //
    // Update vectors \alpha, \xi, \lambda, and scalar \nu according to Newton
    // step and search direction. This completes one Newton's iteration, refer
    // to Algorithm 11.2 in Convex Optimization.

    cout<<"dx: "<<dx[0]<<" "<<dx[1]<<" "<<dx[2]<<endl;
    cout<<"dx_star: "<<dx_star[0]<<" "<<dx_star[1]<<" "<<dx_star[2]<<endl;
    cout<<"dla: "<<dla[0]<<" "<<dla[1]<<" "<<dla[2]<<endl;
    cout<<"ap: "<<ap<<" ad: "<<ad<<endl;
    for (i = 0; i < local_num_rows; ++i) {
      x[i]  += ap * dx[i];
      x_star[i] += ap * dx_star[i];
      xi[i] += ad * dxi[i];
      la[i] += ad * dla[i];
      the[i] += ad * dthe[i];
      phi[i] += ad * dphi[i];
    }
    nu += ad * dnu;    
    TrainingTimeProfile::update_variables.Stop();
  }
  // Not Convergent in specified iterations.
  // Note there are some other criteria of infeasibility.
  TrainingTimeProfile::ipm_misc.Start();
  if (step >= parameter.max_iter && myid  == 0) {
    cout << StringPrintf("Maximum iterations (%d) has "
              "been reached before convergence,\n",
              parameter.max_iter);
    cout << StringPrintf("Please change the parameters.\n");
  }
  TrainingTimeProfile::ipm_misc.Stop();

  // write back the solutions
  TrainingTimeProfile::check_sv.Start();
  model->CheckSupportVector(x, x_star, doc, parameter);
  TrainingTimeProfile::check_sv.Stop();

  // clean up
  TrainingTimeProfile::ipm_misc.Start();
  delete [] dx;
  delete [] x;
  delete [] x_star;
  delete [] dx_star;
  delete [] xi;
  delete [] la;
  delete [] d;
  delete [] z;
  delete [] dxi;
  delete [] dla;
  delete [] xicxstar;
  delete [] lacx;
  delete [] thex;
  delete [] phixstar;

  delete [] delta;
  delete [] delta_star;
  delete [] rho_x;
  delete [] rho_x_star;

  delete [] mult;

  delete [] tcx;
  delete [] tx;
  delete [] tcxstar;
  delete [] txstar;
  delete [] value;
  TrainingTimeProfile::ipm_misc.Stop();
  return 0;
}

// Compute part of $z$, which is $H^TH\alpha$
int PrimalDualIPM::ComputePartialRho(const ParallelMatrix& icf,
                                   const double *x, const double* x_star,
                                   const int local_num_rows,
                                   const double to, double *rho) {
  register int i, j;
  int p = icf.GetNumCols();
  double *vz = new double[p];
  double *vzpart = new double[p];
  // form vz = V^T*x
  memset(vzpart, 0, sizeof(vzpart[0]) * p);
  double sum;
  for (j = 0; j < p; ++j) {
    sum = 0.0;
    for (i = 0; i < local_num_rows; ++i) {
      sum += icf.Get(i, j) * (x[i] - x_star[i]);
    }
    vzpart[j] = sum;
  }
  ParallelInterface *mpi = ParallelInterface::GetParallelInterface();
  mpi->AllReduce(vzpart, vz, p, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  // form z = V*vz
  for (i = 0; i < local_num_rows; ++i) {
    // Get a piece of inner product
    sum = 0.0;
    for (j = 0; j < p; ++j) {
      sum += icf.Get(i, j) * vz[j];
    }
    rho[i] = sum - to * (x[i] - x_star[i]);
  }

  delete [] vz;
  delete [] vzpart;
  return 0;
}

// Compute part of $z$, which is $H^TH\alpha$
int PrimalDualIPM::ComputePartialZ(const ParallelMatrix& icf,
                                   const double *rho_x, const double* rho_x_star, 
                                   const double* delta_star,
                                   const int local_num_rows,
                                   double *z) {
  register int i, j;
  int p = icf.GetNumCols();
  double *vz = new double[p];
  double *vzpart = new double[p];
  // form vz = V^T*x
  memset(vzpart, 0, sizeof(vzpart[0]) * p);
  double sum;
  for (j = 0; j < p; ++j) {
    sum = 0.0;
    for (i = 0; i < local_num_rows; ++i) {
      sum += icf.Get(i, j) * ((rho_x_star[i] + rho_x[i])/delta_star[i]);
    }
    vzpart[j] = sum;
  }
  ParallelInterface *mpi = ParallelInterface::GetParallelInterface();
  mpi->AllReduce(vzpart, vz, p, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

  // form z = V*vz
  for (i = 0; i < local_num_rows; ++i) {
    // Get a piece of inner product
    sum = 0.0;
    for (j = 0; j < p; ++j) {
      sum += icf.Get(i, j) * vz[j];
    }
    z[i] = sum;
  }

  delete [] vz;
  delete [] vzpart;
  return 0;
}

// Compute surrogate gap
double PrimalDualIPM::ComputeSurrogateGap(double c_pos,
                                        double c_neg,
                                        const double *value,
                                        int local_num_rows,
                                        const double *x,
                                        const double *x_star,
                                        const double *la,
                                        const double *xi,
                                        const double *the,
                                        const double *phi) {
  register int i;
  register double sum = 0.0;
  ParallelInterface* mpi = ParallelInterface::GetParallelInterface();
  // sgap = -<f(x), [la,xi]>
  for (i = 0; i < local_num_rows; ++i) {
    double c = (value[i] > 0.0) ? c_pos : c_neg;
    sum += la[i] * (c - x[i]);
    sum += the[i] * x[i];
    sum += xi[i] * (c - x_star[i]);
    sum += phi[i] * x_star[i];
  }
  double global_sum = 0.0;
  mpi->AllReduce(&sum, &global_sum, 1, MPI_DOUBLE,
                 MPI_SUM, MPI_COMM_WORLD);
  return global_sum;
}



// Compute Newton direction of primal variable $\alpha$
int PrimalDualIPM::ComputeDeltaX(const ParallelMatrix& icf,
                                 const double *d, const double *mult_factor,
                                 const double *value,
                                 const double dnu, const double *z,
                                 const LLMatrix& lra, int local_num_rows,
                                 double *dx) {
  register int i;
  double *tz = new double[local_num_rows];
  // calcuate tz = z-dnu
  for (i = 0; i < local_num_rows; ++i)
    tz[i] = z[i] - dnu;

  // calculate inv(Q+D)*(z-dnu)
  LinearSolveViaICFCol(icf, mult_factor, d, tz, lra, local_num_rows, dx);
  
  // clean up
  delete [] tz;
  return 0;
}

// Compute Newton direction of primal variable $\nu$
int PrimalDualIPM::ComputeDeltaNu(const ParallelMatrix& icf, 
				  const double *d, const double *z,
          const double *mult_factor, 
          const double *x, const double *x_star,
          const double *delta, const double *delta_star,
          const double *rho_x, const double *rho_x_star,
          const LLMatrix& lra, int local_num_rows, 
          double *dnu) {
  register int i;
  double numerator = 0.0;

  double *num_vect = new double[local_num_rows];
  double *den_vect = new double[local_num_rows];
  LinearSolveViaICFCol(icf, mult_factor, d, z, lra, local_num_rows, num_vect);

  double denominator = 0.0;
  double *onevect = new double[local_num_rows];
  for (i = 0; i < local_num_rows; ++i)
    onevect[i] = 1.0;
  
  LinearSolveViaICFCol(icf, mult_factor, d, onevect, lra, local_num_rows, den_vect);

  double temp;
  for (i = 0; i < local_num_rows; ++i) {
    temp = (1 + (delta[i] / delta_star[i]));
    numerator += num_vect[i] * temp + (x[i] - x_star[i]) - ((rho_x[i] + rho_x_star[i])/delta_star[i]);
    denominator += den_vect[i] * temp;
  }

  double from_sum[2], to_sum[2];
  from_sum[0] = numerator;
  from_sum[1] = denominator;
  ParallelInterface* mpi = ParallelInterface::GetParallelInterface();
  mpi->AllReduce(from_sum, to_sum, 2,
                 MPI_DOUBLE, MPI_SUM,
                 MPI_COMM_WORLD);

  // clean up
  delete [] den_vect;
  delete [] num_vect;
  delete [] onevect;
  *dnu = to_sum[0]/ to_sum[1];
  return 0;
}

// solve a linear system via Sherman-Morrison-Woodbery formula
int PrimalDualIPM::LinearSolveViaICFCol(const ParallelMatrix& icf,
                                        const double *mult_factor,
                                        const double *d,
                                        const double *b,
                                        const LLMatrix& lra,
                                        int local_num_rows,
                                        double *x) {
  // Solve (D+VV')x = b using ICF and SMW update
  // V(dimxrank) : input matrix (smatrix)
  // D(dim)      : diagonal matrix in vector
  // b(dim)      : target vector
  // rank        : rank of ICF matrix
  register int i, j;
  int p = icf.GetNumCols();
  double *vz = new double[p];
  double *vzpart = new double[p];
  double *z  = new double[local_num_rows];
  // we already inversed matrix before
  // calculate z=inv(D)*b[idx]
  for (i = 0; i < local_num_rows; ++i)
    z[i] = b[i] * d[i];
  // form vz = V^T*z
  memset(vzpart, 0, sizeof(vzpart[0]) * p);
  double sum;
  for (j = 0; j < p; ++j) {
    sum = 0.0;
    for (i = 0; i < local_num_rows; ++i) {
      sum += icf.Get(i, j) * z[i];
    }
    vzpart[j] = sum;
  }
  ParallelInterface* mpi = ParallelInterface::GetParallelInterface();
  mpi->Reduce(vzpart, vz, p, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
  int myid = mpi->GetProcId();
  if (myid == 0) {
    double *ty = new double[p];
    MatrixManipulation::CholForwardSub(lra, vz, ty);
    MatrixManipulation::CholBackwardSub(lra, ty, vz);
    delete [] ty;
  }
  mpi->Bcast(vz, p, MPI_DOUBLE, 0, MPI_COMM_WORLD);
  // calculate u = z - inv(D)*V*t
  for (i = 0; i < local_num_rows; ++i) {
    sum = 0.0;
    for (j = 0; j < p; ++j) {
      sum += icf.Get(i, j) * vz[j] * d[i];
    }
    x[i] = z[i] - sum * mult_factor[i];
  }
  // clean up
  delete [] z;
  delete [] vz;
  delete [] vzpart;
  return 0;
}

// Loads the values of alpha, xi, lambda and nu to resume from an interrupted
// solving process.
void PrimalDualIPM::LoadVariables(
    const PrimalDualIPMParameter& parameter,
    int num_local_doc, int num_total_doc, int *step,
    double* nu, double *x, double *x_star, double *la,
    double *xi, double* the, double* phi) {

  ParallelInterface *interface = ParallelInterface::GetParallelInterface();
  char path[MAX_PATH_LEN];
  int my_id = interface->GetProcId();
  int num_processors = interface->GetNumProcs();

  snprintf(path, sizeof(path), "%s/variables.saved_step", parameter.model_path);
  if (File::Exists(path)) {
    cout << "Intermedia Results found: " << path;
    Timer load_timer;
    load_timer.Start();
    int last_step = 0;
    File *file = File::OpenOrDie(path, "r");
    file->ReadOrDie(&last_step, sizeof(last_step));
    CHECK(file->Close());
    delete file;
    cout << "Resuming from step " << last_step << " ...";

    snprintf(path, sizeof(path), "%s/variables_step%05d.%d",
             parameter.model_path, last_step, my_id);
    file = File::OpenOrDie(path, "r");

    int old_num_local_doc, old_num_total_doc, old_num_processors;
    CHECK(file->Read(step, sizeof(*step)) == sizeof(*step));
    CHECK(file->Read(&old_num_local_doc, sizeof(old_num_local_doc)) ==
          sizeof(old_num_local_doc));
    CHECK(file->Read(&old_num_total_doc, sizeof(old_num_total_doc)) ==
          sizeof(old_num_total_doc));
    CHECK(file->Read(&old_num_processors, sizeof(old_num_processors)) ==
          sizeof(old_num_processors));
    CHECK(old_num_processors == num_processors);
    CHECK(old_num_local_doc == num_local_doc);
    CHECK(old_num_total_doc == num_total_doc);

    CHECK(file->Read(nu, sizeof(nu[0]) * num_local_doc) ==
          sizeof(nu[0]) * num_local_doc);
    CHECK(file->Read(x, sizeof(x[0]) * num_local_doc) ==
          sizeof(x[0]) * num_local_doc);
    CHECK(file->Read(x_star, sizeof(x_star[0]) * num_local_doc) ==
          sizeof(x_star[0]) * num_local_doc);
    CHECK(file->Read(la, sizeof(la[0]) * num_local_doc) ==
          sizeof(la[0]) * num_local_doc);
    CHECK(file->Read(xi, sizeof(xi[0]) * num_local_doc) ==
          sizeof(xi[0]) * num_local_doc);
    CHECK(file->Read(the, sizeof(the[0]) * num_local_doc) ==
          sizeof(the[0]) * num_local_doc);
    CHECK(file->Read(phi, sizeof(phi[0]) * num_local_doc) ==
          sizeof(phi[0]) * num_local_doc);

    CHECK(file->Close());
    delete file;
    load_timer.Stop();
    cout << "IPM resumed in " << load_timer.total() << " seconds" << endl;
  }
}

// Saves the values of alpha, xi, lambda and nu. num_local_doc, num_total_doc
// and num_processors are also saved to facilitate the loading procedure.
void PrimalDualIPM::SaveVariables(
    const PrimalDualIPMParameter& parameter,
    int num_local_doc, int num_total_doc, int step,
    double nu, double *x, double *x_star, double *la,
    double *xi, double* the, double* phi) {
  Timer save_timer;
  save_timer.Start();
  ParallelInterface *interface = ParallelInterface::GetParallelInterface();
  char path[MAX_PATH_LEN];
  int my_id = interface->GetProcId();
  int num_processors = interface->GetNumProcs();
  int last_step = -1;
  File* file;

  snprintf(path, sizeof(path), "%s/variables.saved_step", parameter.model_path);
  if (File::Exists(path)) {
    file = File::OpenOrDie(path, "r");
    file->ReadOrDie(&last_step, sizeof(last_step));
    CHECK(file->Close());
    delete file;
  }
  if (step == last_step) return;

  cout << "Saving variables ... " << endl;
  snprintf(path, sizeof(path), "%s/variables_step%05d.%d",
           parameter.model_path, step, my_id);
  file = File::OpenOrDie(path, "w");

  CHECK(file->Write(&step, sizeof(step)) == sizeof(step));
  CHECK(file->Write(&num_local_doc, sizeof(num_local_doc)) ==
        sizeof(num_local_doc));
  CHECK(file->Write(&num_total_doc, sizeof(num_total_doc)) ==
        sizeof(num_total_doc));
  CHECK(file->Write(&num_processors, sizeof(num_processors)) ==
        sizeof(num_processors));

  CHECK(file->Write(&nu, sizeof(nu)) == sizeof(nu));
  CHECK(file->Write(x, sizeof(x[0]) * num_local_doc) ==
        sizeof(x[0]) * num_local_doc);
  CHECK(file->Write(x_star, sizeof(x_star[0]) * num_local_doc) ==
        sizeof(x_star[0]) * num_local_doc);
  CHECK(file->Write(la, sizeof(la[0]) * num_local_doc) ==
        sizeof(la[0]) * num_local_doc);
  CHECK(file->Write(xi, sizeof(xi[0]) * num_local_doc) ==
        sizeof(xi[0]) * num_local_doc);
  CHECK(file->Write(the, sizeof(the[0]) * num_local_doc) ==
        sizeof(the[0]) * num_local_doc);
  CHECK(file->Write(phi, sizeof(phi[0]) * num_local_doc) ==
        sizeof(phi[0]) * num_local_doc);

  CHECK(file->Flush());
  CHECK(file->Close());
  delete file;
  interface->Barrier(MPI_COMM_WORLD);
  if (my_id == 0) {
    snprintf(path, sizeof(path), "%s/variables.saved_step",
             parameter.model_path);
    file = File::OpenOrDie(path, "w");
    file->WriteOrDie(&step, sizeof(step));
    CHECK(file->Flush());
    CHECK(file->Close());
    delete file;
  }
  interface->Bcast(&last_step, 1, MPI_INT, 0, MPI_COMM_WORLD);
  if (last_step != -1) {
    snprintf(path, sizeof(path), "%s/variables_step%05d.%d",
             parameter.model_path, last_step, my_id);
    CHECK(file->Delete(path));
  }

  save_timer.Stop();
  cout << "Variables saved in " << save_timer.total()
            << " seconds" << endl;
}
}
