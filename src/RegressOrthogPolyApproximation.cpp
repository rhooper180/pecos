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
  if (faultInfo.under_determined || data_rep->
      expConfigOptions.expCoeffsSolnApproach == ORTHOG_LEAST_INTERPOLATION) {
    // defer allocations until sparsity is known

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
  //size_t index = surrData.points() - 1;
  //surrData.anchor_point(surrData.variables_data()[index],
  //                      surrData.response_data()[index]);
  //surrData.pop(1);

  // anchor point, if present, is handled differently for different
  // expCoeffsSolnApproach settings:
  //   SAMPLING:   treat it as another data point
  //   QUADRATURE/CUBATURE/COMBINED_SPARSE_GRID: error
  //   LEAST_SQ_REGRESSION: use equality-constrained least squares
  size_t i, j, num_total_pts = surrData.points();
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


void RegressOrthogPolyApproximation::store_coefficients()
{
  storedSparseIndices = sparseIndices;
  OrthogPolyApproximation::store_coefficients(); // storedExpCoeff{s,Grads}
}


void RegressOrthogPolyApproximation::combine_coefficients(short combine_type)
{
  // based on incoming combine_type, combine the data stored previously
  // by store_coefficients()

  if (sparseIndices.empty() && storedSparseIndices.empty())
    OrthogPolyApproximation::combine_coefficients(combine_type);
    // augment base implementation with clear of storedExpCoeff{s,Grads}
  else {
    SharedRegressOrthogPolyApproxData* data_rep
      = (SharedRegressOrthogPolyApproxData*)sharedDataRep;

    // support mixed case using simple approach of populating the missing
    // sparse indices arrays (not optimal for performance but a lot less code),
    // prior to expansion aggregation.  Note: sparseSobolIndexMap is updated
    // following aggregation.
    if (sparseIndices.empty()) {
      size_t i, num_mi = data_rep->multiIndex.size();
      for (i=0; i<num_mi; ++i)
	sparseIndices.insert(i);
    }
    if (storedSparseIndices.empty()) {
      size_t i, num_st_mi = data_rep->storedMultiIndex.size();
      for (i=0; i<num_st_mi; ++i)
	storedSparseIndices.insert(i);
    }

    switch (combine_type) {
    case ADD_COMBINE: {
      // update sparseIndices and expansionCoeff{s,Grads}
      overlay_expansion(storedSparseIndices, data_rep->storedMultiIndexMap,
			storedExpCoeffs, storedExpCoeffGrads, 1);
      // update sparseSobolIndexMap
      update_sparse_sobol(data_rep->multiIndex, data_rep->sobolIndexMap);
      break;
    }
    case MULT_COMBINE: {
      // perform the multiplication of current and stored expansions
      multiply_expansion(storedSparseIndices, data_rep->storedMultiIndex,
			 storedExpCoeffs, storedExpCoeffGrads,
			 data_rep->combinedMultiIndex);
      // update sparseSobolIndexMap
      update_sparse_sobol(data_rep->combinedMultiIndex,data_rep->sobolIndexMap);
      break;
    }
    case ADD_MULT_COMBINE:
      //overlay_expansion(data_rep->storedMultiIndex, storedExpCoeffs,
      //                  storedExpCoeffGrads, addCoeffs, addCoeffGrads);
      //multiply_expansion(data_rep->storedMultiIndex, storedExpCoeffs,
      //                   storedExpCoeffGrads, multCoeffs, multCoeffGrads);
      //compute_combine_factors(addCoeffs, multCoeffs);
      //apply_combine_factors();
      PCerr << "Error : additive+multiplicative combination not yet "
	    << "implemented in OrthogPolyApproximation::combine_coefficients()"
	    << std::endl;
      abort_handler(-1);
      break;
    }

    computedMean = computedVariance = 0;
  }

  if (expansionCoeffFlag)     storedExpCoeffs.resize(0);
  if (expansionCoeffGradFlag) storedExpCoeffGrads.reshape(0,0);
}


void RegressOrthogPolyApproximation::
overlay_expansion(const SizetSet& sparse_ind_2, const SizetArray& append_mi_map,
		  const RealVector& exp_coeffs_2, const RealMatrix& exp_grads_2,
		  int coeff_2)
{
  // update sparseIndices w/ new contributions relative to updated multi-index
  // generated by SharedOrthogPolyApproxData::pre_combine_data():
  // 1. previous sparseIndices into the ref_mi are still valid (new mi entries
  //    are appended, not inserted, such that previous mi ordering is preserved)
  // 2. sparse_ind_2 into original ref_mi terms may be new (not contained in
  //    sparseIndices) -> insert combined index from append_mi_map (reject
  //    duplicates).  This case invalidates expansionCoeff{s,Grads} (see below)
  // 3. sparse_ind_2 into appended mi are new -> insert combined index from
  //    append_mi_map
  SizetSet combined_sparse_ind = sparseIndices;
  StSCIter cit;
  for (cit=sparse_ind_2.begin(); cit!=sparse_ind_2.end(); ++cit)
    combined_sparse_ind.insert(append_mi_map[*cit]);

  size_t i, j, combined_index, num_deriv_vars,
    num_combined_terms = combined_sparse_ind.size();
  // initialize combined_exp_coeff{s,_grads}.  previous expansionCoeff{s,Grads}
  // cannot simply be resized since they must correspond to the combined sparse
  // indices, which can be reordered (see note 2. above)
  RealVector combined_exp_coeffs; RealMatrix combined_exp_coeff_grads;
  if (expansionCoeffFlag)
    combined_exp_coeffs.size(num_combined_terms); // init to 0
  if (expansionCoeffGradFlag) {
    num_deriv_vars = expansionCoeffGrads.numRows();
    combined_exp_coeff_grads.shape(num_deriv_vars, num_combined_terms); // -> 0.
  }

  for (i=0, cit=sparseIndices.begin(); cit!=sparseIndices.end(); ++i, ++cit) {
    combined_index = find_index(combined_sparse_ind, *cit);
    if (expansionCoeffFlag)
      combined_exp_coeffs[combined_index] = expansionCoeffs[i];
    if (expansionCoeffGradFlag)
      copy_data(expansionCoeffGrads[i], num_deriv_vars,
		combined_exp_coeff_grads[combined_index]);
  }
  for (i=0, cit=sparse_ind_2.begin(); cit!=sparse_ind_2.end(); ++i, ++cit) {
    combined_index = find_index(combined_sparse_ind, append_mi_map[*cit]);
    if (expansionCoeffFlag)
      combined_exp_coeffs[combined_index] += coeff_2 * exp_coeffs_2[i];
    if (expansionCoeffGradFlag) {
      Real*       exp_grad_ndx = combined_exp_coeff_grads[combined_index];
      const Real* grad_i       = exp_grads_2[i];
      for (j=0; j<num_deriv_vars; ++j)
	exp_grad_ndx[j] += coeff_2 * grad_i[j];
    }
  }

  // overlay is complete; can now overwrite previous state
  sparseIndices = combined_sparse_ind;
  if (expansionCoeffFlag)     expansionCoeffs     = combined_exp_coeffs;
  if (expansionCoeffGradFlag) expansionCoeffGrads = combined_exp_coeff_grads;
}


void RegressOrthogPolyApproximation::
multiply_expansion(const SizetSet&      sparse_ind_b,
		   const UShort2DArray& multi_index_b,
		   const RealVector&    exp_coeffs_b,
		   const RealMatrix&    exp_grads_b,
		   const UShort2DArray& multi_index_c)
{
  // sparsity in exp_{coeffs,grads}_c determined *after* all multi_index_c
  // projection terms have been computed

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;

  const UShort2DArray& multi_index_a = data_rep->multiIndex;
  size_t i, j, k, v, si, sj, num_v = sharedDataRep->numVars, num_deriv_vars,
    num_c = multi_index_c.size();
  StSCIter ait, bit;

  // precompute 1D basis triple products required
  unsigned short max_a, max_b, max_c; UShortMultiSet max_abc;
  OrthogonalPolynomial* poly_rep_v;
  for (v=0; v<num_v; ++v) {
    max_a = max_b = max_c = 0; max_abc.clear();
    // could track max_abc within combine_coefficients() and pass in, but would
    // need max orders for each dimension for both factors plus their product.
    // Since this would be awkward and only marginally more efficient, just
    // compute them here from the available multi-index arrays.
    for (ait=++sparseIndices.begin(); ait!=sparseIndices.end(); ++ait)
      if (multi_index_a[*ait][v] > max_a)
	max_a = multi_index_a[*ait][v];
    for (bit=++sparse_ind_b.begin(); bit!=sparse_ind_b.end(); ++bit)
      if (multi_index_b[*bit][v] > max_b)
	max_b = multi_index_b[*bit][v];
    for (k=1; k<num_c; ++k)
      if (multi_index_c[k][v] > max_c)
	max_c = multi_index_c[k][v];
    max_abc.insert(max_a); max_abc.insert(max_b); max_abc.insert(max_c); 
    poly_rep_v
      = (OrthogonalPolynomial*)(data_rep->polynomialBasis[v].polynomial_rep());
    poly_rep_v->precompute_triple_products(max_abc);
  }

  // For c = a * b, compute coefficient of product expansion as:
  // \Sum_k c_k \Psi_k = \Sum_i \Sum_j a_i b_j \Psi_i \Psi_j
  //    c_k <\Psi_k^2> = \Sum_i \Sum_j a_i b_j <\Psi_i \Psi_j \Psi_k>
  RealVector exp_coeffs_c; RealVectorArray exp_grads_c;
  if (expansionCoeffFlag)
    exp_coeffs_c.size(num_c);     // init to 0
  if (expansionCoeffGradFlag) {
    num_deriv_vars = expansionCoeffGrads.numRows();
    exp_grads_c.resize(num_deriv_vars);
    for (v=0; v<num_deriv_vars; ++v)
      exp_grads_c[v].size(num_c); // init to 0
  }
  Real trip_prod, trip_prod_v, norm_sq_k; bool non_zero;
  for (k=0; k<num_c; ++k) {
    for (i=0, ait=sparseIndices.begin(); ait!=sparseIndices.end(); ++i, ++ait) {
      si = *ait;
      for (j=0, bit=sparse_ind_b.begin(); bit!=sparse_ind_b.end(); ++j, ++bit) {
	sj = *bit;
	trip_prod = 1.;
	for (v=0; v<num_v; ++v) {
	  poly_rep_v = (OrthogonalPolynomial*)
	    (data_rep->polynomialBasis[v].polynomial_rep());
	  non_zero = poly_rep_v->triple_product(multi_index_a[si][v],
	    multi_index_b[sj][v], multi_index_c[k][v], trip_prod_v);
	  if (non_zero) trip_prod *= trip_prod_v;
	  else          break;
	}
	if (non_zero) {
	  if (expansionCoeffFlag)
	    exp_coeffs_c[k] += expansionCoeffs[i] * exp_coeffs_b[j] * trip_prod;
	  if (expansionCoeffGradFlag) {
	    const Real* exp_grads_a_i = expansionCoeffGrads[i];
	    const Real* exp_grads_b_j = exp_grads_b[j];
	    for (v=0; v<num_deriv_vars; ++v)
	      exp_grads_c[v][k] += (expansionCoeffs[i] * exp_grads_b_j[v]
		+ exp_coeffs_b[j] * exp_grads_a_i[v]) * trip_prod;
	  }
	}
      }
    }
    norm_sq_k = data_rep->norm_squared(multi_index_c[k]);
    if (expansionCoeffFlag)
      exp_coeffs_c[k] /= norm_sq_k;
    if (expansionCoeffGradFlag)
      for (v=0; v<num_deriv_vars; ++v)
	exp_grads_c[v][k] /= norm_sq_k;
  }

  // update sparse bookkeeping based on nonzero terms
  sparseIndices.clear();
  if (expansionCoeffFlag)
    update_sparse_indices(exp_coeffs_c.values(), num_c);
  if (expansionCoeffGradFlag)
    for (v=0; v<num_deriv_vars; ++v)
      update_sparse_indices(exp_grads_c[v].values(), num_c);
  // update expansion{Coeffs,CoeffGrads} from sparse indices
  if (expansionCoeffFlag)
    update_sparse_coeffs(exp_coeffs_c.values());
  if (expansionCoeffGradFlag)
    for (v=0; v<num_deriv_vars; ++v)
      update_sparse_coeff_grads(exp_grads_c[v].values(), v);
}


Real RegressOrthogPolyApproximation::value(const RealVector& x)
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::value(x);

  // Error check for required data
  if (!expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "RegressOrthogPolyApproximation::value()" << std::endl;
    abort_handler(-1);
  }

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& mi = data_rep->multiIndex;
  Real approx_val = 0.;
  size_t i; StSIter it;
  for (i=0, it=sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it)
    approx_val += expansionCoeffs[i] *
      data_rep->multivariate_polynomial(x, mi[*it]);
  return approx_val;
}


