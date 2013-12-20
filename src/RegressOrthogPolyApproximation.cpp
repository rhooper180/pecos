/*  _______________________________________________________________________

    PECOS: Parallel Environment for Creation Of Stochastics
    Copyright (c) 2011, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Pecos directory.
    _______________________________________________________________________ */

//- Class:        RegressOrthogPolyApproximation
//- Description:  Implementation code for RegressOrthogPolyApproximation class
//-               
//- Owner:        John Jakeman

#include "RegressOrthogPolyApproximation.hpp"
#include "SharedRegressOrthogPolyApproxData.hpp"
#include "pecos_global_defs.hpp"
#include "Teuchos_LAPACK.hpp"
#include "Teuchos_SerialDenseHelpers.hpp"

// headers necessary for cross validation
#include "MathTools.hpp"
#include "CrossValidationIterator.hpp"
#include "LinearModelModules.hpp"
#include "CrossValidationModules.hpp"

//#define DEBUG

namespace Pecos {


int RegressOrthogPolyApproximation::min_coefficients() const
{
  // return the minimum number of data instances required to build the 
  // surface of multiple dimensions
  if (expansionCoeffFlag || expansionCoeffGradFlag)
    // Now that L1-regression has been implemented. There is no longer a need 
    // to enforce a lower bound on the number of data instances.
    return 1;
    // multiIndex is computed by allocate_arrays() in compute_coefficients(),
    // which is too late for use of this fn by ApproximationInterface::
    // minimum_samples() in DataFitSurrModel::build_global(), so number of
    // expansion terms must be calculated.
    //return total_order_terms(approxOrder);
  else
    return 0;
}


void RegressOrthogPolyApproximation::allocate_arrays()
{
  allocate_total_sobol(); // no dependencies

  // multiIndex size needed to set faultInfo.under_determined
  // Note: OLI's multiIndex is empty, but must ignore this flag
  set_fault_info();

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  if (data_rep->expConfigOptions.expCoeffsSolnApproach ==
      ORTHOG_LEAST_INTERPOLATION) {
    PCout << "Orthogonal polynomial approximation of least order\n";

    if (data_rep->expConfigOptions.vbdFlag &&
	data_rep->expConfigOptions.vbdOrderLimit == 1)
      allocate_component_sobol(); // no dependence on multiIndex for order 1
    // else defer until transform_least_interpolant()

    //size_expansion(); // defer until transform_least_interpolant()
  }
  else if (faultInfo.under_determined) {
    // under-determined regression: defer allocations until sparsity known

    if (data_rep->expConfigOptions.vbdFlag && 
	data_rep->expConfigOptions.vbdOrderLimit == 1)
      allocate_component_sobol(); // no dependence on multiIndex for order 1
    // else defer until sparse recovery

    //size_expansion(); // defer until sparse recovery
  }
  else { // pre-allocate total-order expansion
    // uniquely/over-determined regression: perform allocations now

    allocate_component_sobol();

    // size expansion even if !update_exp_form due to possibility of change
    // to expansion{Coeff,GradFlag} settings
    size_expansion();
  }

  if (expansionMoments.empty())
    expansionMoments.sizeUninitialized(2);
}


void RegressOrthogPolyApproximation::allocate_component_sobol()
{
  if (sparseIndices.empty())
    PolynomialApproximation::allocate_component_sobol();
  else {
    SharedRegressOrthogPolyApproxData* data_rep
      = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
    const UShort2DArray& multi_index = data_rep->multiIndex;
    BitArrayULongMap& sobol_map = data_rep->sobolIndexMap;
    SizetSet::const_iterator cit;
    size_t j, num_v = sharedDataRep->numVars;
    BitArray set(num_v);
    for (cit=++sparseIndices.begin(); cit!=sparseIndices.end(); ++cit) {
      const UShortArray& sparse_mi = multi_index[*cit];
      for (j=0; j<num_v; ++j)
	if (sparse_mi[j]) set.set(j);   //   activate bit j
	else              set.reset(j); // deactivate bit j
      sparseSobolIndices.insert(sobol_map[set]); // discard duplicate indices
      // TO DO: may be better to define a map from shared index to sparse index.
      // the map value (sparse index) would be the position in the sorted sparse
      // set --> issue is whether to incur this distance() overhead each time.
    }
    // TO DO: these changes need to be propagated elsewhere, potentially with
    // additional fn specializations:
    // > sobolIndexMap is now insufficient to describe component sobol indices
    //   in all cases
    // > multiIndex is now insufficient to describe expansion terms in all cases
    size_t sobol_len = sparseSobolIndices.size();
    if (sobolIndices.length() != sobol_len)
      sobolIndices.sizeUninitialized(sobol_len);
  }
}


void RegressOrthogPolyApproximation::compute_coefficients()
{
  if (!expansionCoeffFlag && !expansionCoeffGradFlag) {
    PCerr << "Warning: neither expansion coefficients nor expansion "
	  << "coefficient gradients\n         are active in "
	  << "RegressOrthogPolyApproximation::compute_coefficients().\n"
	  << "         Bypassing approximation construction." << std::endl;
    return;
  }

  // For testing of anchor point logic:
  //size_t index = surrData.size() - 1;
  //surrData.anchor_point(surrData.variables_data()[index],
  //                      surrData.response_data()[index]);
  //surrData.pop(1);

  // anchor point, if present, is handled differently for different
  // expCoeffsSolnApproach settings:
  //   SAMPLING:   treat it as another data point
  //   QUADRATURE/CUBATURE/COMBINED_SPARSE_GRID: error
  //   LEAST_SQ_REGRESSION: use equality-constrained least squares
  size_t i, j, num_total_pts = surrData.size();
  if (surrData.anchor())
    ++num_total_pts;
  if (!num_total_pts) {
    PCerr << "Error: nonzero number of sample points required in RegressOrthog"
	  << "PolyApproximation::compute_coefficients()." << std::endl;
    abort_handler(-1);
  }

#ifdef DEBUG
  gradient_check();
#endif // DEBUG

  // check data set for gradients/constraints/faults to determine settings
  surrData.data_checks();

  allocate_arrays();
  //select_solver();
  run_regression(); // calculate PCE coefficients

  computedMean = computedVariance = 0;
}


/** In this case, regression is used in place of spectral projection.  That
    is, instead of calculating the PCE coefficients using inner products, 
    linear least squares is used to estimate the PCE coefficients which
    best match a set of response samples.  The least squares estimation is
    performed using DGELSS (SVD) or DGGLSE (equality-constrained) from
    LAPACK, based on anchor point and derivative data availability. */
void RegressOrthogPolyApproximation::select_solver()
{
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  CSOpts.solver = data_rep->expConfigOptions.expCoeffsSolnApproach;
  bool fn_constrained_lls
    = (data_rep->basisConfigOptions.useDerivs && faultInfo.constr_eqns &&
       faultInfo.constr_eqns < data_rep->expansion_terms()); // candidate exp
  bool eq_con
    = (fn_constrained_lls || faultInfo.anchor_fn || faultInfo.anchor_grad);
  if (CSOpts.solver==DEFAULT_REGRESSION) {
    if (faultInfo.under_determined)
      CSOpts.solver = LASSO_REGRESSION;
    else
      CSOpts.solver = (eq_con) ? EQ_CON_LEAST_SQ_REGRESSION :
	SVD_LEAST_SQ_REGRESSION;
  }
  else if (CSOpts.solver==DEFAULT_LEAST_SQ_REGRESSION)
    CSOpts.solver = (eq_con && !faultInfo.under_determined) ?
      EQ_CON_LEAST_SQ_REGRESSION : SVD_LEAST_SQ_REGRESSION;

  // Set solver parameters
  RealVector& noise_tols = data_rep->noiseTols;
  if ( CSOpts.solver == LASSO_REGRESSION )
    CSOpts.delta = data_rep->l2Penalty;
  if ( noise_tols.length() > 0 )
    CSOpts.epsilon = noise_tols[0];
  else {
    noise_tols.size( 1 );
    noise_tols[0] = CSOpts.epsilon;
    if ( CSOpts.solver == BASIS_PURSUIT_DENOISING ) noise_tols[0] = 1e-3;
  }
  CSOpts.solverTolerance = (CSOpts.solver == SVD_LEAST_SQ_REGRESSION)
    ? -1.0 : data_rep->expConfigOptions.convergenceTol;
  CSOpts.verbosity = std::max(0, data_rep->expConfigOptions.outputLevel - 1);
  if ( data_rep->expConfigOptions.maxIterations > 0 )
    CSOpts.maxNumIterations = data_rep->expConfigOptions.maxIterations;

  // Solve the regression problem using L1 or L2 minimization approaches
  //bool regression_err = 0;
  if (CSOpts.solver==EQ_CON_LEAST_SQ_REGRESSION && !data_rep->crossValidation){
    if ( eq_con && !faultInfo.under_determined )
      CSOpts.numFunctionSamples = surrData.size();
    else {
      PCout << "Could not perform equality constrained least-squares. ";
      if (faultInfo.under_determined) {
	CSOpts.solver = LASSO_REGRESSION;
	PCout << "Using LASSO regression instead\n";
      }
      else {
	CSOpts.solver = SVD_LEAST_SQ_REGRESSION;
	PCout << "Using SVD least squares regression instead\n";
      }
      //regression_err = L2_regression(num_data_pts_fn, num_data_pts_grad, reuse_solver_data);
    }
  }
  //else
    //regression_err = L2_regression(num_data_pts_fn, num_data_pts_grad, reuse_solver_data);

  //if (regression_err) { // if numerical problems in LLS, abort with error
  //  PCerr << "Error: nonzero return code from least squares solution in "
  //        << "RegressOrthogPolyApproximation::regression()" << std::endl;
  //  abort_handler(-1);
  //}
}


void RegressOrthogPolyApproximation::set_fault_info()
{
  size_t constr_eqns, anchor_fn, anchor_grad, num_data_pts_fn,
    num_data_pts_grad, total_eqns, num_surr_data_pts;
  bool under_determined = false, reuse_solver_data;

  // compute order of data contained within surrData
  short data_order = (expansionCoeffFlag) ? 1 : 0;
  if (surrData.anchor()) {
    if (!surrData.anchor_gradient().empty())     data_order |= 2;
    //if (!surrData.anchor_hessian().empty())    data_order |= 4;
  }
  else {
    if (!surrData.response_gradient(0).empty())  data_order |= 2;
    //if (!surrData.response_hessian(0).empty()) data_order |= 4;
  }
  // verify support for basisConfigOptions.useDerivs, which indicates usage of
  // derivative data with respect to expansion variables (aleatory or combined)
  // within the expansion coefficient solution process, which must be
  // distinguished from usage of derivative data with respect to non-expansion
  // variables (the expansionCoeffGradFlag case).
  bool config_err = false;
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  if (data_rep->basisConfigOptions.useDerivs) {
    if (!(data_order & 2)) {
      PCerr << "Error: useDerivs configuration option lacks data support in "
	    << "RegressOrthogPolyApproximation::regression()" << std::endl;
      config_err = true;
    }
    if (expansionCoeffGradFlag) {
      PCerr << "Error: useDerivs configuration option conflicts with gradient "
	    << "expansion request in RegressOrthogPolyApproximation::"
	    << "regression()" << std::endl;
      config_err = true;
    }
    //if (data_order & 4)
    //  PCerr << "Warning: useDerivs configuration option does not yet support "
    //	      << "Hessian data in RegressOrthogPolyApproximation::regression()"
    //	      << std::endl;
  }
  if (config_err)
    abort_handler(-1);

  // compute data counts
  const SizetShortMap& failed_resp_data = surrData.failed_response_data();
  size_t num_failed_surr_fn = 0, num_failed_surr_grad = 0,
    num_v = sharedDataRep->numVars;
  SizetShortMap::const_iterator fit; bool faults_differ = false;
  for (fit=failed_resp_data.begin(); fit!=failed_resp_data.end(); ++fit) {
    short fail_bits = fit->second;
    if (fail_bits & 1) ++num_failed_surr_fn;
    if (fail_bits & 2) ++num_failed_surr_grad;
    // if failure omissions are not consistent, manage differing Psi matrices
    if ( (fail_bits & data_order) != data_order ) faults_differ = true;
  }
  num_surr_data_pts = surrData.size();
  num_data_pts_fn   = num_surr_data_pts - num_failed_surr_fn;
  num_data_pts_grad = num_surr_data_pts - num_failed_surr_grad;
  anchor_fn = false;
  anchor_grad = false;
  if (surrData.anchor()) {
    short failed_anchor_data = surrData.failed_anchor_data();
    if ((data_order & 1) && !(failed_anchor_data & 1)) anchor_fn   = true;
    if ((data_order & 2) && !(failed_anchor_data & 2)) anchor_grad = true;
  }

  // detect underdetermined system of equations (following fault omissions)
  // for either expansion coeffs or coeff grads (switch logic treats together)
  reuse_solver_data
    = (expansionCoeffFlag && expansionCoeffGradFlag && !faults_differ);
  constr_eqns = 0;
  if (expansionCoeffFlag) {
    constr_eqns = num_data_pts_fn;
    if (anchor_fn)   constr_eqns += 1;
    if (anchor_grad) constr_eqns += num_v;
    total_eqns = (data_rep->basisConfigOptions.useDerivs) ?
      constr_eqns + num_data_pts_grad*num_v : constr_eqns;
    if (total_eqns < data_rep->expansion_terms()) // candidate expansion size
      under_determined = true;
  }
  if (expansionCoeffGradFlag) {
    total_eqns = (anchor_grad) ? num_data_pts_grad+1 : num_data_pts_grad;
    if (total_eqns < data_rep->expansion_terms()) // candidate expansion size
      under_determined = true;
  }

  faultInfo.set_info( constr_eqns, anchor_fn, anchor_grad,
		      under_determined, num_data_pts_fn, num_data_pts_grad,
		      reuse_solver_data, total_eqns, num_surr_data_pts,
		      num_v, data_rep->basisConfigOptions.useDerivs,
		      surrData.num_derivative_variables() );
};


void RegressOrthogPolyApproximation::run_regression()
{
  // Assume all function values are stored in top block of matrix in rows
  // 0 to num_surr_data_pts-1. Gradient information will be stored
  // in the bottom block of the matrix in rows num_surr_data_pts to
  // num_surr_data_pts + num_data_pts_grad * num_v. All the gradient 
  // information of point 0 will be stored consecutively then all the gradient
  // data of point 1, and so on.

  // Currently nothing is done  to modify the regression linear system matrices
  // A and B if surrData.anchor() is true, as currently surrData.anchor()
  // is always false. If in the future surrData.anchor() is enabled then
  // A must be adjusted to include the extra constraint information associated
  // with the anchor data. That is, if using EQ_CON_LEAST_SQUARES C matrix 
  // (top block of A ) must contain the fn and grad data of anchor point.
  // This will violate the first assumption discussed above and effect cross
  // validation. For this reason no modification is made as yet.

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;

  size_t i, j, a_cntr = 0, b_cntr = 0, num_surr_data_pts = surrData.size(),
    num_deriv_vars = surrData.num_derivative_variables(),
    num_v = sharedDataRep->numVars;
  int num_rows_A =  0, // number of rows in matrix A
      num_cols_A = data_rep->expansion_terms(), // candidate expansion size
      num_coeff_rhs, num_grad_rhs = num_deriv_vars, num_rhs;
  bool add_val, add_grad;

  RealMatrix A, B;
  
  bool multiple_rhs = (expansionCoeffFlag && expansionCoeffGradFlag);

  bool anchor_fn = false, anchor_grad = false;
  if (surrData.anchor())
    anchor_fn = anchor_grad = true;

  int num_data_pts_fn = num_surr_data_pts, 
    num_data_pts_grad = num_surr_data_pts;

  size_t a_grad_cntr = 0, b_grad_cntr = 0;

  CompressedSensingOptionsList opts_list;
  RealMatrixArray solutions;
  CSOpts.standardizeInputs = false; // false essential when using derivatives

  RealMatrix points( num_v, num_surr_data_pts, false );
  for (i=0; i<num_surr_data_pts; ++i) {
    for (j=0;j<num_v;j++)
      points(j,i) = surrData.continuous_variables(i)[j];
  }
  
  if (expansionCoeffFlag) {

    // matrix/vector sizing
    num_rows_A = (data_rep->basisConfigOptions.useDerivs) ?
      num_data_pts_fn + num_data_pts_grad * num_v : num_data_pts_fn;
    num_coeff_rhs = 1;
    num_rhs = (multiple_rhs) ? num_coeff_rhs + num_grad_rhs : num_coeff_rhs;

    A.shapeUninitialized(num_rows_A,num_cols_A);
    B.shapeUninitialized(num_rows_A,num_rhs);
    Real *A_matrix = A.values(), *b_vectors = B.values();
    // The "A" matrix is a contiguous block of memory packed in column-major
    // ordering as required by F77 for the GELSS subroutine from LAPACK.  For
    // example, the 6 elements of A(2,3) are stored in the order A(1,1),
    // A(2,1), A(1,2), A(2,2), A(1,3), A(2,3).
    for (i=0; i<num_cols_A; ++i) {
      a_cntr = num_rows_A*i;
      a_grad_cntr = a_cntr + num_data_pts_fn;
      const UShortArray& mi_i = data_rep->multiIndex[i];
      for (j=0;j<num_surr_data_pts; ++j) {
	add_val = true; add_grad = data_rep->basisConfigOptions.useDerivs;
	data_rep->pack_polynomial_data(surrData.continuous_variables(j), mi_i,
				       add_val, A_matrix, a_cntr, add_grad,
				       A_matrix, a_grad_cntr);
      }
    }
    
    // response data (values/gradients) define the multiple RHS which are
    // matched in the LS soln.  b_vectors is num_data_pts (rows) x num_rhs
    // (cols), arranged in column-major order.
    b_cntr = 0;
    b_grad_cntr = num_data_pts_fn;
    const SDRArray& sdr_array = surrData.response_data();
    for (i=0; i<num_surr_data_pts; ++i) {
      add_val = true; add_grad = data_rep->basisConfigOptions.useDerivs;
      data_rep->pack_response_data(sdr_array[i], add_val, b_vectors, b_cntr,
				   add_grad, b_vectors, b_grad_cntr);
    }

    // if no RHS augmentation, then solve for coeffs now
    if (!multiple_rhs) {

      // Perform cross validation loop over degrees here.
      // Current cross validation will not work for equality 
      // constrained least squares
      if ( data_rep->crossValidation ) // run cross validation
	{
	  if ( data_rep->expConfigOptions.expCoeffsSolnApproach == 
	       ORTHOG_LEAST_INTERPOLATION )
	    throw( std::runtime_error("Cannot use cross validation with least interpolation") );
	  select_solver();
	  run_cross_validation( A, B, points, num_data_pts_fn );
	}
      else {
	if ( data_rep->expConfigOptions.expCoeffsSolnApproach != 
	     ORTHOG_LEAST_INTERPOLATION )
	  {
	    IntVector index_mapping; 
	    RealMatrix points_dummy;
	    remove_faulty_data( A, B, points_dummy, index_mapping, faultInfo,
				surrData.failed_response_data() );
	    faultInfo.under_determined = num_rows_A < num_cols_A;
	    PCout << "Applying regression to compute " << num_cols_A << " chaos"
		  << " coefficients using " << num_rows_A << " equations.\n";
	    select_solver();
	    data_rep->CSTool.solve( A, B, solutions, CSOpts, opts_list );
	    
	    if (faultInfo.under_determined) // exploit CS sparsity
	      update_sparse(solutions[0][0], num_cols_A);
	    else {                          // retain full solution
	      copy_data(solutions[0][0], num_cols_A, expansionCoeffs);
	      sparseIndices.clear();
	    }
	  }
	else
	  {
	    IntVector index_mapping; 
	    remove_faulty_data( A, B, points, index_mapping, faultInfo,
				surrData.failed_response_data() );
	    faultInfo.under_determined = false;
	    PCout << "using least interpolation\n";
	    least_interpolation( points, B );
	  }
      }
    }
  }

  if (expansionCoeffGradFlag) {
    if (!multiple_rhs) {
      num_rows_A = num_data_pts_grad;
      num_rhs    = num_grad_rhs; num_coeff_rhs = 0;
      A.shapeUninitialized(num_rows_A,num_cols_A);
      B.shapeUninitialized(num_rows_A,num_rhs);
      Real *A_matrix   = A.values();

      // repack "A" matrix with different Psi omissions
      a_cntr = 0;
      for (i=0; i<num_cols_A; ++i) {
	const UShortArray& mi_i = data_rep->multiIndex[i];
	for (j=0; j<num_surr_data_pts; ++j) {
	  add_val = false; add_grad = true;
	  if (add_grad) {
	    A_matrix[a_cntr] = data_rep->
	      multivariate_polynomial(surrData.continuous_variables(j), mi_i);
	    ++a_cntr;
	  }
	}
      }
    }
    
    PCout << "Applying regression to compute gradients of " << num_cols_A
	  << " chaos coefficients using " << num_rows_A << " equations.\n";

    // response data (values/gradients) define the multiple RHS which are
    // matched in the LS soln.  b_vectors is num_data_pts (rows) x num_rhs
    // (cols), arranged in column-major order.
    Real *b_vectors  = B.values();
    b_cntr = 0;
    for (i=0; i<num_surr_data_pts; ++i) {
      add_val = false; add_grad = true;
      if (add_grad) {
	const RealVector& resp_grad = surrData.response_gradient(i);
	for (j=0; j<num_grad_rhs; ++j) // i-th point, j-th grad component
	  b_vectors[(j+num_coeff_rhs)*num_data_pts_grad+b_cntr] = resp_grad[j];
	++b_cntr;
      }
    }

    if (data_rep->expConfigOptions.expCoeffsSolnApproach != ORTHOG_LEAST_INTERPOLATION) {
      // solve
      IntVector index_mapping; 
      remove_faulty_data( A, B, points, index_mapping,
			  faultInfo, surrData.failed_response_data());
      faultInfo.under_determined = num_rows_A < num_cols_A;
      PCout << "Applying regression to compute " << num_cols_A
	    << " chaos coefficients using " << num_rows_A << " equations.\n";
      select_solver();
      data_rep->CSTool.solve( A, B, solutions, CSOpts, opts_list );
    }
    else {
      IntVector index_mapping; 
      remove_faulty_data( A, B, points, index_mapping, faultInfo,
			  surrData.failed_response_data() );
      faultInfo.under_determined = false;
      PCout << "using least interpolation\n";
      least_interpolation( points, B );
    }

    if (faultInfo.under_determined) { // exploit CS sparsity
      // overlay sparse solutions into an aggregated set of sparse indices
      sparseIndices.clear();
      if (multiple_rhs)
	update_sparse_indices(solutions[0][0], num_cols_A);
      for (i=0; i<num_grad_rhs; ++i)
	update_sparse_indices(solutions[i+num_coeff_rhs][0], num_cols_A);
      // update expansion{Coeffs,CoeffGrads} from sparse indices
      if (multiple_rhs)
	update_sparse_coeffs(solutions[0][0]);
      for (i=0; i<num_grad_rhs; ++i)
	update_sparse_coeff_grads(solutions[i+num_coeff_rhs][0], i);
      // update sobol index bookkeeping
      update_sparse_sobol();//update_sparse_multi_index();
    }
    else { // retain original multiIndex layout
      if (multiple_rhs)
	copy_data(solutions[0][0], num_cols_A, expansionCoeffs);
      for (i=0; i<num_grad_rhs; ++i) {
 	Real* dense_coeffs = solutions[i+num_coeff_rhs][0];
 	for (j=0; j<num_cols_A; ++j)
	  expansionCoeffGrads(i,j) = dense_coeffs[j];
      }
      sparseIndices.clear();
    }
  }
}


Real RegressOrthogPolyApproximation::
covariance(PolynomialApproximation* poly_approx_2)
{
  RegressOrthogPolyApproximation* ropa_2
    = (RegressOrthogPolyApproximation*)poly_approx_2;
  if (sparseIndices.empty()) // TO DO: support mixed case?
    return OrthogPolyApproximation::covariance(poly_approx_2);
  else if (ropa_2 == this) {
    // Error check for required data
    if ( !expansionCoeffFlag ) {
      PCerr << "Error: expansion coefficients not defined in "
	    << "RegressOrthogPolyApproximation::covariance()" << std::endl;
      abort_handler(-1);
    }
    SharedRegressOrthogPolyApproxData* data_rep
      = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
    const UShort2DArray& multi_index = data_rep->multiIndex; // shared
    SizetSet::const_iterator cit; size_t i; Real covar = 0.;
    for (cit=++sparseIndices.begin(), i=1; cit!=sparseIndices.end(); ++cit, ++i)
      covar += expansionCoeffs[i] * expansionCoeffs[i] *
	data_rep->norm_squared(multi_index[*cit]);
    return covar;
  }
  else { // for sparse PCE, multiIndex sequences may differ for different resp
    // Error check for required data
    if ( !expansionCoeffFlag || !ropa_2->expansionCoeffFlag ) {
      PCerr << "Error: expansion coefficients not defined in "
	    << "RegressOrthogPolyApproximation::covariance()" << std::endl;
      abort_handler(-1);
    }
    SharedRegressOrthogPolyApproxData* data_rep
      = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
    const UShort2DArray& multi_index = data_rep->multiIndex; // shared
    const RealVector& exp_coeffs_2 = ropa_2->expansionCoeffs;
    const SizetSet&   sparse_ind_2 = ropa_2->sparseIndices;
    SizetSet::const_iterator cit1 = ++sparseIndices.begin(),
      cit2 = ++sparse_ind_2.begin();
    size_t si1, si2, i1 = 1, i2 = 1;
    Real covar = 0.;
    while (cit1 != sparseIndices.end() && cit2 != sparse_ind_2.end()) {
      si1 = *cit1; si2 = *cit2;
      if (si1 == si2) { // equality in sparse index implies multiIndex equality
	covar += expansionCoeffs[i1] * exp_coeffs_2[i2] *
	  data_rep->norm_squared(multi_index[si1]); // or multi_index[si2]
	++cit1; ++cit2; ++i1; ++i2;
      }
      else if (si1 < si2) { ++cit1; ++i1; }
      else                { ++cit2; ++i2; }
    }
    return covar;
  }
}


Real RegressOrthogPolyApproximation::
covariance(const RealVector& x, PolynomialApproximation* poly_approx_2)
{
  if (sparseIndices.empty()) // full multiIndex in use for all resp fns
    return OrthogPolyApproximation::covariance(x, poly_approx_2);
  // TO DO: support mixed cases of sparseIndices for 1 resp but not another?

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  RegressOrthogPolyApproximation* ropa_2
    = (RegressOrthogPolyApproximation*)poly_approx_2;
  bool same = (this == ropa_2), all_mode = !data_rep->nonRandomIndices.empty();

  // Error check for required data
  if ( !expansionCoeffFlag ||
       ( !same && !ropa_2->expansionCoeffFlag )) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::covariance()" << std::endl;
    abort_handler(-1);
  }

  if ( same && all_mode && (computedVariance & 1) &&
       data_rep->match_nonrandom_vars(x, xPrevVar) )
    return expansionMoments[1];

  const UShort2DArray& multi_index = data_rep->multiIndex; // shared
  const SizetList&        rand_ind = data_rep->randomIndices;
  const SizetList&       nrand_ind = data_rep->nonRandomIndices;
  const RealVector&   exp_coeffs_2 = ropa_2->expansionCoeffs;
  const SizetSet&     sparse_ind_2 = ropa_2->sparseIndices;
  size_t i1, i2, si1, si2; Real covar = 0.;
  SizetSet::const_iterator cit1, cit2;
  for (cit1=++sparseIndices.begin(), i1=1; cit1!=sparseIndices.end();
       ++cit1, ++i1) {
    // For r = random_vars and nr = non_random_vars,
    // sigma^2_R(nr) = < (R(r,nr) - \mu_R(nr))^2 >_r
    // -> only include terms from R(r,nr) which don't appear in \mu_R(nr)
    si1 = *cit1;
    if (!data_rep->zero_random(multi_index[si1])) {
      Real coeff_norm_poly = expansionCoeffs[i1] * 
	data_rep->norm_squared(multi_index[si1], rand_ind) *
	data_rep->multivariate_polynomial(x, multi_index[si1], nrand_ind);
      const UShortArray& mi1 = multi_index[si1];
      for (cit2=++sparse_ind_2.begin(), i2=1; cit2!=sparse_ind_2.end();
	   ++cit2, ++i2) {
	si2 = *cit2;
	// random polynomial part must be identical to contribute to variance
	// (else orthogonality drops term).  Note that it is not necessary to
	// collapse terms with the same random basis subset, since cross term
	// in (a+b)(a+b) = a^2+2ab+b^2 gets included.  If terms were collapsed
	// (following eval of non-random portions), the nested loop could be
	// replaced with a single loop to evaluate (a+b)^2.
	if (data_rep->match_random_key(mi1, multi_index[si2]))
	  covar += coeff_norm_poly * exp_coeffs_2[i2] * 
	    data_rep->multivariate_polynomial(x, multi_index[si2], nrand_ind);
      }
    }
  }
  if (same && all_mode)
    { expansionMoments[1] = covar; computedVariance |= 1; xPrevVar = x; }
  return covar;
}