const RealVector& RegressOrthogPolyApproximation::
gradient_basis_variables(const RealVector& x)
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::gradient_basis_variables(x);

  // could define a default_dvv and call gradient_basis_variables(x, dvv),
  // but we want this fn to be as fast as possible

  // Error check for required data
  if (!expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in RegressOrthogPoly"
	  << "Approximation::gradient_basis_variables()" << std::endl;
    abort_handler(-1);
  }

  size_t i, j, num_v = sharedDataRep->numVars;
  if (approxGradient.length() != num_v)
    approxGradient.size(num_v); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& mi = data_rep->multiIndex;
  StSIter it;
  for (i=0, it=sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it) {
    const RealVector& term_i_grad
      = data_rep->multivariate_polynomial_gradient_vector(x, mi[*it]);
    Real coeff_i = expansionCoeffs[i];
    for (j=0; j<num_v; ++j)
      approxGradient[j] += coeff_i * term_i_grad[j];
  }
  return approxGradient;
}


const RealVector& RegressOrthogPolyApproximation::
gradient_basis_variables(const RealVector& x, const SizetArray& dvv)
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::gradient_basis_variables(x, dvv);

  // Error check for required data
  if (!expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in RegressOrthogPoly"
	  << "Approximation::gradient_basis_variables()" << std::endl;
    abort_handler(-1);
  }

  size_t i, j, num_deriv_vars = dvv.size();
  if (approxGradient.length() != num_deriv_vars)
    approxGradient.size(num_deriv_vars); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& mi = data_rep->multiIndex;
  StSIter it;
  for (i=0, it=sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it) {
    const RealVector& term_i_grad
      = data_rep->multivariate_polynomial_gradient_vector(x, mi[*it], dvv);
    Real coeff_i = expansionCoeffs[i];
    for (j=0; j<num_deriv_vars; ++j)
      approxGradient[j] += coeff_i * term_i_grad[j];
  }
  return approxGradient;
}