void RegressOrthogPolyApproximation::
update_sparse(Real* dense_coeffs, size_t num_dense_terms)
{
  // just one pass through to define sparseIndices
  sparseIndices.clear();
  update_sparse_indices(dense_coeffs, num_dense_terms);

  // now update expansionCoeffs
  update_sparse_coeffs(dense_coeffs);
  //update_sparse_multi_index();
  update_sparse_sobol();
}


void RegressOrthogPolyApproximation::
update_sparse_indices(Real* dense_coeffs, size_t num_dense_terms)
{
  // always retain leading coefficient (mean)
  if (sparseIndices.empty())
    sparseIndices.insert(0);
  // update sparseIndices based on nonzero coeffs
  for (size_t i=1; i<num_dense_terms; ++i)
    if (std::abs(dense_coeffs[i]) > DBL_EPSILON)
      sparseIndices.insert(i); // discards duplicates (coeffs, coeffgrads)
}


void RegressOrthogPolyApproximation::update_sparse_coeffs(Real* dense_coeffs)
{
  // build sparse expansionCoeffs
  size_t num_exp_terms = sparseIndices.size();
  expansionCoeffs.sizeUninitialized(num_exp_terms);
  size_t i; SizetSet::const_iterator cit;
  for (i=0, cit=sparseIndices.begin(); i<num_exp_terms; ++i, ++cit)
    expansionCoeffs[i] = dense_coeffs[*cit];
}


void RegressOrthogPolyApproximation::
update_sparse_coeff_grads(Real* dense_coeffs, int row)
{
  // build sparse expansionCoeffGrads
  size_t num_exp_terms = sparseIndices.size();
  if (expansionCoeffGrads.numCols() != num_exp_terms)
    expansionCoeffGrads.reshape(surrData.num_derivative_variables(),
				num_exp_terms);
  int j; SizetSet::const_iterator cit;
  for (j=0, cit=sparseIndices.begin(); j<num_exp_terms; ++j, ++cit)
    expansionCoeffGrads(row, j) = dense_coeffs[*cit];
}


/*
void RegressOrthogPolyApproximation::update_sparse_multi_index()
{
  UShort2DArray old_multi_index = multiIndex;

  // build sparse multiIndex
  size_t num_exp_terms = sparseIndices.size();
  multiIndex.resize(num_exp_terms);
  size_t i; SizetSet::const_iterator cit;
  for (i=0, cit=sparseIndices.begin(); i<num_exp_terms; ++i, ++cit)
    multiIndex[i] = old_multi_index[*cit];
}
*/


void RegressOrthogPolyApproximation::update_sparse_sobol()
{
  // define the Sobol' indices based on the sparse multiIndex
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  if (data_rep->expConfigOptions.vbdFlag &&
      data_rep->expConfigOptions.vbdOrderLimit != 1)
    allocate_component_sobol(); // specialization for sparseSobolIndices
}