const RealVector& RegressOrthogPolyApproximation::
gradient_nonbasis_variables(const RealVector& x)
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::gradient_nonbasis_variables(x);

  // Error check for required data
  if (!expansionCoeffGradFlag) {
    PCerr << "Error: expansion coefficient gradients not defined in Regress"
	  << "OrthogPolyApproximation::gradient_coefficient_variables()"
	  << std::endl;
    abort_handler(-1);
  }

  size_t i, j, num_deriv_vars = expansionCoeffGrads.numRows();
  if (approxGradient.length() != num_deriv_vars)
    approxGradient.size(num_deriv_vars); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& mi = data_rep->multiIndex;
  StSIter it;
  for (i=0, it=sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it) {
    Real term_i = data_rep->multivariate_polynomial(x, mi[*it]);
    const Real* exp_coeff_grad_i = expansionCoeffGrads[i];
    for (j=0; j<num_deriv_vars; ++j)
      approxGradient[j] += exp_coeff_grad_i[j] * term_i;
  }
  return approxGradient;
}


Real RegressOrthogPolyApproximation::stored_value(const RealVector& x)
{
  if (storedSparseIndices.empty())
    return OrthogPolyApproximation::stored_value(x);

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& stored_mi = data_rep->storedMultiIndex;

  // Error check for required data
  if (stored_mi.empty()) {
    PCerr << "Error: stored expansion coefficients not available in "
	  << "RegressOrthogPolyApproximation::stored_value()" << std::endl;
    abort_handler(-1);
  }

  Real approx_val = 0.;
  size_t i; StSIter it;
  for (i=0, it=storedSparseIndices.begin(); it!=storedSparseIndices.end();
       ++i, ++it)
    approx_val += storedExpCoeffs[i] *
      data_rep->multivariate_polynomial(x, stored_mi[*it]);
  return approx_val;
}


const RealVector& RegressOrthogPolyApproximation::
stored_gradient_basis_variables(const RealVector& x)
{
  if (storedSparseIndices.empty())
    return OrthogPolyApproximation::stored_gradient_basis_variables(x);

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& stored_mi = data_rep->storedMultiIndex;

  // Error check for required data
  size_t i, j, num_v = sharedDataRep->numVars;
  if (stored_mi.empty()) {
    PCerr << "Error: stored expansion coefficients not available in Regress"
	  << "OrthogPolyApproximation::stored_gradient_basis_variables()"
	  << std::endl;
    abort_handler(-1);
  }

  if (approxGradient.length() != num_v)
    approxGradient.size(num_v); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  StSIter it;
  for (i=0, it=storedSparseIndices.begin(); it!=storedSparseIndices.end();
       ++i, ++it) {
    const RealVector& term_i_grad
      = data_rep->multivariate_polynomial_gradient_vector(x, stored_mi[*it]);
    Real coeff_i = storedExpCoeffs[i];
    for (j=0; j<num_v; ++j)
      approxGradient[j] += coeff_i * term_i_grad[j];
  }
  return approxGradient;
}


const RealVector& RegressOrthogPolyApproximation::
stored_gradient_nonbasis_variables(const RealVector& x)
{
  if (storedSparseIndices.empty())
    return OrthogPolyApproximation::stored_gradient_nonbasis_variables(x);

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& stored_mi = data_rep->storedMultiIndex;

  // Error check for required data
  size_t i, j, num_deriv_vars = storedExpCoeffGrads.numRows();
  if (stored_mi.empty()) {
    PCerr << "Error: stored expansion coeff grads not available in Regress"
	  << "OrthogPolyApproximation::stored_gradient_nonbasis_variables()"
	  << std::endl;
    abort_handler(-1);
  }

  if (approxGradient.length() != num_deriv_vars)
    approxGradient.size(num_deriv_vars); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  StSIter it;
  for (i=0, it=storedSparseIndices.begin(); it!=storedSparseIndices.end();
       ++i, ++it) {
    Real term_i = data_rep->multivariate_polynomial(x, stored_mi[*it]);
    const Real* coeff_grad_i = storedExpCoeffGrads[i];
    for (j=0; j<num_deriv_vars; ++j)
      approxGradient[j] += coeff_grad_i[j] * term_i;
  }
  return approxGradient;
}


/** In this case, a subset of the expansion variables are random
    variables and the mean of the expansion involves evaluating the
    expectation over this subset. */
Real RegressOrthogPolyApproximation::mean(const RealVector& x)
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::mean(x);

  // Error check for required data
  if (!expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "RegressOrthogPolyApproximation::mean()" << std::endl;
    abort_handler(-1);
  }

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const SizetList& nrand_ind = data_rep->nonRandomIndices;
  bool all_mode = !nrand_ind.empty();
  if (all_mode && (computedMean & 1) &&
      data_rep->match_nonrandom_vars(x, xPrevMean))
    return expansionMoments[0];

  const UShort2DArray& mi = data_rep->multiIndex;
  Real mean = expansionCoeffs[0];
  size_t i; StSIter it;
  for (i=1, it=++sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it) {
    const UShortArray& mi_i = mi[*it];
    // expectations are zero for expansion terms with nonzero random indices
    if (data_rep->zero_random(mi_i)) {
      mean += expansionCoeffs[i] *
	data_rep->multivariate_polynomial(x, mi_i, nrand_ind);
#ifdef DEBUG
      PCout << "Mean estimate inclusion: term index = " << i << " Psi = "
	    << data_rep->multivariate_polynomial(x, mi_i, nrand_ind)
	    << " PCE coeff = " << expansionCoeffs[i] << " total = " << mean
	    << std::endl;
#endif // DEBUG
    }
  }

  if (all_mode)
    { expansionMoments[0] = mean; computedMean |= 1; xPrevMean = x; }
  return mean;
}


/** In this function, a subset of the expansion variables are random
    variables and any augmented design/state variables (i.e., not
    inserted as random variable distribution parameters) are included
    in the expansion.  In this case, the mean of the expansion is the
    expectation over the random subset and the derivative of the mean
    is the derivative of the remaining expansion over the non-random
    subset.  This function must handle the mixed case, where some
    design/state variables are augmented (and are part of the
    expansion: derivatives are evaluated as described above) and some
    are inserted (derivatives are obtained from expansionCoeffGrads). */