void RegressOrthogPolyApproximation::
run_cross_validation( RealMatrix &A, RealMatrix &B, RealMatrix &points, 
		      size_t num_data_pts_fn )
{
  RealMatrix A_copy( Teuchos::Copy, A, A.numRows(), A.numCols() );
  RealMatrix B_copy( Teuchos::Copy, B, B.numRows(), B.numCols() );
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShortArray& approx_order = data_rep->approxOrder;
  int num_rhs = B.numCols(), num_dims( approx_order.size() );
  // Do cross validation for varing polynomial orders up to 
  // a maximum order defined by approxOrder[0]
  unsigned short min_order = 1, ao0 = approx_order[0];
  if ( min_order > ao0 ) min_order = ao0;

  /// The options used to create the best PCE for each QOI
  std::vector<CompressedSensingOptions> bestCompressedSensingOpts_;

  /// The options of the best predictors of the predictors produced by each item
  /// in predictorOptionsList_. Information is stored for each PCE degree
  RealMatrix2DArray predictorOptionsHistory_;

  /// The best predictors of the predictors produced by each item 
  /// in predictorOptionsList_. Information is stored for each PCE degree
  RealMatrix2DArray predictorIndicatorsHistory_;

  /// The indicators of each partition for the best predictors of the 
  /// predictors produced by each item in predictorOptionsList_. 
  /// Information is stored for each PCE degree
  RealMatrix2DArray predictorPartitionIndicatorsHistory_;
  bestCompressedSensingOpts_;

  std::vector<CompressedSensingOptions> best_cs_opts( num_rhs );
  
  RealVector min_best_predictor_indicators( num_rhs );
  min_best_predictor_indicators = std::numeric_limits<Real>::max();
  bestCompressedSensingOpts_.resize( num_rhs );
  IntVector best_cross_validation_orders( num_rhs );
  unsigned short len = ao0 - min_order + 1;
  predictorOptionsHistory_.resize( len );
  predictorIndicatorsHistory_.resize( len );
  predictorPartitionIndicatorsHistory_.resize( len );
  int cnt( 0 );
  for ( int order = min_order; order <= ao0; order++ )
    {
      if (data_rep->expConfigOptions.outputLevel >= NORMAL_OUTPUT)
	PCout << "Testing PCE order " << order << std::endl;
      int num_basis_terms = nchoosek( num_dims + order, order );
      RealMatrix vandermonde_submatrix( Teuchos::View, 
					A_copy,
					A_copy.numRows(),
					num_basis_terms, 0, 0 );

      RealVector best_predictor_indicators;
      estimate_compressed_sensing_options_via_cross_validation( 
							       vandermonde_submatrix, 
							       B_copy, 
							       best_cs_opts,
							       best_predictor_indicators,
							       predictorOptionsHistory_[cnt], 
							       predictorIndicatorsHistory_[cnt],  
							       predictorPartitionIndicatorsHistory_[cnt],
							       num_data_pts_fn );

      // Only execute on master processor
      //      if ( is_master() )
      if ( true )
	{
	  for ( int k = 0; k < num_rhs; k++ )
	    {
	      if ( best_predictor_indicators[k] < 
		   min_best_predictor_indicators[k] )
		{
		  min_best_predictor_indicators[k] = 
		    best_predictor_indicators[k];
		  best_cross_validation_orders[k] = order;
		  bestCompressedSensingOpts_[k] = best_cs_opts[k];
		}
	      if (data_rep->expConfigOptions.outputLevel >= NORMAL_OUTPUT)
		{
		  PCout<<"Cross validation error for rhs "<<k<<" and degree ";
		  PCout << order << ": " <<  best_predictor_indicators[k]<< "\n";
		}
	    }
	}
      cnt++;
    }
  bestApproxOrder = best_cross_validation_orders;
  int num_basis_terms = nchoosek( num_dims + bestApproxOrder[0], 
				  bestApproxOrder[0] );
  if (data_rep->expConfigOptions.outputLevel >= NORMAL_OUTPUT)
    {
      PCout << "Best approximation order: " << bestApproxOrder[0]<< "\n";
      PCout << "Best cross validation error: ";
      PCout <<  min_best_predictor_indicators[0]<< "\n";
    }
  // set CSOpts so that best PCE can be built. We are assuming num_rhs=1
  RealMatrix vandermonde_submatrix( Teuchos::View, 
				    A_copy,
				    A_copy.numRows(),
				    num_basis_terms, 0, 0 );
  CompressedSensingOptionsList opts_list;
  RealMatrixArray solutions;
  bestCompressedSensingOpts_[0].storeHistory = false;
  bestCompressedSensingOpts_[0].print();
  IntVector index_mapping;
  RealMatrix points_dummy;
  remove_faulty_data(vandermonde_submatrix, B_copy, points_dummy, index_mapping,
		     faultInfo, surrData.failed_response_data());
  int num_rows_V = vandermonde_submatrix.numRows(),
      num_cols_V = vandermonde_submatrix.numCols();
  faultInfo.under_determined = num_rows_V < num_cols_V;
  PCout << "Applying regression to compute " << num_cols_V
	<< " chaos coefficients using " << num_rows_V << " equations.\n";
  select_solver();
  data_rep->CSTool.solve( vandermonde_submatrix, B_copy, solutions, 
			  bestCompressedSensingOpts_[0], opts_list );

  if (faultInfo.under_determined) // exploit CS sparsity
    update_sparse(solutions[0][0], num_basis_terms);
  else {
    // if best expansion order is less than maximum candidate, truncate
    // expansion arrays.  Note that this requires care in cross-expansion
    // evaluations such as off-diagonal covariance.
    if (num_basis_terms < data_rep->expansion_terms()) // candidate exp size
      for (size_t i=0; i<num_basis_terms; ++i)
	sparseIndices.insert(i); // sparse subset is first num_basis_terms
    copy_data(solutions[0][0], num_basis_terms, expansionCoeffs);
  }
};