const RealVector& RegressOrthogPolyApproximation::
mean_gradient(const RealVector& x, const SizetArray& dvv)
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::mean_gradient(x, dvv);

  // if already computed, return previous result
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const SizetList& nrand_ind = data_rep->nonRandomIndices;
  bool all_mode = !nrand_ind.empty();
  if ( all_mode && (computedMean & 2) &&
       data_rep->match_nonrandom_vars(x, xPrevMeanGrad) ) // && dvv == dvvPrev)
    return meanGradient;

  size_t i, j, deriv_index, num_deriv_vars = dvv.size(),
    cntr = 0; // insertions carried in order within expansionCoeffGrads
  if (meanGradient.length() != num_deriv_vars)
    meanGradient.sizeUninitialized(num_deriv_vars);
  const UShort2DArray& mi = data_rep->multiIndex;
  StSIter it;
  for (i=0; i<num_deriv_vars; ++i) {
    Real& grad_i = meanGradient[i];
    deriv_index = dvv[i] - 1; // OK since we are in an "All" view
    bool random = data_rep->randomVarsKey[deriv_index];
    if (random) { // deriv w.r.t. des var insertion
      if (!expansionCoeffGradFlag) { // error check for required data
	PCerr << "Error: expansion coefficient gradients not defined in Regress"
	      << "OrthogPolyApproximation::mean_gradient()." << std::endl;
	abort_handler(-1);
      }
      grad_i = expansionCoeffGrads[0][cntr];
    }
    else {
      grad_i = 0.;
      if (!expansionCoeffFlag) { // check for reqd data
	PCerr << "Error: expansion coefficients not defined in RegressOrthog"
	      << "PolyApproximation::mean_gradient()" << std::endl;
	abort_handler(-1);
      }
    }
    for (j=1, it=++sparseIndices.begin(); it!=sparseIndices.end(); ++j, ++it) {
      const UShortArray& mi_j = mi[*it];
      // expectations are zero for expansion terms with nonzero random indices
      if (data_rep->zero_random(mi_j)) {
	// In both cases below, term to differentiate is alpha_j(s) Psi_j(s)
	// since <Psi_j>_xi = 1 for included terms.  The difference occurs
	// based on whether a particular s_i dependence appears in alpha
	// (for inserted) or Psi (for augmented).
	if (random)
	  // -------------------------------------------
	  // derivative w.r.t. design variable insertion
	  // -------------------------------------------
	  grad_i += expansionCoeffGrads[j][cntr] * data_rep->
	    multivariate_polynomial(x, mi_j, nrand_ind);
	else
	  // ----------------------------------------------
	  // derivative w.r.t. design variable augmentation
	  // ----------------------------------------------
	  grad_i += expansionCoeffs[j] * data_rep->
	    multivariate_polynomial_gradient(x, deriv_index, mi_j, nrand_ind);
      }
    }
    if (random) // deriv w.r.t. des var insertion
      ++cntr;
  }
  if (all_mode) { computedMean |=  2; xPrevMeanGrad = x; }
  else            computedMean &= ~2; // deactivate 2-bit: protect mixed usage
  return meanGradient;
}


Real RegressOrthogPolyApproximation::
covariance(PolynomialApproximation* poly_approx_2)
{
  RegressOrthogPolyApproximation* ropa_2
    = (RegressOrthogPolyApproximation*)poly_approx_2;
  const SizetSet& sparse_ind_2 = ropa_2->sparseIndices;
  if (sparseIndices.empty() && sparse_ind_2.empty())
    return OrthogPolyApproximation::covariance(poly_approx_2);

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  bool same = (ropa_2 == this), std_mode = data_rep->nonRandomIndices.empty();

  // Error check for required data
  if ( !expansionCoeffFlag ||
       ( !same && !ropa_2->expansionCoeffFlag ) ) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "RegressOrthogPolyApproximation::covariance()" << std::endl;
    abort_handler(-1);
  }

  if (same && std_mode && (computedVariance & 1))
    return expansionMoments[1];

  size_t i1, i2, si1, si2; StSCIter cit1, cit2;
  const UShort2DArray& mi = data_rep->multiIndex;
  const RealVector& exp_coeffs_2 = ropa_2->expansionCoeffs;
  Real covar = 0.;
  if (same)
    for (i1=1, cit1=++sparseIndices.begin(); cit1!=sparseIndices.end();
	 ++i1, ++cit1)
      covar += expansionCoeffs[i1] * expansionCoeffs[i1]
	    *  data_rep->norm_squared(mi[*cit1]);
  else if (sparseIndices.empty()) // mixed mode
    for (i2=1, cit2=++sparse_ind_2.begin(); cit2!=sparse_ind_2.end();
	 ++i2, ++cit2) {
      si2 = *cit2;
      covar += expansionCoeffs[si2] * exp_coeffs_2[i2]
	    *  data_rep->norm_squared(mi[si2]);
    }
  else if (sparse_ind_2.empty()) // mixed mode
    for (i1=1, cit1=++sparseIndices.begin(); cit1!=sparseIndices.end();
	 ++i1, ++cit1) {
      si1 = *cit1;
      covar += expansionCoeffs[i1] * exp_coeffs_2[si1]
	    *  data_rep->norm_squared(mi[si1]);
    }
  else {
    i1=1; i2=1; cit1=++sparseIndices.begin(); cit2=++sparse_ind_2.begin();
    while (cit1!=sparseIndices.end() && cit2!=sparse_ind_2.end()) {
      si1 = *cit1; si2 = *cit2;
      if (si1 == si2) {
	covar += expansionCoeffs[i1] * exp_coeffs_2[i2]
	      *  data_rep->norm_squared(mi[si1]);
	++i1; ++cit1; ++i2; ++cit2;
      }
      else if (si1 < si2) { ++i1; ++cit1; }
      else                { ++i2; ++cit2; }
    }
  }
  if (same && std_mode)
    { expansionMoments[1] = covar; computedVariance |= 1; }
  return covar;
}


Real RegressOrthogPolyApproximation::
covariance(const RealVector& x, PolynomialApproximation* poly_approx_2)
{
  RegressOrthogPolyApproximation* ropa_2
    = (RegressOrthogPolyApproximation*)poly_approx_2;
  const SizetSet& sparse_ind_2 = ropa_2->sparseIndices;
  if (sparseIndices.empty() && sparse_ind_2.empty())
    return OrthogPolyApproximation::covariance(x, poly_approx_2);

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const SizetList& nrand_ind = data_rep->nonRandomIndices;
  bool same = (this == ropa_2), all_mode = !nrand_ind.empty();

  // Error check for required data
  if ( !expansionCoeffFlag ||
       ( !same && !ropa_2->expansionCoeffFlag )) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "RegressOrthogPolyApproximation::covariance()" << std::endl;
    abort_handler(-1);
  }

  if ( same && all_mode && (computedVariance & 1) &&
       data_rep->match_nonrandom_vars(x, xPrevVar) )
    return expansionMoments[1];

  const UShort2DArray&        mi = data_rep->multiIndex;
  const SizetList&      rand_ind = data_rep->randomIndices;
  const RealVector& exp_coeffs_2 = ropa_2->expansionCoeffs;
  Real covar = 0.;
  if (sparseIndices.empty()) { // mixed mode
    size_t i1, i2, num_mi = mi.size(); StSCIter cit2;
    for (i1=1; i1<num_mi; ++i1) {
      const UShortArray& mi1 = mi[i1];
      if (!data_rep->zero_random(mi1)) {
	Real coeff_norm_poly = expansionCoeffs[i1] * 
	  data_rep->norm_squared(mi1, rand_ind) *
	  data_rep->multivariate_polynomial(x, mi1, nrand_ind);
	for (cit2=++sparse_ind_2.begin(), i2=1; cit2!=sparse_ind_2.end();
	     ++cit2, ++i2) {
	  const UShortArray& mi2 = mi[*cit2];
	  if (data_rep->match_random_key(mi1, mi2))
	    covar += coeff_norm_poly * exp_coeffs_2[i2] * 
	      data_rep->multivariate_polynomial(x, mi2, nrand_ind);
	}
      }
    }
  }
  else if (sparse_ind_2.empty()) { // mixed mode
    size_t i1, i2, num_mi = mi.size(); StSCIter cit1;
    for (cit1=++sparseIndices.begin(), i1=1; cit1!=sparseIndices.end();
	 ++cit1, ++i1) {
      const UShortArray& mi1 = mi[*cit1];
      if (!data_rep->zero_random(mi1)) {
	Real coeff_norm_poly = expansionCoeffs[i1] * 
	  data_rep->norm_squared(mi1, rand_ind) *
	  data_rep->multivariate_polynomial(x, mi1, nrand_ind);
	for (i2=1; i2<num_mi; ++i2) {
	  const UShortArray& mi2 = mi[i2];
	  if (data_rep->match_random_key(mi1, mi2))
	    covar += coeff_norm_poly * exp_coeffs_2[i2] * 
	      data_rep->multivariate_polynomial(x, mi2, nrand_ind);
	}
      }
    }
  }
  else {
    size_t i1, i2; StSCIter cit1, cit2;
    for (cit1=++sparseIndices.begin(), i1=1; cit1!=sparseIndices.end();
	 ++cit1, ++i1) {
      // For r = random_vars and nr = non_random_vars,
      // sigma^2_R(nr) = < (R(r,nr) - \mu_R(nr))^2 >_r
      // -> only include terms from R(r,nr) which don't appear in \mu_R(nr)
      const UShortArray& mi1 = mi[*cit1];
      if (!data_rep->zero_random(mi1)) {
	Real coeff_norm_poly = expansionCoeffs[i1] * 
	  data_rep->norm_squared(mi1, rand_ind) *
	  data_rep->multivariate_polynomial(x, mi1, nrand_ind);
	for (cit2=++sparse_ind_2.begin(), i2=1; cit2!=sparse_ind_2.end();
	     ++cit2, ++i2) {
	  const UShortArray& mi2 = mi[*cit2];
	  // random polynomial part must be identical to contribute to variance
	  // (else orthogonality drops term).  Note that it is not necessary to
	  // collapse terms with the same random basis subset, since cross term
	  // in (a+b)(a+b) = a^2+2ab+b^2 gets included.  If terms were collapsed
	  // (following eval of non-random portions), the nested loop could be
	  // replaced with a single loop to evaluate (a+b)^2.
	  if (data_rep->match_random_key(mi1, mi2))
	    covar += coeff_norm_poly * exp_coeffs_2[i2] * 
	      data_rep->multivariate_polynomial(x, mi2, nrand_ind);
	}
      }
    }
  }

  if (same && all_mode)
    { expansionMoments[1] = covar; computedVariance |= 1; xPrevVar = x; }
  return covar;
}


/** In this function, all expansion variables are random variables and
    any design/state variables are omitted from the expansion.  The
    mixed derivative case (some design variables are inserted and some
    are augmented) requires no special treatment. */
const RealVector& RegressOrthogPolyApproximation::variance_gradient()
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::variance_gradient();

  // d/ds \sigma^2_R = Sum_{j=1}^P <Psi^2_j> d/ds \alpha^2_j
  //                 = 2 Sum_{j=1}^P \alpha_j <dR/ds, Psi_j>

  // Error check for required data
  if (!expansionCoeffFlag || !expansionCoeffGradFlag) {
    PCerr << "Error: insufficient expansion coefficient data in RegressOrthog"
	  << "PolyApproximation::variance_gradient()." << std::endl;
    abort_handler(-1);
  }

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  bool std_mode = data_rep->nonRandomIndices.empty();
  if (std_mode && (computedVariance & 2))
    return varianceGradient;

  size_t i, j, num_deriv_vars = expansionCoeffGrads.numRows();
  if (varianceGradient.length() != num_deriv_vars)
    varianceGradient.sizeUninitialized(num_deriv_vars);
  varianceGradient = 0.;
  const UShort2DArray& mi = data_rep->multiIndex;
  StSIter it;
  for (i=1, it=++sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it) {
    Real term_i = 2. * expansionCoeffs[i] * data_rep->norm_squared(mi[*it]);
    for (j=0; j<num_deriv_vars; ++j)
      varianceGradient[j] += term_i * expansionCoeffGrads[i][j];
  }
  if (std_mode) computedVariance |=  2;
  else          computedVariance &= ~2; // deactivate 2-bit: protect mixed usage
  return varianceGradient;
}


/** In this function, a subset of the expansion variables are random
    variables and any augmented design/state variables (i.e., not
    inserted as random variable distribution parameters) are included
    in the expansion.  This function must handle the mixed case, where
    some design/state variables are augmented (and are part of the
    expansion) and some are inserted (derivatives are obtained from
    expansionCoeffGrads). */