void RegressOrthogPolyApproximation::gridSearchFunction( RealMatrix &opts,
						  int M, int N, 
						  int num_function_samples )
{
  // Setup a grid based search
  bool is_under_determined = M < N;

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;

  // Define the 1D grids for under and over-determined LARS, LASSO, OMP, BP and 
  // LS
  RealVectorArray opts1D( 9 );
  opts1D[0].size( 1 ); // solver type
  opts1D[0][0] = CSOpts.solver;
  opts1D[1].size( 1 ); // Solver tolerance. 
  opts1D[1][0] = CSOpts.solverTolerance;
  opts1D[2] = data_rep->noiseTols; // epsilon.
  opts1D[3].size( 1 ); // delta
  opts1D[3] = CSOpts.delta;
  opts1D[4].size( 1 ); // max_number of non_zeros
  opts1D[4] = CSOpts.maxNumIterations; 
  opts1D[5].size( 1 );  // standardizeInputs
  opts1D[5] = false;
  opts1D[6].size( 1 );  // storeHistory
  opts1D[6] = true;  
  opts1D[7].size( 1 );  // Verbosity. Warnings on
  opts1D[7] =  std::max(0, data_rep->expConfigOptions.outputLevel - 1);
  opts1D[8].size( 1 );  // num function samples
  opts1D[8] = num_function_samples;
      
  // Form the multi-dimensional grid
  cartesian_product( opts1D, opts );
};

void RegressOrthogPolyApproximation::
estimate_compressed_sensing_options_via_cross_validation( RealMatrix &vandermonde_matrix, RealMatrix &rhs, std::vector<CompressedSensingOptions> &best_cs_opts, RealVector &best_predictor_indicators, RealMatrixArray &predictor_options_history, RealMatrixArray &predictor_indicators_history, RealMatrixArray &predictor_partition_indicators_history, size_t num_data_pts_fn ){

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;

  // Initialise the cross validation iterator
  CrossValidationIterator CV;
  CV.mpi_communicator( MPI_COMM_WORLD );
  CV.verbosity(  std::max(0, data_rep->expConfigOptions.outputLevel - 1) );
  // Set and partition the data
  CV.set_data( vandermonde_matrix, rhs, num_data_pts_fn );
  int num_folds( 10 );
  // Keep copy of state
  CompressedSensingOptions cs_opts_copy = CSOpts;
  
  if ( ( ( num_data_pts_fn / num_folds == 1 ) && 
	 ( num_data_pts_fn - 3 < vandermonde_matrix.numCols() ) )  || 
       ( num_data_pts_fn / num_folds == 0 ) )
    // use one at a time cross validation
    num_folds = num_data_pts_fn;
  if ( num_data_pts_fn == vandermonde_matrix.numCols() )
    {
      PCout << "Warning: The cross validation results will not be consistent. ";
      PCout << "The number of function samples = the number of basis terms, ";
      PCout << "thus only underdetermined matrices will be generated during ";
      PCout << "cross validation even though the system is fully determined.\n";
    }

  if ( ( CSOpts.solver == EQ_CON_LEAST_SQ_REGRESSION ) &&
       ( num_folds = num_data_pts_fn ) && 
       ( vandermonde_matrix.numRows() * ( num_data_pts_fn - 1 ) / num_data_pts_fn  <= vandermonde_matrix.numCols() ) )
    // includes exactly determined case
    {
      PCout << "EQ_CON_LEAST_SQ_REGRESSION could not be used. ";
      PCout << "The cross validation training vandermonde matrix is ";
      PCout << "under-determined\n";
      CSOpts.solver = LASSO_REGRESSION;
    }
  if ( ( CSOpts.solver == EQ_CON_LEAST_SQ_REGRESSION ) && ( num_folds = num_data_pts_fn ) &&  ( num_data_pts_fn - 1 >= vandermonde_matrix.numCols() ) )
    {
      PCout << "EQ_CON_LEAST_SQ_REGRESSION could not be used. ";
      PCout << "The cross validation training vandermonde matrix is ";
      PCout << "over-determined\n";
      CSOpts.solver = DEFAULT_REGRESSION;
    }
    
  CV.setup_k_folds( num_folds );
  
  // Tell the cross validation iterator what options to test
  RealMatrix opts;
  gridSearchFunction( opts, vandermonde_matrix.numRows(),
		      vandermonde_matrix.numCols(), num_data_pts_fn );
  CV.predictor_options_list( opts );

  // Perform cross validation
  CV.run( &rmse_indicator, &linear_predictor_analyser, 
	  &normalised_mean_selector,
	  &linear_predictor_best_options_extractor,
	  faultInfo, surrData.failed_response_data() );

  // Get results of cross validation
  RealMatrix best_predictor_options;
  CV.get_best_predictor_info( best_predictor_options, 
			      best_predictor_indicators );

  CV.get_history_data( predictor_options_history, 
		       predictor_indicators_history,
		       predictor_partition_indicators_history );

  //if ( CV.is_master() )
  if ( true )
    {
      int len_opts(  best_predictor_options.numRows() ), 
	num_rhs( rhs.numCols() );
      for ( int k = 0; k < num_rhs; k++ )
	{
	  RealVector col( Teuchos::View,  best_predictor_options[k], len_opts );
	  set_linear_predictor_options( col, best_cs_opts[k] );
	}
    }

  //restore state
  CSOpts = cs_opts_copy;
};


void RegressOrthogPolyApproximation::least_interpolation( RealMatrix &pts, 
							  RealMatrix &vals )
{
#ifdef DEBUG
  if ( pts.numCols() != vals.numRows() ) 
    {
      std::string msg = "least_interpolation() dimensions of pts and vals ";
      msg += "are inconsistent";
      throw( std::runtime_error( msg ) );
    }
#endif

  RealMatrix L, U, H;
  IntVector p, k;
  RealVector v; 

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;

  // must do this inside transform_least_interpolant
  //RealVector domain;
  //TensorProductBasis_ptr basis; 
  //std::vector<PolyIndex_ptr> basis_indices;
  //pce.get_domain( domain );
  //pce.get_basis( basis );
  //pce.get_basis_indices( basis_indices );

  // This prevents the case where multiIndex is defined earlier in an efficient
  // way to match the data
  data_rep->multiIndex.clear(); // TO DO

  // TO DO:
  // > if no sim faults, then this could be done once in SharedRegressOPAD
  // > if there are sim faults, then can we leverage sparseIndices approach?
  least_factorization( pts, data_rep->multiIndex, L, U, H, p, v, k );

  RealMatrix coefficients;
  transform_least_interpolant( L, U, H, p, v, vals );
  PCout << "@@@@@\n";
  PCout << vals.numCols() << std::endl;

  // must do this inside transform_least_interpolant
  //pce.set_basis_indices( basis_indices );
  //pce.set_coefficients( coefficients );
}


void RegressOrthogPolyApproximation::
transform_least_interpolant( RealMatrix &L, RealMatrix &U, RealMatrix &H,
			     IntVector &p,  RealVector &v, RealMatrix &vals )
{
  int num_pts = vals.numRows(), num_qoi = vals.numCols();
  
  RealMatrix LU_inv;
  IntVector dummy;
  lu_inverse( L, U, dummy, LU_inv );

  RealMatrix V_inv( H.numCols(), num_pts );
  V_inv.multiply( Teuchos::TRANS, Teuchos::NO_TRANS, 1.0, H, LU_inv, 0.0 );

  IntVector p_col;
  argsort( p, p_col );
  permute_matrix_columns( V_inv, p_col );

  RealMatrix coefficients;
  coefficients.shapeUninitialized( V_inv.numRows(), num_qoi );
  coefficients.multiply( Teuchos::NO_TRANS, Teuchos::NO_TRANS, 1.0, V_inv, 
			 vals, 0.0 );

  // multiIndex should be consistent across QoI vector
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  copy_data(coefficients.values(), (int)data_rep->expansion_terms(),
	    expansionCoeffs);

  // now define the Sobol' indices based on the least polynomial multiIndex
  if (data_rep->expConfigOptions.vbdFlag &&
      data_rep->expConfigOptions.vbdOrderLimit != 1)
    allocate_component_sobol();
}