const RealVector& RegressOrthogPolyApproximation::
variance_gradient(const RealVector& x, const SizetArray& dvv)
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::variance_gradient(x, dvv);

  // Error check for required data
  if (!expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "RegressOrthogPolyApproximation::variance_gradient()" << std::endl;
    abort_handler(-1);
  }

  // if already computed, return previous result
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const SizetList& nrand_ind = data_rep->nonRandomIndices;
  bool all_mode = !nrand_ind.empty();
  if ( all_mode && (computedVariance & 2) &&
       data_rep->match_nonrandom_vars(x, xPrevVarGrad) ) // && dvv == dvvPrev)
    return varianceGradient;

  size_t i, j, k, deriv_index, num_deriv_vars = dvv.size(),
    cntr = 0; // insertions carried in order within expansionCoeffGrads
  if (varianceGradient.length() != num_deriv_vars)
    varianceGradient.sizeUninitialized(num_deriv_vars);
  varianceGradient = 0.;

  const UShort2DArray&   mi = data_rep->multiIndex;
  const SizetList& rand_ind = data_rep->randomIndices;
  Real norm_sq_j, poly_j, poly_grad_j, norm_poly_j, coeff_j, coeff_grad_j;
  StSIter jit, kit;
  for (i=0; i<num_deriv_vars; ++i) {
    deriv_index = dvv[i] - 1; // OK since we are in an "All" view
    bool random = data_rep->randomVarsKey[deriv_index];
    if (random && !expansionCoeffGradFlag){
      PCerr << "Error: expansion coefficient gradients not defined in Regress"
	    << "OrthogPolyApproximation::variance_gradient()." << std::endl;
      abort_handler(-1);
    }
    for (j=1, jit=++sparseIndices.begin(); jit!=sparseIndices.end();
	 ++j, ++jit) {
      const UShortArray& mi_j = mi[*jit];
      if (!data_rep->zero_random(mi_j)) {
	coeff_j   = expansionCoeffs[j];
	norm_sq_j = data_rep->norm_squared(mi_j, rand_ind);
	poly_j    = data_rep->multivariate_polynomial(x, mi_j, nrand_ind);
	if (random) {
	  norm_poly_j  = norm_sq_j * poly_j;
	  coeff_grad_j = expansionCoeffGrads[j][cntr];
	}
	else
	  poly_grad_j = data_rep->
	    multivariate_polynomial_gradient(x, deriv_index, mi_j, nrand_ind);
	for (k=1, kit=++sparseIndices.begin(); kit!=sparseIndices.end();
	     ++k, ++kit) {
	  // random part of polynomial must be identical to contribute to
	  // variance (else orthogonality drops term)
	  const UShortArray& mi_k = mi[*kit];
	  if (data_rep->match_random_key(mi_j, mi_k)) {
	    // In both cases below, the term to differentiate is
	    // alpha_j(s) alpha_k(s) <Psi_j^2>_xi Psi_j(s) Psi_k(s) and the
	    // difference occurs based on whether a particular s_i dependence
	    // appears in alpha (for inserted) or Psi (for augmented).
	    if (random)
	      // -------------------------------------------
	      // derivative w.r.t. design variable insertion
	      // -------------------------------------------
	      varianceGradient[i] += norm_poly_j *
		(coeff_j * expansionCoeffGrads[k][cntr] +
		 expansionCoeffs[k] * coeff_grad_j) *
		data_rep->multivariate_polynomial(x, mi_k, nrand_ind);
	    else
	      // ----------------------------------------------
	      // derivative w.r.t. design variable augmentation
	      // ----------------------------------------------
	      varianceGradient[i] +=
		coeff_j * expansionCoeffs[k] * norm_sq_j *
		// Psi_j * dPsi_k_ds_i + dPsi_j_ds_i * Psi_k
		(poly_j * data_rep->multivariate_polynomial_gradient(x,
		   deriv_index, mi_k, nrand_ind) +
		 poly_grad_j * data_rep->multivariate_polynomial(x, mi_k,
		   nrand_ind));
	  }
	}
      }
    }
    if (random) // deriv w.r.t. des var insertion
      ++cntr;
  }
  if (all_mode) { computedVariance |=  2; xPrevVarGrad = x; }
  else            computedVariance &= ~2;//deactivate 2-bit: protect mixed usage
  return varianceGradient;
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
       faultInfo.constr_eqns < data_rep->multiIndex.size()); // candidate exp
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
      CSOpts.numFunctionSamples = surrData.points();
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
  num_surr_data_pts = surrData.points();
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
    if (total_eqns < data_rep->multiIndex.size()) // candidate expansion size
      under_determined = true;
  }
  if (expansionCoeffGradFlag) {
    total_eqns = (anchor_grad) ? num_data_pts_grad+1 : num_data_pts_grad;
    if (total_eqns < data_rep->multiIndex.size()) // candidate expansion size
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

  size_t i, j, a_cntr = 0, b_cntr = 0, num_surr_data_pts = surrData.points(),
    num_deriv_vars = surrData.num_derivative_variables(),
    num_v = sharedDataRep->numVars;
  int num_rows_A =  0, // number of rows in matrix A
      num_cols_A = data_rep->multiIndex.size(), // candidate expansion size
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

    if (data_rep->expConfigOptions.expCoeffsSolnApproach !=
	ORTHOG_LEAST_INTERPOLATION) {
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
      update_sparse_sobol(data_rep->multiIndex, data_rep->sobolIndexMap);
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


void RegressOrthogPolyApproximation::
update_sparse(Real* dense_coeffs, size_t num_dense_terms)
{
  // just one pass through to define sparseIndices
  sparseIndices.clear();
  update_sparse_indices(dense_coeffs, num_dense_terms);

  // update expansionCoeffs
  update_sparse_coeffs(dense_coeffs);

  // update the sparse Sobol' indices
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  update_sparse_sobol(data_rep->multiIndex, data_rep->sobolIndexMap);
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


void RegressOrthogPolyApproximation::
update_multi_index(const UShort2DArray& mi_subset, bool track_sparse)
{
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  UShort2DArray& mi_shared = data_rep->multiIndex;

  // update sparseIndices based on a local subset of shared multiIndex.
  // OLI does not employ an upper bound candidate multiIndex, so we must
  // incrementally define it here.
  sparseIndices.clear();
  size_t i, num_mi_subset = mi_subset.size(), num_mi_shared = mi_shared.size();
  if (!num_mi_shared) {
    mi_shared = mi_subset;
    // use sparseIndices even though not currently a subset, since
    // subsequent QoI could render this a subset.
    if (track_sparse)
      for (i=0; i<num_mi_subset; ++i)
	sparseIndices.insert(i);
  }
  else {
    for (i=0; i<num_mi_subset; ++i) {
      if (track_sparse)
	sparseIndices.insert(i);
      if (i<num_mi_shared) {
	// For efficiency in current use cases, assume/enforce that mi_subset
	// is a leading subset with ordering that is consistent with mi_shared
	if (mi_subset[i] != mi_shared[i]) {
	  PCerr << "Error: simplifying assumption violated in RegressOrthogPoly"
		<< "Approximation::update_sparse_indices(UShort2DArray&)."
		<< std::endl;
	  abort_handler(-1);
	}
      }
      else
	mi_shared.push_back(mi_subset[i]);
    }
  }
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


void RegressOrthogPolyApproximation::
update_sparse_sobol(const UShort2DArray& shared_multi_index,
		    const BitArrayULongMap& shared_sobol_map)
{
  // define the Sobol' indices based on the sparse multiIndex
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  if ( !data_rep->expConfigOptions.vbdFlag ||
        data_rep->expConfigOptions.vbdOrderLimit == 1 )
    return;

  sparseSobolIndexMap.clear();
  if (sparseIndices.empty()) {
    size_t sobol_len = shared_sobol_map.size();
    if (sobolIndices.length() != sobol_len)
      sobolIndices.sizeUninitialized(sobol_len);
    return;
  }

  // update sparseSobolIndexMap from shared sobolIndexMap.  Since
  // sparseSobolIndexMap is sorted by the sobolIndexMap map-values
  // (indices within a dense sobolIndices), we know that the ordering
  // of the sparse interactions will be consistent, e.g.:
  // sparseSobolIndexMap keys:   0, 2, 4, 5, 9, 11 (from sobolIndexMap)
  // sparseSobolIndexMap values: 0, 1, 2, 3, 4, 5  (new sobolIndices sequence)
  size_t j, num_v = sharedDataRep->numVars;
  StSIter sit; BAULMCIter cit; BitArray set(num_v);
  for (sit=sparseIndices.begin(); sit!=sparseIndices.end(); ++sit) {
    const UShortArray& sparse_mi = shared_multi_index[*sit];
    for (j=0; j<num_v; ++j)
      if (sparse_mi[j]) set.set(j);   //   activate bit j
      else              set.reset(j); // deactivate bit j
    // define map from shared index to sparse index
    cit = shared_sobol_map.find(set);
    if (cit == shared_sobol_map.end()) {
      PCerr << "Error: sobolIndexMap lookup failure in RegressOrthogPoly"
	    << "Approximation::update_sparse_sobol()" << std::endl;
      abort_handler(-1);
    }
    // key = shared index, value = sparse index (init to 0, to be updated below)
    sparseSobolIndexMap[cit->second] = 0;
  }
  // now that keys are complete, assign new sequence for sparse Sobol indices
  unsigned long sobol_len = 0; ULULMIter mit;
  for (mit=sparseSobolIndexMap.begin(); mit!=sparseSobolIndexMap.end(); ++mit)
    mit->second = sobol_len++;
  // now size sobolIndices
  if (sobolIndices.length() != sobol_len)
    sobolIndices.sizeUninitialized(sobol_len);
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
    if (num_basis_terms < data_rep->multiIndex.size()) // candidate exp size
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

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;

  // must do this inside transform_least_interpolant
  //RealVector domain;
  //TensorProductBasis_ptr basis; 
  //std::vector<PolyIndex_ptr> basis_indices;
  //pce.get_domain( domain );
  //pce.get_basis( basis );
  //pce.get_basis_indices( basis_indices );

  // if no sim faults on subsequent QoI and previous QoI interp size matches,
  // then reuse previous factorization.  Detecting non-null fault sets that are
  // consistent is more complicated and would require changes to the Dakota::
  // ApproximationInterface design to propagate fault deltas among the QoI.
  // Note 1: size logic is more general than currently necessary since OLI does
  //         not currently support deriv enhancement.
  // Note 2: multiIndex size check captures first QoI pass as well as reentrancy
  //         (changes in points sets for OUU and mixed UQ) due to clear() in
  //         SharedRegressOrthogPolyApproxData::allocate_data().
  bool faults = ( surrData.failed_anchor_data() ||
		  surrData.failed_response_data().size() ),
    inconsistent_prev = ( data_rep->multiIndex.empty() ||
      surrData.active_response_size() != data_rep->pivotHistory.numRows() );
  if (faults || inconsistent_prev) {
    // compute the least factorization that interpolates this data set
    UShort2DArray local_multi_index; IntVector k;
    least_factorization( pts, local_multi_index, data_rep->lowerFactor,
			 data_rep->upperFactor, data_rep->pivotHistory,
			 data_rep->pivotVect, k );
    // update approxOrder (for use in, e.g., combine_coefficients())
    int last_index = k.length() - 1, new_order = k[last_index];
    data_rep->update_approx_order(new_order);
    // update sparseIndices and shared multiIndex from local_multi_index
    update_multi_index(local_multi_index, true);
    // update shared sobolIndexMap from local_multi_index
    data_rep->update_component_sobol(local_multi_index);
  }
  else {
    // define sparseIndices for this QoI (sparseSobolIndexMap updated below)
    size_t i, num_mi = data_rep->multiIndex.size();
    for (i=0; i<num_mi; ++i)
      sparseIndices.insert(i);
  }
  // define sparseSobolIndexMap from sparseIndices, shared multiIndex,
  // and shared sobolIndexMap
  update_sparse_sobol(data_rep->multiIndex, data_rep->sobolIndexMap);

  RealMatrix coefficients;
  transform_least_interpolant( data_rep->lowerFactor,  data_rep->upperFactor,
			       data_rep->pivotHistory, data_rep->pivotVect,
			       vals );

  // must do this inside transform_least_interpolant
  //pce.set_basis_indices( basis_indices );
  //pce.set_coefficients( coefficients );
}


void RegressOrthogPolyApproximation::
transform_least_interpolant( RealMatrix &L, RealMatrix &U, RealMatrix &H,
			     IntVector &p,  RealMatrix &vals )
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
  copy_data(coefficients.values(), (int)sparseIndices.size(), expansionCoeffs);
}


void RegressOrthogPolyApproximation::
least_factorization( RealMatrix &pts, UShort2DArray &basis_indices,
		     RealMatrix &l, RealMatrix &u, RealMatrix &H,
		     IntVector &p, IntVector &k )
{
  int num_vars = pts.numRows(), num_pts = pts.numCols();

  eye( num_pts, l );
  eye( num_pts, u );

  range( p, 0, num_pts, 1 );

  //This is just a guess: this vector could be much larger, or much smaller
  RealVector v( 1000 );
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


void RegressOrthogPolyApproximation::compute_component_sobol()
{
  if (sparseIndices.empty())
    { OrthogPolyApproximation::compute_component_sobol(); return; }

  // sobolIndices are indexed via a bit array, one bit per variable.
  // A bit is turned on for an expansion term if there is a variable
  // dependence (i.e., its multi-index component is non-zero).  Since
  // the Sobol' indices involve a consolidation of variance contributions
  // from the expansion terms, we define a bit array from the multIndex
  // and then use a lookup within sobolIndexMap to assign the expansion
  // term contribution to the correct Sobol' index.

  // iterate through multiIndex and store sensitivities.  Note: sobolIndices[0]
  // (corresponding to constant exp term with no variable dependence) is unused.
  sobolIndices = 0.; // initialize

  // compute and sum the variance contributions for each expansion term.  For
  // all_vars mode, this approach picks up the total expansion variance, which
  // is the desired reference pt for type-agnostic global sensitivity analysis.
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& mi = data_rep->multiIndex;
  const BitArrayULongMap& index_map = data_rep->sobolIndexMap;
  size_t i, j, num_v = sharedDataRep->numVars; StSIter it;
  BitArray set(num_v);
  Real p_var, sum_p_var = 0.;
  for (i=1, it=++sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it) {
    const UShortArray& mi_i = mi[*it];
    p_var = expansionCoeffs(i) * expansionCoeffs(i)
          * data_rep->norm_squared(mi_i);
    sum_p_var += p_var;

    // determine the bit set corresponding to this expansion term
    for (j=0; j<num_v; ++j)
      if (mi_i[j]) set.set(j);   //   activate bit j
      else         set.reset(j); // deactivate bit j

    // lookup the bit set within sobolIndexMap --> increment the correct
    // Sobol' index with the variance contribution from this expansion term.
    BAULMCIter cit = index_map.find(set);
    if (cit != index_map.end()) { // may not be found if vbdOrderLimit
      unsigned long sp_index = sparseSobolIndexMap[cit->second];
      sobolIndices[sp_index] += p_var; // divide by sum_p_var below
    }
  }
  if (sum_p_var > SMALL_NUMBER) // don't attribute variance if zero/negligible
    sobolIndices.scale(1./sum_p_var);

#ifdef DEBUG
  PCout << "In RegressOrthogPolyApproximation::compute_component_sobol(), "
	<< "sobolIndices =\n"; write_data(PCout, sobolIndices);
#endif // DEBUG
}


void RegressOrthogPolyApproximation::compute_total_sobol() 
{
  if (sparseIndices.empty())
    { OrthogPolyApproximation::compute_total_sobol(); return; }

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  size_t j, num_v = sharedDataRep->numVars;
  const UShort2DArray& mi = data_rep->multiIndex;
  totalSobolIndices = 0.;
  if (data_rep->expConfigOptions.vbdOrderLimit) {
    // all component indices may not be available, so compute total indices
    // from scratch by computing and summing variance contributions for each
    // expansion term
    size_t i, num_exp_terms = sparseIndices.size();
    Real p_var, sum_p_var = 0., ratio_i; StSIter it;
    for (i=1, it=++sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it) {
      const UShortArray& mi_i = mi[*it];
      p_var = expansionCoeffs(i) * expansionCoeffs(i)
	    * data_rep->norm_squared(mi_i);
      sum_p_var += p_var;
      // for any constituent variable j in exansion term i, the expansion
      // term contributes to the total sensitivity of variable j
      for (j=0; j<num_v; ++j)
	if (mi_i[j])
	  totalSobolIndices[j] += p_var; // divide by sum_p_var outside loop
    }
    // if negligible variance (e.g., a deterministic test fn), then attribution
    // of this variance is suspect.  Defaulting totalSobolIndices to zero is a
    // good choice since it drops out from anisotropic refinement based on the
    // response-average of these indices.
    if (sum_p_var > SMALL_NUMBER) // avoid division by zero
      totalSobolIndices.scale(1./sum_p_var);
  }
  else {
    const BitArrayULongMap& index_map = data_rep->sobolIndexMap;
    // all component effects have been computed, so add them up:
    // totalSobolIndices parses the bit sets of each of the sobolIndices
    // and adds them to each matching variable bin
    // Note: compact iteration over sparseSobolIndexMap could be done but
    //       requires a value to key mapping to get from uit->first to set.
    for (BAULMCIter cit=index_map.begin(); cit!=index_map.end(); ++cit) {
      ULULMIter uit = sparseSobolIndexMap.find(cit->second);
      if (uit != sparseSobolIndexMap.end()) {
	const BitArray& set = cit->first;
	Real comp_sobol = sobolIndices[uit->second];
	for (j=0; j<num_v; ++j) 
	  if (set[j]) // var j is present in this Sobol' index
	    totalSobolIndices[j] += comp_sobol;
      }
    }
  }

#ifdef DEBUG
  PCout << "In RegressOrthogPolyApproximation::compute_total_sobol(), "
	<< "totalSobolIndices =\n"; write_data(PCout, totalSobolIndices);
#endif // DEBUG
}


const RealVector& RegressOrthogPolyApproximation::dimension_decay_rates()
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::dimension_decay_rates();

  size_t i, j, num_exp_terms = sparseIndices.size(),
    num_v = sharedDataRep->numVars;
  if (decayRates.empty())
    decayRates.sizeUninitialized(num_v);

  // define max_orders for each var for sizing LLS matrices/vectors
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& mi = data_rep->multiIndex;
  UShortArray max_orders(num_v, 0); StSIter it;
  for (it=++sparseIndices.begin(); it!=sparseIndices.end(); ++it) {
    const UShortArray& mi_i = mi[*it];
    for (j=0; j<num_v; ++j)
      if (mi_i[j] > max_orders[j])
	max_orders[j] = mi_i[j];
  }

  // size A_vectors and b_vectors and initialize with defaults since
  // sparseIndices could leave gaps
  RealVectorArray A_vectors(num_v), b_vectors(num_v);
  unsigned short order, non_zero, var_index, order_index;
  Real norm;
  for (i=0; i<num_v; ++i) {
    unsigned short max_ord = max_orders[i];
    RealVector& A_i = A_vectors[i]; A_i.sizeUninitialized(max_ord);
    RealVector& b_i = b_vectors[i]; b_i.sizeUninitialized(max_ord);
    BasisPolynomial& poly_i = data_rep->polynomialBasis[i];
    for (j=0; j<max_ord; ++j) {
      order = j + 1;
      A_i[j] = (Real)order;
      // log(norm * 1.e-25) for cut-off coeff value of 1.e-25
      b_i[j] = std::log10(poly_i.norm_squared(order))/2. - 25.; // updated below
    }
  }

  // populate A_vectors and b_vectors
  bool univariate;
  for (it=++sparseIndices.begin(), i=1; it!=sparseIndices.end(); ++it, ++i) {
    const UShortArray& mi_i = mi[*it];
    univariate = true; non_zero = 0;
    for (j=0; j<num_v; ++j) {
      if (mi_i[j]) {
	++non_zero;
	if (non_zero > 1) { univariate = false; break; }
	else { order = mi_i[j]; var_index = j; order_index = order-1; }
      }
    }
    if (univariate) {
      // find a for y = ax + b with x = term order, y = log(coeff), and
      // b = known intercept for order x = 0
      Real abs_coeff = std::abs(expansionCoeffs[i]);
#ifdef DECAY_DEBUG
      PCout << "Univariate contribution: order = " << order << " coeff = "
	    << abs_coeff << " norm = " << std::sqrt(data_rep->
	       polynomialBasis[var_index].norm_squared(order)) << '\n';
#endif // DECAY_DEBUG
      if (abs_coeff > 1.e-25)
	// b = std::log10(abs_coeff * norm), but don't recompute norm
	b_vectors[var_index][order_index] += 25. + std::log10(abs_coeff);
    }
  }
#ifdef DECAY_DEBUG
  PCout << "raw b_vectors:\n";
  for (i=0; i<num_v; ++i)
    { PCout << "Variable " << i+1 << '\n'; write_data(PCout, b_vectors[i]); }
#endif // DECAY_DEBUG

  solve_decay_rates(A_vectors, b_vectors, max_orders);
  return decayRates;
}


RealVector RegressOrthogPolyApproximation::dense_coefficients() const
{
  if (sparseIndices.empty())
    return OrthogPolyApproximation::dense_coefficients();

  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& mi = data_rep->multiIndex;
  RealVector dense_coeffs(mi.size()); // init to 0
  size_t i; StSIter it;
  for (i=0, it=sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it)
    dense_coeffs[*it] = expansionCoeffs[i];
  return dense_coeffs;
}


void RegressOrthogPolyApproximation::
print_coefficients(std::ostream& s, bool normalized)
{
  if (sparseIndices.empty())
    { OrthogPolyApproximation::print_coefficients(s, normalized); return; }

  size_t i, j, num_v = sharedDataRep->numVars;
  StSIter it;
  char tag[10];

  // terms and term identifiers
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& mi = data_rep->multiIndex;
  for (i=0, it=sparseIndices.begin(); it!=sparseIndices.end(); ++i, ++it) {
    const UShortArray& mi_i = mi[*it];
    s << "\n  " << std::setw(WRITE_PRECISION+7);
    if (normalized) // basis is divided by norm, so coeff is multiplied by norm
      s << expansionCoeffs[i] * std::sqrt(data_rep->norm_squared(mi_i));
    else
      s << expansionCoeffs[i];
    for (j=0; j<num_v; ++j) {
      data_rep->get_tag(tag, j, mi_i[j]);
      s << std::setw(5) << tag;
    }
  }
  s << '\n';
}


void RegressOrthogPolyApproximation::
coefficient_labels(std::vector<std::string>& coeff_labels) const
{
  if (sparseIndices.empty())
    { OrthogPolyApproximation::coefficient_labels(coeff_labels); return; }

  size_t i, j, num_v = sharedDataRep->numVars;
  char tag[10];

  coeff_labels.reserve(sparseIndices.size());

  // terms and term identifiers
  SharedRegressOrthogPolyApproxData* data_rep
    = (SharedRegressOrthogPolyApproxData*)sharedDataRep;
  const UShort2DArray& mi = data_rep->multiIndex;
  for (StSIter it=sparseIndices.begin(); it!=sparseIndices.end(); ++it) {
    const UShortArray& mi_i = mi[*it];
    std::string tags;
    for (j=0; j<num_v; ++j) {
      if (j) tags += ' ';
      data_rep->get_tag(tag, j, mi_i[j]);
      tags += tag;
    }
    coeff_labels.push_back(tags);
  }
}

} // namespace Pecos