void RegressOrthogPolyApproximation::
least_factorization( RealMatrix &pts, UShort2DArray &basis_indices,
		     RealMatrix &l, RealMatrix &u, RealMatrix &H, IntVector &p,
		     RealVector &v, IntVector &k )
{
  int num_vars = pts.numRows(), num_pts = pts.numCols();

  eye( num_pts, l );
  eye( num_pts, u );

  range( p, 0, num_pts, 1 );

  //This is just a guess: this vector could be much larger, or much smaller
  v.size( 1000 );
  int v_index = 0;

  // Current polynomial degree
  int k_counter = 0;
  // k[q] gives the degree used to eliminate the q'th point
  k.size( num_pts );

  // The current LU row to factor out:
  int lu_row = 0;

  UShort2DArray internal_basis_indices;

  // Current degree is k_counter, and we iterate on this
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  while ( lu_row < num_pts )
    {
      UShort2DArray new_indices;
      if ( basis_indices.size() == 0 )
	{
	  data_rep->total_order_multi_index( (unsigned short)k_counter,
					     (size_t)num_vars, new_indices );
	  //get_hyperbolic_level_indices( num_vars,
	  ///			k_counter,
	  //				1.,
	  //new_indices );

	  int num_indices = internal_basis_indices.size();
	  //for ( int i = 0; i < (int)new_indices.size(); i++ )
	  // new_indices[i]->set_array_index( num_indices + i );
	}
      else
	{
	  //int cnt = internal_basis_indices.size();
	  for ( int i = 0; i < (int)basis_indices.size(); i++ )
	    {
	      //if ( basis_indices[i]->get_level_sum() == k_counter )
	      if ( index_norm( basis_indices[i] ) == k_counter )
		{
		  new_indices.push_back( basis_indices[i] );
		  //basis_indices[i]->set_array_index( cnt );
		  //cnt++;
		}
	    }
	  // If the basis_indices set is very sparse then not all degrees may 
	  // be represented in basis_indices. Thus increase degree counter
	  if ( new_indices.size() == 0 )
	    k_counter++;
	  if ( ( basis_indices.size() == internal_basis_indices.size() ) && 
	       ( new_indices.size() == 0 ) )
	    {
	      std::string msg = "least_factorization() the basis indices ";
	      msg += "specified were insufficient to interpolate the data";
	      throw( std::runtime_error( msg ) ); 
	    }
	}

      if ( (  new_indices.size() > 0 ) )
	{	
      
	  internal_basis_indices.insert( internal_basis_indices.end(), 
					 new_indices.begin(), 
					 new_indices.end() );
      
	  int current_dim = new_indices.size();

	  // Evaluate the basis
	  //RealMatrix W;
	  //basis->value_set( pts, new_indices, W );
	  RealMatrix W( num_pts, current_dim, false);
	  for ( int j = 0; j < num_pts; j++ )
	    {
	      RealVector x( Teuchos::View, pts[j], num_vars );
	      for ( int i = 0; i < (int)new_indices.size(); i++ )
		{ 
		  W(j,i) = data_rep->
		    multivariate_polynomial( x, new_indices[i] );
		}
	    }

	  permute_matrix_rows( W, p );
      
	  // Row-reduce W according to previous elimination steps
	  int m = W.numRows(), n = W.numCols();
	  for ( int q = 0; q < lu_row; q++ )
	    {
	      for ( int i = 0; i < n; i++ )
		W(q,i) /= l(q,q);

	      RealMatrix tmp( Teuchos::View, W, m-q-1, n, q+1, 0 ),
		l_col( Teuchos::View, l, m-q-1, 1, q+1, q ),
		W_row( Teuchos::View, W, 1, n, q, 0 ); 
	      tmp.multiply( Teuchos::NO_TRANS, Teuchos::NO_TRANS, -1.0, l_col, 
			    W_row, 1.0 );
	    }

	  //RealMatrix wm( Teuchos::View, W, m-lu_row, n, lu_row, 0 );
	  RealMatrix wm( n, m-lu_row );
	  for ( int i = 0; i < m-lu_row; i++ )
	    {
	      for ( int j = 0; j < n; j++ )
		wm(j,i) = W(lu_row+i,j);
	    }
	  RealMatrix Q, R;
	  IntVector evec;
	  pivoted_qr_factorization( wm, Q, R, evec );
      
	  // Compute rank
	  int rnk = 0;
	  for ( int i = 0; i < R.numRows(); i++ )
	    {
	      if ( std::fabs( R(i,i) ) < 0.001 * std::fabs( R(0,0) ) ) break;
	      rnk += 1;
	    }

	  // Now first we must permute the rows by e
	  IntMatrix p_sub( Teuchos::View, &p[lu_row], num_pts - lu_row, 
			   num_pts - lu_row, 1 );
	  permute_matrix_rows( p_sub, evec );
      
	  // And correct by permuting l as well
 
	  RealMatrix l_sub( Teuchos::View, l, num_pts - lu_row, lu_row,lu_row,0);
	  if ( ( l_sub.numRows() > 0 ) && ( l_sub.numCols() > 0 ) )
	    permute_matrix_rows( l_sub, evec );

	  // The matrix r gives us inner product information for all rows below 
	  // these in W
	  for ( int i = 0; i < rnk; i++ )
	    {
	      for ( int j = 0; j < num_pts - lu_row; j++ )
		l(lu_row+j,lu_row+i) = R(i,j);
	    }

	  // Now we must find inner products of all the other rows above 
	  // these in W
	  RealMatrix W_sub( Teuchos::View, W, lu_row, W.numCols(), 0, 0 ),
	    Q_sub( Teuchos::View, Q, Q.numRows(), rnk, 0, 0 ),
	    u_sub( Teuchos::View, u, lu_row, rnk, 0, lu_row );

	  if ( ( u_sub.numRows() > 0 ) && ( u_sub.numCols() > 0 ) )
	    u_sub.multiply( Teuchos::NO_TRANS, Teuchos::NO_TRANS, 1.0, W_sub, 
			    Q_sub, 0.0 );

	  if ( v_index+(current_dim*rnk) > v.length() )
	    v.resize( v.length() + std::max( 1000, current_dim*rnk ) );
	  // The matrix q must be saved in order to characterize basis
	  int cnt = v_index;
	  for ( int j = 0; j < rnk; j++ )
	    {
	      for ( int i = 0; i < Q.numRows(); i++ )
		{
		  v[cnt] = Q(i,j);
		  cnt++;
		}
	    }
	  v_index += ( current_dim * rnk );

	  // Update degree markers, and node and degree count
	  for ( int i = lu_row; i < lu_row+rnk; i++ )
	    k[i] = k_counter;
	  lu_row += rnk;
	  k_counter++;
	}
    }
  // Chop off parts of unnecessarily allocated vector v
  v.resize( v_index );
  
  // Return the indices used by least interpolation. This may be different
  // to the basis_indices specified on entry.
  basis_indices = internal_basis_indices;

  // Make matrix H
  get_least_polynomial_coefficients( v, k, basis_indices, num_vars, num_pts,
				     H );
};


void RegressOrthogPolyApproximation::get_least_polynomial_coefficients(
				       RealVector &v, IntVector &k,
				       UShort2DArray &basis_indices,
				       int num_vars, int num_pts,
				       RealMatrix &H )
{
  int num_basis_indices = basis_indices.size();
  H.shape( num_pts, num_basis_indices );
  int v_counter = 0, previous_dimension = 0, current_size = 0;
  for ( int i = 0; i < num_pts; i++ )
    {
      if ( ( i == 0 ) || ( k[i] != k[i-1] ) )
	{
	  current_size = 0;
	  for ( int j = 0; j < num_basis_indices; j++ )
	    {
	      //if ( basis_indices[j]->get_level_sum() == k[i] )
	      if ( index_norm( basis_indices[j] ) == k[i] )
		current_size++;
	    }
	}
      for ( int j = 0; j < current_size; j++ )
	{
	  H(i,previous_dimension+j) = v[v_counter+j];
	}
      v_counter += current_size;
      if ( ( i+1 < num_pts ) && ( k[i] != k[i+1] ) )
	previous_dimension += current_size;
    }
};

} // namespace Pecos
