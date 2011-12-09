/*  _______________________________________________________________________

    PECOS: Parallel Environment for Creation Of Stochastics
    Copyright (c) 2011, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Pecos directory.
    _______________________________________________________________________ */

//- Class:        OrthogPolyApproximation
//- Description:  Implementation code for OrthogPolyApproximation class
//-               
//- Owner:        Mike Eldred

#include "OrthogPolyApproximation.hpp"
#include "TensorProductDriver.hpp"
#include "SparseGridDriver.hpp"
#include "CubatureDriver.hpp"
#include "pecos_global_defs.hpp"
#include "Teuchos_LAPACK.hpp"
#include "Teuchos_SerialDenseHelpers.hpp"

//#define DEBUG
//#define DECAY_DEBUG


namespace Pecos {

/** This version supports only orthogonal polynomial types.  In this
    case, the polynomial types needed for an orthogonal basis and for
    computing collocation points and weights in an integration driver
    are the same. */
bool OrthogPolyApproximation::
initialize_basis_types(const ShortArray& u_types, ShortArray& basis_types)
{
  bool extra_dist_params = false;

  // Initialize basis_types and extra_dist_params from u_types.
  size_t i, num_vars = u_types.size();
  if (basis_types.size() != num_vars)
    basis_types.resize(num_vars);
  for (i=0; i<num_vars; ++i) {
    switch (u_types[i]) {
    case STD_NORMAL:      basis_types[i] = HERMITE_ORTHOG;            break;
    case STD_UNIFORM:     basis_types[i] = LEGENDRE_ORTHOG;           break;
    // weight fn = 1/sqrt(1-x^2); same as BETA/Jacobi for alpha=beta=-1/2
    //case xxx:           basis_types[i] = CHEBYSHEV_ORTHOG;          break;
    case STD_EXPONENTIAL: basis_types[i] = LAGUERRE_ORTHOG;           break;
    case STD_BETA:
      basis_types[i] = JACOBI_ORTHOG;       extra_dist_params = true; break;
    case STD_GAMMA:
      basis_types[i] = GEN_LAGUERRE_ORTHOG; extra_dist_params = true; break;
    default:
      basis_types[i] = NUM_GEN_ORTHOG;      extra_dist_params = true; break;
    }
  }

  return extra_dist_params;
}


int OrthogPolyApproximation::min_coefficients() const
{
  // return the minimum number of data instances required to build the 
  // surface of multiple dimensions
  if (expConfigOptions.expansionCoeffFlag ||
      expConfigOptions.expansionCoeffGradFlag)
    switch (expConfigOptions.expCoeffsSolnApproach) {
    case QUADRATURE: case CUBATURE: case SPARSE_GRID: case SAMPLING:
      return 1; // quadrature: (int)pow((Real)MIN_GAUSS_PTS, numVars);
      break;
    case REGRESSION:
      // numExpansionTerms is either set from the NonDPolynomialChaos ctor if
      // expansion_terms is specified or computed by the allocate_arrays() call
      // in compute_coefficients() if expansion_order is specified.  The latter
      // case is too late for use of this fn by ApproximationInterface::
      // minimum_samples() in DataFitSurrModel::build_global(), so
      // numExpansionTerms must be calculated for this case.
      return (numExpansionTerms) ?
	numExpansionTerms : total_order_terms(approxOrder);
      break;
    case -1: default: // coefficient import 
      return 0;
      break;
    }
  else
    return 0;
}


void OrthogPolyApproximation::allocate_arrays()
{
  allocate_component_effects();
  allocate_total_effects();

  // Infer expansion formulation from quadrature_order or sparse_grid_level
  // spec, as in SC.  Preserve previous capability (quadrature_order and
  // sparse_grid_level with total-order expansions) for paper results via
  // quadratureExpansion/sparseGridExpansion (compile-time) switches.

  // update_exp_form controls when to update (refinement) and when not to
  // update (subIterator execution) an expansion's multiIndex definition.
  // Simple logic of updating if previous number of points != current number
  // is not robust enough for anisotropic updates --> track using Prev arrays.
  switch (expConfigOptions.expCoeffsSolnApproach) {
  case QUADRATURE: {
    TensorProductDriver* tpq_driver = (TensorProductDriver*)driverRep;
    const UShortArray& quad_order = tpq_driver->quadrature_order();
    bool update_exp_form = (quad_order != quadOrderPrev);
    // *** TO DO: capture updates to parameterized/numerical polynomials?

    if (update_exp_form) {
      UShortArray integrand_order(numVars);
      quadrature_order_to_integrand_order(quad_order, integrand_order);
      integrand_order_to_expansion_order(integrand_order, approxOrder);
    }
    PCout << "Orthogonal polynomial approximation order = { ";
    for (size_t i=0; i<numVars; ++i)
      PCout << approxOrder[i] << ' ';
    switch (quadratureExpansion) {
    case TENSOR_INT_TENSOR_EXP:
      if (update_exp_form)
	tensor_product_multi_index(approxOrder, multiIndex);
      PCout << "} using tensor-product expansion of "; break;
    case TENSOR_INT_TOTAL_ORD_EXP:
      if (update_exp_form)
	total_order_multi_index(approxOrder, multiIndex);
      PCout << "} using total-order expansion of ";    break;
    default:
      PCerr << "}\n\nError: unsupported setting for quadratureExpansion in "
	    << "OrthogPolyApproximation::allocate_arrays()." << std::endl;
      abort_handler(-1);                               break;
    }
    if (update_exp_form)
      numExpansionTerms = multiIndex.size();
    PCout << numExpansionTerms << " terms\n";
    // update reference points
    quadOrderPrev = quad_order;
    break;
  }
  case CUBATURE: {
    CubatureDriver* cub_driver = (CubatureDriver*)driverRep;
    //unsigned short cub_int_order = cub_driver->integrand_order();
    //bool update_exp_form = (cub_int_order != cubIntOrderPrev);

    UShortArray integrand_order(numVars, cub_driver->integrand_order());
    integrand_order_to_expansion_order(integrand_order, approxOrder);
    total_order_multi_index(approxOrder, multiIndex);
    numExpansionTerms = multiIndex.size();
    PCout << "Orthogonal polynomial approximation order = { ";
    for (size_t i=0; i<numVars; ++i)
      PCout << approxOrder[i] << ' ';
    PCout << "} using total-order expansion of " << numExpansionTerms
	  << " terms\n";
    // update reference points
    //cubIntOrderPrev = cub_int_order;
    break;
  }
  case SPARSE_GRID: {
    SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
    unsigned short    ssg_level  = ssg_driver->level();
    const RealVector& aniso_wts  = ssg_driver->anisotropic_weights();
    bool update_exp_form = (ssg_level != ssgLevelPrev ||
      aniso_wts != ssgAnisoWtsPrev || expConfigOptions.refinementControl ==
      DIMENSION_ADAPTIVE_GENERALIZED_SPARSE);
    // *** TO DO: capture updates to parameterized/numerical polynomials?

    if (update_exp_form) { // compute and output number of terms
      sparse_grid_multi_index(multiIndex);
      numExpansionTerms = multiIndex.size();
    }
    switch (sparseGridExpansion) {
    case TENSOR_INT_TENSOR_SUM_EXP:
      PCout << "Orthogonal polynomial approximation level = " << ssg_level
	    << " using tensor integration and tensor sum expansion of "
	    << numExpansionTerms << " terms\n"; break;
    case SPARSE_INT_TENSOR_SUM_EXP: case SPARSE_INT_RESTR_TENSOR_SUM_EXP:
      PCout << "Orthogonal polynomial approximation level = " << ssg_level
	    << " using sparse integration and tensor sum expansion of "
	    << numExpansionTerms << " terms\n"; break;
    case SPARSE_INT_TOTAL_ORD_EXP:  case SPARSE_INT_HEUR_TOTAL_ORD_EXP:
      PCout << "Orthogonal polynomial approximation order = { ";
      for (size_t i=0; i<numVars; ++i)
	PCout << approxOrder[i] << ' ';
      PCout << "} using sparse integration and total-order expansion of "
	    << numExpansionTerms << " terms\n"; break;
    default:
      PCerr << "Error: unsupported setting for sparseGridExpansion in "
	    << "OrthogPolyApproximation::allocate_arrays()." << std::endl;
      abort_handler(-1);                        break;
    }
    // update reference points
    ssgLevelPrev = ssg_level; ssgAnisoWtsPrev = aniso_wts;
    break;
  }
  default: { // SAMPLING and REGRESSION
    // For uniform refinement, only the initial reference expansion supports
    // a partial total-order definition.  All subsequent refinements will be
    // based off of approxOrder.  For PCBDO, numExpansionTerms and approxOrder
    // are invariant and a multiIndex update is prevented by update_exp_form.
    if (!approxOrder.empty()) {
      bool update_exp_form = (approxOrder != approxOrderPrev);
      if (update_exp_form) {
	size_t order_len = approxOrder.size();
	if (order_len != numVars) {
	  if (order_len == 1) {
	    unsigned short order = approxOrder[0];
	    approxOrder.assign(numVars, order);
	  }
	  else {
	    PCerr << "Error: expansion_order specification length does not "
		  << "match number of active variables." << std::endl;
	    abort_handler(-1);
	  }
	}
	total_order_multi_index(approxOrder, multiIndex);
	numExpansionTerms = multiIndex.size();
	partialOrder = false;
      }
      // update reference point
      approxOrderPrev = approxOrder;
    }
    else if (numExpansionTerms) { // default is 0
      unsigned short order = 0; size_t full_order_expansion = 1;
      while (numExpansionTerms > full_order_expansion) {
	++order;
	// do in 2 steps rather than 1 to avoid truncation from int division
	full_order_expansion *= numVars+order;
	full_order_expansion /= order;
      }
      partialOrder = (numExpansionTerms != full_order_expansion);
      // define approxOrder to be isotropic and full total-order, but
      // restrict the multiIndex update according to numExpansionTerms.
      // Any subsequent uniform refinement increments approxOrder and
      // loses this restriction.  Subsequent PCBDO invocations retain
      // this restriction by nature of the update_exp_form protection.
      approxOrder.assign(numVars, order);
      total_order_multi_index(approxOrder, multiIndex, -1, numExpansionTerms);
    }
    else {
      PCerr << "Error: bad expansion specification in "
	    << "OrthogPolyApproximation::allocate_arrays()." << std::endl;
      abort_handler(-1);
    }
    // output expansion form
    PCout << "Orthogonal polynomial approximation order = ";
    if (partialOrder)
      PCout << approxOrder[0] << " using partial ";
    else {
      PCout << "{ ";
      for (size_t i=0; i<numVars; ++i)
	PCout << approxOrder[i] << ' ';
      PCout << "} using ";
    }
    PCout << "total-order expansion of " << numExpansionTerms << " terms\n";
    break;
  }
  }

  // now that terms & order are known, shape some arrays.  This is done here,
  // rather than in compute_coefficients(), in order to support array sizing
  // for the data import case.
  if (expConfigOptions.expansionCoeffFlag &&
      expansionCoeffs.length() != numExpansionTerms)
    expansionCoeffs.sizeUninitialized(numExpansionTerms);
  if (expConfigOptions.expansionCoeffGradFlag) {
    size_t num_deriv_vars = surrData.num_derivative_variables();
    if (expansionCoeffGrads.numRows() != num_deriv_vars ||
	expansionCoeffGrads.numCols() != numExpansionTerms)
      expansionCoeffGrads.shapeUninitialized(num_deriv_vars, numExpansionTerms);
  }

  // no combination by default, even if storedMultiIndex and
  // storedExp{Coeffs,CoeffGrads} are defined.  Redefined by
  // attribute passed in combine_coefficients(short).
  storedExpCombineType = NO_COMBINE;
}


void OrthogPolyApproximation::compute_coefficients()
{
  if (!expConfigOptions.expansionCoeffFlag &&
      !expConfigOptions.expansionCoeffGradFlag) {
    PCerr << "Warning: neither expansion coefficients nor expansion "
	  << "coefficient gradients\n         are active in "
	  << "OrthogPolyApproximation::compute_coefficients().\n         "
	  << "Bypassing approximation construction." << std::endl;
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
  //   QUADRATURE/CUBATURE/SPARSE_GRID: error
  //   REGRESSION: use equality-constrained least squares
  size_t i, j, num_total_pts = surrData.size();
  if (surrData.anchor())
    ++num_total_pts;
  if (!num_total_pts) {
    PCerr << "Error: nonzero number of sample points required in "
	  << "OrthogPolyApproximation::compute_coefficients()." << std::endl;
    abort_handler(-1);
  }

  // Array sizing can be divided into two parts:
  // > data used in all cases (size in allocate_arrays())
  // > data not used in expansion import case (size here)
  allocate_arrays();
#ifdef DEBUG
  gradient_check();
#endif // DEBUG

  // calculate polynomial chaos coefficients
  switch (expConfigOptions.expCoeffsSolnApproach) {
  case QUADRATURE: {
    // verify quad_order stencil matches num_total_pts
    TensorProductDriver* tpq_driver = (TensorProductDriver*)driverRep;
    const UShortArray& quad_order = tpq_driver->quadrature_order();
    if (quad_order.size() != numVars) {
      PCerr << "Error: quadrature order array is not consistent with number of "
	    << "variables (" << numVars << ")\n       in "
	    << "OrthogPolyApproximation::compute_coefficients()." << std::endl;
      abort_handler(-1);
    }
    size_t num_colloc_pts = 1;
    for (i=0; i<numVars; ++i)
      num_colloc_pts *= quad_order[i];
    if (num_total_pts != num_colloc_pts) {
      PCerr << "Error: number of current points (" << num_total_pts
	    << ") is not consistent with\n       quadrature data in "
	    << "OrthogPolyApproximation::compute_coefficients()." << std::endl;
      abort_handler(-1);
    }

#ifdef DEBUG
    for (i=0; i<numVars; ++i) {
      OrthogonalPolynomial* poly_rep
	= (OrthogonalPolynomial*)polynomialBasis[i].polynomial_rep();
      for (j=1; j<=quad_order[i]; ++j)
	poly_rep->gauss_check(j);
    }
#endif // DEBUG

    // single expansion integration
    integration_checks();
    integrate_expansion(multiIndex, surrData.variables_data(),
			surrData.response_data(),
			driverRep->type1_weight_sets(),
			expansionCoeffs, expansionCoeffGrads);
    break;
  }
  case CUBATURE:
    // single expansion integration
    integration_checks();
    integrate_expansion(multiIndex, surrData.variables_data(),
			surrData.response_data(),
			driverRep->type1_weight_sets(),
			expansionCoeffs, expansionCoeffGrads);
    break;
  case SPARSE_GRID:
    switch (sparseGridExpansion) {
    case TENSOR_INT_TENSOR_SUM_EXP: {
      // multiple expansion integration
      // Note: allocate_arrays() calls sparse_grid_multi_index() which uses
      // append_multi_index() to build multiIndex.
      if (expConfigOptions.expansionCoeffFlag)     expansionCoeffs = 0.;
      if (expConfigOptions.expansionCoeffGradFlag) expansionCoeffGrads = 0.;
      SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
      const IntArray& sm_coeffs = ssg_driver->smolyak_coefficients();
      size_t i, num_tensor_grids = tpMultiIndex.size(); int coeff;
      SDVArray tp_data_vars; SDRArray tp_data_resp;
      RealVector tp_wts, tp_coeffs; RealMatrix tp_coeff_grads;
      bool store_tp = (expConfigOptions.refinementControl ==
		       DIMENSION_ADAPTIVE_GENERALIZED_SPARSE);
      // loop over tensor-products, forming sub-expansions, and sum them up
      for (i=0; i<num_tensor_grids; ++i) {
	// form tp_data_vars, tp_data_resp, tp_wts using collocKey et al.
	integration_data(i, tp_data_vars, tp_data_resp, tp_wts);

	// form tp_multi_index from tpMultiIndexMap
	//map_tensor_product_multi_index(tp_multi_index, i);

	// form tp expansion coeffs
	RealVector& tp_coeffs_i = (store_tp) ? tpExpansionCoeffs[i] : tp_coeffs;
	RealMatrix& tp_grads_i
	  = (store_tp) ? tpExpansionCoeffGrads[i] : tp_coeff_grads;
	integrate_expansion(tpMultiIndex[i], tp_data_vars, tp_data_resp, tp_wts,
			    tp_coeffs_i, tp_grads_i);

	// sum tensor product coeffs/grads into expansion coeffs/grads
	coeff = sm_coeffs[i];
	if (coeff)
	  overlay_expansion(tpMultiIndexMap[i], tp_coeffs_i, tp_grads_i, coeff);
      }
      //if (!reEntrantFlag) {
      //  ssg_driver->clear_smolyak_arrays();
      //  ssg_driver->clear_collocation_arrays();
      //  tpMultiIndex.clear(); tpMultiIndexMap.clear();
      //}
      break;
    }
    default: // SPARSE_INT_*
      // single expansion integration
      integration_checks();
      integrate_expansion(multiIndex, surrData.variables_data(),
			  surrData.response_data(),
			  driverRep->type1_weight_sets(),
			  expansionCoeffs, expansionCoeffGrads);
      break;
    }
    break;
  case REGRESSION:
    surrData.data_checks();
    regression();
    break;
  case SAMPLING:
    surrData.data_checks();
    expectation();
    break;
  }
}


void OrthogPolyApproximation::increment_coefficients()
{
  bool err_flag = false;
  size_t last_index;
  switch (expConfigOptions.expCoeffsSolnApproach) {
  case SPARSE_GRID: {
    SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
    switch (sparseGridExpansion) {
    case TENSOR_INT_TENSOR_SUM_EXP: {
      last_index = tpMultiIndex.size();
      // size tpMultiIndex and tpExpansion{Coeffs,CoeffGrads}
      size_t new_size = last_index+1;
      tpMultiIndex.resize(new_size);
      tpExpansionCoeffs.resize(new_size);
      tpExpansionCoeffGrads.resize(new_size);
      // update tpMultiIndex
      UShortArray exp_order(numVars);
      sparse_grid_levels_to_expansion_order(ssg_driver->trial_set(), exp_order);
      tensor_product_multi_index(exp_order, tpMultiIndex[last_index]);
      // update multiIndex and numExpansionTerms
      append_multi_index(tpMultiIndex[last_index], multiIndex, tpMultiIndexMap,
			 tpMultiIndexMapRef);
      resize_expansion();
      // form tp_data_pts, tp_wts using collocKey et al.
      SDVArray tp_data_vars; SDRArray tp_data_resp; RealVector tp_wts;
      integration_data(last_index, tp_data_vars, tp_data_resp, tp_wts);
      // form trial expansion coeffs/grads
      integrate_expansion(tpMultiIndex[last_index], tp_data_vars, tp_data_resp,
			  tp_wts, tpExpansionCoeffs[last_index],
			  tpExpansionCoeffGrads[last_index]);
      // sum trial expansion into expansionCoeffs/expansionCoeffGrads
      append_tensor_expansions(last_index);
      // cleanup
      //if (!reEntrantFlag) {
      //  ssg_driver->clear_smolyak_arrays();
      //  ssg_driver->clear_collocation_arrays();
      //  tpMultiIndex.clear(); tpMultiIndexMap.clear();
      //}
      break;
    }
    default:
      err_flag = true; break;
    }
    break;
  }
  default:
    err_flag = true; break;
  }

  if (err_flag) {
    PCerr << "Error: unsupported grid definition in OrthogPolyApproximation::"
	  << "increment_coefficients()" << std::endl;
    abort_handler(-1);
  }
}


void OrthogPolyApproximation::decrement_coefficients()
{
  switch (expConfigOptions.expCoeffsSolnApproach) {
  case SPARSE_GRID:
    SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
    switch (sparseGridExpansion) {
    case TENSOR_INT_TENSOR_SUM_EXP: {
      // reset expansion{Coeffs,CoeffGrads}: (set in append_tensor_expansions())
      expansionCoeffs     = prevExpCoeffs;
      expansionCoeffGrads = prevExpCoeffGrads;

      // reset multiIndex and numExpansionTerms:
      numExpansionTerms   = tpMultiIndexMapRef.back();
      multiIndex.resize(numExpansionTerms); // truncate previous increment
      // resize not necessary since (1) already updated from prevExp and 
      // (2) not updating expansion on decrement (next increment updates).
      //resize_expansion();

      // reset tensor-product bookkeeping and save restorable data
      savedLevMultiIndex.push_back(ssg_driver->trial_set());
      savedTPMultiIndex.push_back(tpMultiIndex.back());
      savedTPMultiIndexMap.push_back(tpMultiIndexMap.back());
      savedTPMultiIndexMapRef.push_back(numExpansionTerms);
      savedTPExpCoeffs.push_back(tpExpansionCoeffs.back());
      savedTPExpCoeffGrads.push_back(tpExpansionCoeffGrads.back());
      tpMultiIndex.pop_back();       tpMultiIndexMap.pop_back();
      tpMultiIndexMapRef.pop_back(); tpExpansionCoeffs.pop_back();
      tpExpansionCoeffGrads.pop_back();
      break;
    }
    }
    break;
  }
}


void OrthogPolyApproximation::restore_coefficients()
{
  size_t last_index;
  switch (expConfigOptions.expCoeffsSolnApproach) {
  case SPARSE_GRID:
    switch (sparseGridExpansion) {
    case TENSOR_INT_TENSOR_SUM_EXP: {
      // move previous expansion data to current expansion
      last_index = tpMultiIndex.size();
      SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
      std::deque<UShortArray>::iterator sit
	= std::find(savedLevMultiIndex.begin(), savedLevMultiIndex.end(),
		    ssg_driver->trial_set());
      size_t index_star = std::distance(savedLevMultiIndex.begin(), sit);
      savedLevMultiIndex.erase(sit);
      std::deque<UShort2DArray>::iterator iit = savedTPMultiIndex.begin();
      std::deque<SizetArray>::iterator    mit = savedTPMultiIndexMap.begin();
      std::deque<size_t>::iterator        rit = savedTPMultiIndexMapRef.begin();
      std::deque<RealVector>::iterator    cit = savedTPExpCoeffs.begin();
      std::deque<RealMatrix>::iterator    git = savedTPExpCoeffGrads.begin();
      std::advance(iit, index_star);      std::advance(mit, index_star);
      std::advance(rit, index_star);      std::advance(cit, index_star);
      std::advance(git, index_star);
      tpMultiIndex.push_back(*iit);          savedTPMultiIndex.erase(iit);
      tpMultiIndexMap.push_back(*mit);       savedTPMultiIndexMap.erase(mit);
      tpMultiIndexMapRef.push_back(*rit);    savedTPMultiIndexMapRef.erase(rit);
      tpExpansionCoeffs.push_back(*cit);     savedTPExpCoeffs.erase(cit);
      tpExpansionCoeffGrads.push_back(*git); savedTPExpCoeffGrads.erase(git);
      // update multiIndex and numExpansionTerms
      append_multi_index(tpMultiIndex[last_index], tpMultiIndexMap[last_index],
			 tpMultiIndexMapRef[last_index], multiIndex);
      resize_expansion();
      // sum trial expansion into expansionCoeffs/expansionCoeffGrads
      append_tensor_expansions(last_index);
      break;
    }
    break;
  }
  }
}


void OrthogPolyApproximation::finalize_coefficients()
{
  size_t start_index;
  switch (expConfigOptions.expCoeffsSolnApproach) {
  case SPARSE_GRID:
    switch (sparseGridExpansion) {
    case TENSOR_INT_TENSOR_SUM_EXP: {
      start_index = tpMultiIndex.size();
      // update multiIndex and numExpansionTerms
      std::deque<UShort2DArray>::iterator iit = savedTPMultiIndex.begin();
      std::deque<SizetArray>::iterator    mit = savedTPMultiIndexMap.begin();
      std::deque<size_t>::iterator        rit = savedTPMultiIndexMapRef.begin();
      for (; iit!=savedTPMultiIndex.end(); ++iit, ++mit, ++rit)
	append_multi_index(*iit, *mit, *rit, multiIndex);
      resize_expansion();
      // move previous expansion data to current expansion
      tpMultiIndex.insert(tpMultiIndex.end(),
	savedTPMultiIndex.begin(), savedTPMultiIndex.end());
      tpMultiIndexMap.insert(tpMultiIndexMap.end(),
	savedTPMultiIndexMap.begin(), savedTPMultiIndexMap.end());
      tpMultiIndexMapRef.insert(tpMultiIndexMapRef.end(),
	savedTPMultiIndexMapRef.begin(), savedTPMultiIndexMapRef.end());
      tpExpansionCoeffs.insert(tpExpansionCoeffs.end(),
	savedTPExpCoeffs.begin(), savedTPExpCoeffs.end());
      tpExpansionCoeffGrads.insert(tpExpansionCoeffGrads.end(),
	savedTPExpCoeffGrads.begin(), savedTPExpCoeffGrads.end());
      savedLevMultiIndex.clear();     savedTPMultiIndex.clear();
      savedTPMultiIndexMap.clear();   savedTPMultiIndexMapRef.clear();
      savedTPExpCoeffs.clear();       savedTPExpCoeffGrads.clear();
      // sum remaining trial expansions into expansionCoeffs/expansionCoeffGrads
      append_tensor_expansions(start_index);
      break;
    }
    break;
  }
  }
}


void OrthogPolyApproximation::store_coefficients()
{
  // Store the aggregated expansion data.  This approach is preferred to
  // appending to savedTP{MultiIndex,Coeffs,CoeffGrads} since the savedTP
  // approach is less general (TP and sum of TP only), less memory
  // efficient (tensor redundancies in sparse grids), and causes ambiguity
  // in finalize_coefficients() for generalized sparse grids.
  storedMultiIndex      = multiIndex;
  if (expConfigOptions.expansionCoeffFlag)
    storedExpCoeffs     = expansionCoeffs;
  if (expConfigOptions.expansionCoeffGradFlag)
    storedExpCoeffGrads = expansionCoeffGrads;
  switch (expConfigOptions.expCoeffsSolnApproach) { // approach-specific storage
  case SPARSE_GRID: { // sum of tensor-product expansions
    SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
    storedLevMultiIndex = ssg_driver->smolyak_multi_index();
    break;
  }
  default: // tensor-product and total-order expansions
    storedApproxOrder = approxOrder;
  }
}


void OrthogPolyApproximation::combine_coefficients(short combine_type)
{
  // based on incoming combine_type, combine the data stored previously
  // by store_coefficients()

  // storedExpCombineType used later in compute_numerical_response_moments()
  storedExpCombineType = combine_type;

  switch (storedExpCombineType) {
  case ADD_COMBINE: {

    // update multiIndex with any storedMultiIndex terms not yet included
    Sizet2DArray stored_mi_map; SizetArray stored_mi_map_ref;
    append_multi_index(storedMultiIndex, multiIndex, stored_mi_map,
		       stored_mi_map_ref);
    // resize expansion{Coeffs,CoeffGrads} based on updated multiIndex
    resize_expansion();
    // update expansion{Coeffs,CoeffGrads}
    overlay_expansion(stored_mi_map.back(), storedExpCoeffs,
		      storedExpCoeffGrads, 1);
    break;
  }
  case MULT_COMBINE: {
    // compute form of product expansion
    UShort2DArray multi_index_prod;
    switch (expConfigOptions.expCoeffsSolnApproach) {
    case QUADRATURE: // product of two tensor-product expansions
      for (size_t i=0; i<numVars; ++i)
	approxOrder[i] += storedApproxOrder[i];
      tensor_product_multi_index(approxOrder, multi_index_prod);
      break;
    case SPARSE_GRID: { // product of two sums of tensor-product expansions
      SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
      // filter out dominated Smolyak multi-indices that don't contribute
      // to the definition of the product expansion
      UShort2DArray curr_pareto, stored_pareto;
      update_pareto(ssg_driver->smolyak_multi_index(), curr_pareto);
      update_pareto(storedLevMultiIndex,             stored_pareto);
      size_t i, j, k, num_stored_mi = stored_pareto.size(),
	num_curr_mi = curr_pareto.size();
      // overlay each product expansion from the tensor-product combinations
      UShortArray exp_order_i, exp_order_j, exp_order_prod(numVars);
      UShort2DArray tp_multi_index_prod;
      for (i=0; i<num_stored_mi; ++i) {
	sparse_grid_levels_to_expansion_order(stored_pareto[i], exp_order_i);
	for (j=0; j<num_curr_mi; ++j) {
	  sparse_grid_levels_to_expansion_order(curr_pareto[j], exp_order_j);
	  for (k=0; k<numVars; ++k)
	    exp_order_prod[k] = exp_order_i[k] + exp_order_j[k];
	  tensor_product_multi_index(exp_order_prod, tp_multi_index_prod);
	  append_multi_index(tp_multi_index_prod, multi_index_prod);
	}
      }
      break;
    }
    default: // product of two total-order expansions
      for (size_t i=0; i<numVars; ++i)
	approxOrder[i] += storedApproxOrder[i];
      total_order_multi_index(approxOrder, multi_index_prod);
      break;
    }

    // perform the multiplication of current and stored expansions
    multiply_expansion(storedMultiIndex, storedExpCoeffs, storedExpCoeffGrads,
		       multi_index_prod);
    break;
  }
  case ADD_MULT_COMBINE:
    //overlay_expansion(storedMultiIndex, storedExpCoeffs,
    //                  storedExpCoeffGrads, addCoeffs, addCoeffGrads);
    //multiply_expansion(storedMultiIndex, storedExpCoeffs,
    //                   storedExpCoeffGrads, multCoeffs, multCoeffGrads);
    //compute_combine_factors(addCoeffs, multCoeffs);
    //apply_combine_factors();
    PCerr << "Error : additive+multiplicative combination not yet implemented "
	  << "in OrthogPolyApproximation::combine_coefficients()" << std::endl;
    abort_handler(-1);
    break;
  }

  /* Code below moved to compute_numerical_response_moments()
  storedMultiIndex.clear();
  if (expConfigOptions.expansionCoeffFlag)     storedExpCoeffs.resize(0);
  if (expConfigOptions.expansionCoeffGradFlag) storedExpCoeffGrads.reshape(0,0);
  */
}


void OrthogPolyApproximation::
sparse_grid_multi_index(UShort2DArray& multi_index)
{
  SparseGridDriver*    ssg_driver     = (SparseGridDriver*)driverRep;
  const UShort2DArray& sm_multi_index = ssg_driver->smolyak_multi_index();
  size_t i, num_smolyak_indices = sm_multi_index.size();

  switch (sparseGridExpansion) {
  case TENSOR_INT_TENSOR_SUM_EXP: {
    // assemble a complete list of individual polynomial coverage
    // defined from the linear combination of mixed tensor products
    multi_index.clear(); tpMultiIndexMap.clear(); tpMultiIndexMapRef.clear();
    tpMultiIndex.resize(num_smolyak_indices);
    if (expConfigOptions.refinementControl ==
	DIMENSION_ADAPTIVE_GENERALIZED_SPARSE) {
      tpExpansionCoeffs.resize(num_smolyak_indices);
      tpExpansionCoeffGrads.resize(num_smolyak_indices);
    }
    UShortArray expansion_order(numVars);
    for (i=0; i<num_smolyak_indices; ++i) {
      // regenerate i-th expansion_order as collocKey[i] cannot be used in
      // general case (i.e., for nested rules GP, CC, F2, or GK).  Rather,
      // collocKey[i] is to be used only as the key to the collocation pts.
      sparse_grid_levels_to_expansion_order(sm_multi_index[i], expansion_order);
      tensor_product_multi_index(expansion_order, tpMultiIndex[i]);
      append_multi_index(tpMultiIndex[i], multi_index, tpMultiIndexMap,
			 tpMultiIndexMapRef);
#ifdef DEBUG
      PCout << "level =\n" << sm_multi_index[i] << "quad_order =\n"
	    << quad_order << "integrand_order =\n" << integrand_order
	    << "expansion_order =\n" << expansion_order << "tp_multi_index =\n"
	    << tpMultiIndex[i] << "multi_index =\n" << multi_index << '\n';
#endif // DEBUG
    }
    break;
  }
  case SPARSE_INT_TENSOR_SUM_EXP: case SPARSE_INT_RESTR_TENSOR_SUM_EXP: {
    // assemble a complete list of individual polynomial coverage
    // defined from the linear combination of mixed tensor products
    multi_index.clear();
    UShort2DArray tp_multi_index;
    UShortArray int_order(numVars), exp_order(numVars);
    // Note: restricted rule growth within the sparse grid point set is separate
    // from restricted definition of the expansion terms.  The former makes
    // integrand precision more uniform by delaying exponential sequences, but
    // may still contain some nonuniformity.  The latter may enforce expansion
    // uniformity that would not otherwise be present based on integrand
    // precision alone, in order to reduce the possibility of a response-basis
    // product landing in the concave interior of the integrand resolution.
    short exp_growth = (sparseGridExpansion == SPARSE_INT_RESTR_TENSOR_SUM_EXP)
      ? MODERATE_RESTRICTED_GROWTH : UNRESTRICTED_GROWTH;
    for (i=0; i<num_smolyak_indices; ++i) {
      sparse_grid_levels_to_expansion_order(sm_multi_index[i], exp_order,
					    exp_growth);
      tensor_product_multi_index(exp_order, tp_multi_index);
      append_multi_index(tp_multi_index, multi_index);
#ifdef DEBUG
      PCout << "level =\n" << sm_multi_index[i] << "integrand order =\n"
	    << int_order << "expansion order =\n" << exp_order << '\n';
	  //<< "tp_multi_index =\n" << tp_multi_index
	  //<< "multi_index =\n" << multi_index << '\n';
#endif // DEBUG
    }
    break;
  }
  case SPARSE_INT_TOTAL_ORD_EXP: {
    // back out approxOrder & use total_order_multi_index()
    UShortArray quad_order(numVars), integrand_order(numVars);
    UShort2DArray pareto(1), total_pareto;
    for (i=0; i<num_smolyak_indices; ++i) {
      ssg_driver->level_to_order(sm_multi_index[i], quad_order);
      quadrature_order_to_integrand_order(quad_order, integrand_order);
      // maintain an n-dimensional Pareto front of nondominated multi-indices
      pareto[0] = integrand_order;
      update_pareto(pareto, total_pareto);
#ifdef DEBUG
      PCout << "level =\n" << sm_multi_index[i] << "\nquad_order =\n"
	    << quad_order << "\nintegrand_order =\n" << integrand_order << '\n';
#endif // DEBUG
    }
#ifdef DEBUG
    PCout << "total_pareto =\n" << total_pareto << '\n';
#endif // DEBUG

    // first pass: compute max isotropic integrand that fits within Pareto front
    unsigned short order = 0;
    integrand_order.assign(numVars, order);
    bool total_order_dominated = true;
    while (total_order_dominated) {
      // calculate all nondominated polynomials for a total-order expansion
      pareto.clear();
      total_order_multi_index(integrand_order, pareto, 0);
      total_order_dominated = assess_dominance(pareto, total_pareto);
#ifdef DEBUG
      PCout << "integrand_order =\n" << integrand_order << "pareto =\n"
	    << pareto << "total_order_dominated = " << total_order_dominated
	    << '\n';
#endif // DEBUG
      // could increment/decrement by 2's due to expansion_order conversion,
      // but the actual resolvable integrand order is typically odd.
      if (total_order_dominated)
	++order; // advance to next test level
      else
	--order; // exiting loop: rewind to last successful
      integrand_order.assign(numVars, order);
    }
#ifdef DEBUG
    PCout << "Isotropic integrand_order =\n" << integrand_order << '\n';
#endif // DEBUG
    /* Since anisotropic total-order expansions only prune corners of the Pascal
       hyper-pyramid and polynomial gaps from Smolyak (with nonlinear growth
       rules) are on the interior, the following approach is not effective.

    // second pass: if integrand order is anisotropic, assess dimensional
    // increments that may fit within Pareto front.  An important current
    // use case is PCBDO in all_variables mode.
    UShortArray sgl_to_int_order(n); // sg level -> quad order -> int order
    ssg_driver->level_to_order(ssg_driver->level(), quad_order);
    quadrature_order_to_integrand_order(quad_order, sgl_to_int_order);
    bool isotropic_integrand = true;
    order = sgl_to_int_order[0];
    for (i=1; i<n; ++i)
      if (sgl_to_int_order[i] != order)
	{ isotropic_integrand = false; break; }
    if (!isotropic_integrand) {
      bool remaining_dimensions = true;
      BoolDeque dominated(n, true);
      UShort2DArray to_multi_index;
      while (remaining_dimensions) {
	remaining_dimensions = false;
	for (i=0; i<n; ++i) {
	  if (dominated[i]) {
	    remaining_dimensions = true;
	    ++integrand_order[i];   // advance to next test level
	    // anisotropic total_order doesn't support lower bound offset,
	    // so must resort to defining the Pareto set in two steps
	    to_multi_index.clear();
	    total_order_multi_index(integrand_order, to_multi_index);//, 0);
	    pareto.clear();
	    update_pareto(to_multi_index, pareto);
	    dominated[i] = assess_dominance(pareto, total_pareto);
#ifdef DEBUG
	    PCout << "Anisotropic integrand_order =\n" << integrand_order
	        //<< "pareto =\n" << pareto
		  << "dominated[" << i << "] = " << dominated[i] << '\n';
#endif // DEBUG
	    if (!dominated[i])
	      --integrand_order[i]; // rewind to last successful
	  }
	}
      }
    }
    */

    integrand_order_to_expansion_order(integrand_order, approxOrder);
    total_order_multi_index(approxOrder, multi_index);
    break;
  }
  case SPARSE_INT_HEUR_TOTAL_ORD_EXP: // early heuristic
    heuristic_sparse_grid_level_to_expansion_order(ssg_driver->level(),
						   approxOrder);
    total_order_multi_index(approxOrder, multi_index);
    break;
  default: // possible fallback: revert to [optional] expansion_order spec.
    PCerr << "Error: unsupported sparseGridExpansion option in "
	  << "OrthogPolyApproximation::sparse_grid_multi_index()" << std::endl;
    abort_handler(-1);
    break;
  }
}


/* This approach reduces memory requirements but must perform additional
   calculation to regenerate the tp_multi_index instances (previously
   generated in sparse_grid_multi_index()).  Currently, these tp_multi_index
   instances are stored in tpMultiIndex for later use in compute_coefficients().
void OrthogPolyApproximation::
map_tensor_product_multi_index(UShort2DArray& tp_multi_index, size_t tp_index)
{
  const SizetArray& tp_mi_map = tpMultiIndexMap[tp_index];
  size_t i, num_tp_terms = tp_mi_map.size();
  tp_multi_index.resize(num_tp_terms);
  for (i=0; i<num_tp_terms; ++i)
    tp_multi_index[i] = multiIndex[tp_mi_map[i]];
}
*/


/* These mappings reflect simplified heuristics (expected to be exact
   for CC, Gauss-Patterson, and linear growth Gaussian, but not for
   exponential Gaussian). */
void OrthogPolyApproximation::
heuristic_sparse_grid_level_to_expansion_order(unsigned short ssg_level,
					       UShortArray& exp_order)
{
  if (exp_order.size() != numVars)
    exp_order.resize(numVars);
  SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
  const ShortArray& colloc_rules = ssg_driver->collocation_rules();
  const IntArray&   growth_rules = ssg_driver->api_growth_rules();
  for (size_t i=0; i<numVars; ++i) {
    switch (growth_rules[i]) {
    case FULL_EXPONENTIAL:
      switch (colloc_rules[i]) {
      case CLENSHAW_CURTIS: case FEJER2: { // TO DO: verify F2
	// integrand order = 2*level+1 ("sharp" result from Novak & Ritter,1996)
	unsigned short integrand = 2*ssg_level + 1;
	exp_order[i] = integrand / 2; // remainder truncated
	// results in exp_order = level
	break;
      }
      default:
	PCerr << "Error: unsupported integration rule for FULL_EXPONENTIAL "
	      << "growth in OrthogPolyApproximation::heuristic_sparse_grid_"
	      << "level_to_expansion_order()." << std::endl;
	abort_handler(-1);
      }
      break;
    case MODERATE_LINEAR: {
      // linear growth Gauss rules & matching moderate growth exponential rules

      // For std exponential growth: use total integrand order = m (based on
      // experimental observations in PCE_logratio.m for n=2 and levels<5).
      // This heuristic has been shown to be nonconservative for higher
      // dimensions (e.g., short column w/ n=3 and cantilever beam w/ n=4).
      //SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
      //ssg_driver->level_to_order_open_exponential(ssg_level, integrand);

      // For linear growth (quad_order = 2*level+1):
      // This relationship derived from Pareto PCE eo for n={2,4} and w={0,8}
      // and verified for n=5 and n=9.
      // > Log ratio  n = 2: For w={0,8}, eo = {0,1,3,5,7,9,11,13,15}
      // > Short col  n = 3: For w={0,8}, eo = {0,1,2,4,6,8,10,12,14}
      // > Cantilever n = 4: For w={0,8}, eo = {0,1,2,3,5,7, 9,11,13}
      // > Gen Rosen  n = 5: For w={0,6}, eo = {0,1,2,3,4,6, 8}
      // > Steel col  n = 9: For w={0,5}, eo = {0,1,2,3,4,5}
      // -> Increases by 1 up to wmNp1=0 and increases by 2 thereafter.
      short wmNp1 = ssg_level - numVars + 1;
      exp_order[i] = (wmNp1 > 0) ? wmNp1 + ssg_level : ssg_level;
      break;
    }
    default:
      PCerr << "Error: unsupported growth rule in OrthogPolyApproximation::"
	    << "heuristic_sparse_grid_level_to_expansion_order()," << std::endl;
      abort_handler(-1);
    }
  }
}


/** The optional growth_rate supports the option of forcing the
    computed integrand order to be conservative in the presence of
    exponential growth due to nested quadrature rules.  This avoids
    aggressive formulation of PCE expansion orders when an exponential
    rule takes a large jump that is not balanced by the other index
    set component mappings.  Note that restricting the expansion
    growth directly from the level (*_RESTRICTED_GROWTH cases below
    used by SPARSE_INT_RESTR_TENSOR_SUM_EXP) is similar but not
    identical to restricting the quadrature order growth from the
    level and then computing the integrand and expansion orders from
    the restricted quadrature order (default UNRESTRICTED_GROWTH case
    below used by SPARSE_INT_TENSOR_SUM_EXP and
    TENSOR_INT_TENSOR_SUM_EXP, where quadrature rule restriction
    happens elsewhere).  In particular, these approaches differ in
    granularity of control, since the former approach grows linearly
    and the latter approach selects the minimal quadrature order
    (from nonlinear growth or lookup) that meets a linear target. */
void OrthogPolyApproximation::
sparse_grid_levels_to_expansion_order(const UShortArray& levels,
				      UShortArray& exp_order, short growth_rate)
{
  size_t n = levels.size();
  UShortArray int_order(n);
  switch (growth_rate) {
  case UNRESTRICTED_GROWTH: { // used for {SPARSE,TENSOR}_INT_TENSOR_SUM_EXP
    // Best option for TENSOR_INT_TENSOR_SUM_EXP, but SPARSE_INT_TENSOR_SUM_EXP
    // is generally too aggressive for nested rules and exponential growth
    // (SPARSE_INT_RESTR_TENSOR_SUM_EXP is preferred).
    SparseGridDriver* ssg_driver = (SparseGridDriver*)driverRep;
    UShortArray quad_order(n);
    ssg_driver->level_to_order(levels, quad_order);
    quadrature_order_to_integrand_order(quad_order, int_order);
    break;
  }
  case SLOW_RESTRICTED_GROWTH: // not currently used
    for (size_t i=0; i<n; ++i) // synch with slow linear growth: i = 2l + 1
      int_order[i] =  2*levels[i] + 1;
    break;
  case MODERATE_RESTRICTED_GROWTH: // used for SPARSE_INT_RESTR_TENSOR_SUM_EXP
    // mitigate uneven integrand coverage due to exponential rule growth by
    // enforcing moderate linear expansion growth.
    for (size_t i=0; i<n; ++i) // synch with moderate linear growth: i = 4l + 1
      int_order[i] =  4*levels[i] + 1;
    break;
  }
  integrand_order_to_expansion_order(int_order, exp_order);
}


void OrthogPolyApproximation::
quadrature_order_to_integrand_order(const UShortArray& quad_order,
				    UShortArray& int_order)
{
  // Need to know exact polynomial resolution for each mixed tensor grid:
  //   Gaussian integrand resolution:        2m-1
  //   Gauss-Patterson integrand resolution: 2m-1 - previous constraints + 1
  //   Clenshaw-Curtis integrand resolution: m (odd m), m-1 (even m)

  // Burkardt monomial test logic:
  //   sparse_grid_monomial_test: resolve monomials of total degree 2*level + 1
  //   for all rules --> doesn't make sense for exponential growth rules where
  //   order grows faster for Gauss than CC (level_to_order exponential is
  //   2^{w+1}-1 for Gauss and 2^w+1 for CC) --> estimate appears valid for CC
  //   (although it does not define a crisp boundary, since some monomials above
  //   the boundary are resolved) but overly conservative for Gauss (whole
  //   orders above the boundary estimate are resolved).

  size_t i, n = quad_order.size();
  if (int_order.size() != n)
    int_order.resize(n);
  const ShortArray& colloc_rules = driverRep->collocation_rules();
  if (colloc_rules.empty()) // use basisTypes with default modes
    for (i=0; i<n; ++i)
      switch (basisTypes[i]) {
      case CHEBYSHEV_ORTHOG: // default mode is Clenshaw-Curtis
	int_order[i] = (quad_order[i] % 2) ? quad_order[i] : quad_order[i] - 1;
	break;
      default: // default mode is standard non-nested Gauss rules
	int_order[i] =  2*quad_order[i] - 1; // i = 2m - 1
	break;
      }
  else {
    const UShortArray& gk_order = driverRep->genz_keister_order();
    const UShortArray& gk_prec  = driverRep->genz_keister_precision();
    for (i=0; i<n; ++i)
      switch (colloc_rules[i]) {
      case CLENSHAW_CURTIS: case FEJER2:
	// i = m (odd m), m-1 (even m).  Note that growth rule enforces odd.
	// TO DO: verify FEJER2 same as CC
	int_order[i] = (quad_order[i] % 2) ? quad_order[i] : quad_order[i] - 1;
	break;
      case GAUSS_PATTERSON: {
	// for o(l)=2^{l+1}-1, o(l-1) = (o(l)-1)/2
	unsigned short prev_o = std::max(1,(quad_order[i] - 1)/2);
	int_order[i] = 2*quad_order[i] - prev_o;
	break;
      }
      case GENZ_KEISTER: {
	// same relationship as Gauss-Patterson, except prev_o does not follow
	// simple pattern and requires lookup
	unsigned short lev = 0, max_lev = 5;
	for (lev=0; lev<=max_lev; ++lev)
	  if (gk_order[lev] == quad_order[i])
	    { int_order[i] = gk_prec[lev]; break; }
	/*
	int lev, o, prev_o = 1, max_lev = 4, i_rule = GENZ_KEISTER,
	  g_rule = FULL_EXPONENTIAL; // map l->o directly without restriction
	for (lev=0; lev<=max_lev; ++lev) {
	  webbur::level_growth_to_order(1, &lev, &i_rule, &g_rule, &o);
	  if (o == quad_order[i])
	    { int_order[i] = 2*quad_order[i] - prev_o; break; }
	  else
	    prev_o = o;
	}
	*/
	if (lev > max_lev) {
	  PCerr << "Error: maximum GENZ_KEISTER level exceeded in OrthogPoly"
		<< "Approximation::quadrature_order_to_integrand_order()"
		<< std::endl;
	  abort_handler(-1);
	}
	break;
      }
      default: // standard non-nested Gauss rules
	int_order[i] =  2*quad_order[i] - 1; break; // i = 2m - 1
      }
  }
}


void OrthogPolyApproximation::
integrand_order_to_expansion_order(const UShortArray& int_order,
				   UShortArray& exp_order)
{
  // reserve half of the integrand order for the expansion and half for the
  // response function (integrand = 2p)
  size_t i, n = int_order.size();
  if (exp_order.size() != n)
    exp_order.resize(n);
  for (i=0; i<n; ++i)
    exp_order[i] = int_order[i] / 2; // remainder truncated
}


/** Append to multi_index based on app_multi_index. */
void OrthogPolyApproximation::
append_multi_index(const UShort2DArray& app_multi_index,
		   UShort2DArray& multi_index)
{
  if (multi_index.empty())
    multi_index = app_multi_index;
  else {
    size_t i, num_app_mi = app_multi_index.size();
    for (i=0; i<num_app_mi; ++i) {
      const UShortArray& search_mi = app_multi_index[i];
      // TO DO: make this process more efficient
      if (std::find(multi_index.begin(), multi_index.end(), search_mi) ==
	  multi_index.end()) // search_mi does not yet exist in multi_index
	multi_index.push_back(search_mi);
    }
  }
}


/** Append to multi_index, multi_index_map, and multi_index_map_ref
    based on app_multi_index. */
void OrthogPolyApproximation::
append_multi_index(const UShort2DArray& app_multi_index,
		   UShort2DArray& multi_index, Sizet2DArray& multi_index_map,
		   SizetArray& multi_index_map_ref)
{
  size_t i, num_app_mi = app_multi_index.size();
  if (multi_index.empty()) {
    multi_index = app_multi_index;
    multi_index_map_ref.push_back(0);
    multi_index_map.resize(1); multi_index_map[0].resize(num_app_mi);
    for (i=0; i<num_app_mi; ++i)
      multi_index_map[0][i] = i;
  }
  else {
    multi_index_map_ref.push_back(multi_index.size());
    SizetArray app_mi_map(num_app_mi);
    for (i=0; i<num_app_mi; ++i) {
      const UShortArray& search_mi = app_multi_index[i];
      // TO DO: make this process more efficient
      size_t index = find_index(multi_index, search_mi);
      if (index == _NPOS) { // search_mi does not yet exist in multi_index
	app_mi_map[i] = multi_index.size();
	multi_index.push_back(search_mi);
      }
      else
	app_mi_map[i] = index;
    }
    multi_index_map.push_back(app_mi_map);
  }
}


/** Append to multi_index based on app_multi_index and previously
    defined multi_index_map and multi_index_map_ref.  If necessary,
    update multi_index_map and multi_index_map_ref. */
void OrthogPolyApproximation::
append_multi_index(const UShort2DArray& app_multi_index,
		   SizetArray& multi_index_map, size_t& multi_index_map_ref,
		   UShort2DArray& multi_index)
{
  if (multi_index.empty())
    multi_index = app_multi_index;
  else {
    size_t i, num_app_mi = app_multi_index.size(), num_mi = multi_index.size();
    if (num_mi == multi_index_map_ref) { // current mi corresponds to ref
      for (i=0; i<num_app_mi; ++i)
	if (multi_index_map[i] >= multi_index_map_ref)
	  multi_index.push_back(app_multi_index[i]);
    }
    else if (num_mi > multi_index_map_ref) { // mi has grown since ref taken
      for (i=0; i<num_app_mi; ++i)
	if (multi_index_map[i] >= multi_index_map_ref) { // previously appended
	  const UShortArray& search_mi = app_multi_index[i];
	  // search from reference pt forward
	  UShort2DArray::iterator it, it_start = multi_index.begin();
	  std::advance(it_start, multi_index_map_ref);
	  it = std::find(it_start, multi_index.end(), search_mi);
	  if (it == multi_index.end()) { // still an append: update map, append
	    multi_index_map[i] = multi_index.size();
	    multi_index.push_back(app_multi_index[i]);
	  }
	  else // no longer an append: only update map
	    multi_index_map[i] = multi_index_map_ref
	                       + std::distance(it_start, it);
	}
      multi_index_map_ref = num_mi; // reference point now updated
    }
    else { // multi_index is not allowed to shrink since ref taken
      PCerr << "Error: multi_index inconsistent with reference size in "
	    << "OrthogPolyApproximation::append_multi_index()." << std::endl;
      abort_handler(-1);
    }
  }
}


void OrthogPolyApproximation::
overlay_expansion(const SizetArray& multi_index_map,
		  const RealVector& exp_coeffs, const RealMatrix& exp_grads,
		  int coeff)
{
  size_t i, j, index, num_terms = multi_index_map.size(), 
    num_deriv_vars = expansionCoeffGrads.numRows();
  for (i=0; i<num_terms; ++i) {
    index = multi_index_map[i];
    if (expConfigOptions.expansionCoeffFlag)
      expansionCoeffs[index] += coeff * exp_coeffs[i];
    if (expConfigOptions.expansionCoeffGradFlag) {
      Real*       exp_grad_ndx = expansionCoeffGrads[index];
      const Real* grad_i       = exp_grads[i];
      for (j=0; j<num_deriv_vars; ++j)
	exp_grad_ndx[j] += coeff * grad_i[j];
    }
  }
}


void OrthogPolyApproximation::
multiply_expansion(const UShort2DArray& multi_index_b,
		   const RealVector&    exp_coeffs_b,
		   const RealMatrix&    exp_grads_b,
		   const UShort2DArray& multi_index_c)
{
  UShort2DArray multi_index_a = multiIndex;  // copy (both expConfigOptions)
  RealVector exp_coeffs_a = expansionCoeffs; // copy (both expConfigOptions)
  RealMatrix exp_grads_a;
  if (expConfigOptions.expansionCoeffGradFlag)
    exp_grads_a = expansionCoeffGrads;       // copy (CoeffGrads only)
  size_t i, j, k, v, num_a = multi_index_a.size(), num_b = multi_index_b.size(),
    num_c = multi_index_c.size(), num_deriv_vars = exp_grads_a.numRows();

  // precompute 1D basis triple products required
  unsigned short max_a, max_b, max_c; UShortMultiSet max_abc;
  OrthogonalPolynomial* poly_rep_v;
  for (v=0; v<numVars; ++v) {
    max_a = max_b = max_c = 0; max_abc.clear();
    // could track max_abc within combine_coefficients() and pass in, but would
    // need max orders for each dimension for both factors plus their product.
    // Since this would be awkward and only marginally more efficient, just
    // compute them here from the available multi-index arrays.
    for (i=0; i<num_a; ++i)
      if (multi_index_a[i][v] > max_a)
	max_a = multi_index_a[i][v];
    for (i=0; i<num_b; ++i)
      if (multi_index_b[i][v] > max_b)
	max_b = multi_index_b[i][v];
    for (i=0; i<num_c; ++i)
      if (multi_index_c[i][v] > max_c)
	max_c = multi_index_c[i][v];
    max_abc.insert(max_a); max_abc.insert(max_b); max_abc.insert(max_c); 
    poly_rep_v = (OrthogonalPolynomial*)polynomialBasis[v].polynomial_rep();
    poly_rep_v->precompute_triple_products(max_abc);
  }

  // For c = a * b, compute coefficient of product expansion as:
  // \Sum_k c_k \Psi_k = \Sum_i \Sum_j a_i b_j \Psi_i \Psi_j
  //    c_k <\Psi_k^2> = \Sum_i \Sum_j a_i b_j <\Psi_i \Psi_j \Psi_k>
  if (expConfigOptions.expansionCoeffFlag)
    expansionCoeffs.size(num_c);                      // init to 0
  if (expConfigOptions.expansionCoeffGradFlag)
    expansionCoeffGrads.shape(num_deriv_vars, num_c); // init to 0
  Real trip_prod, trip_prod_v, norm_sq_k; bool non_zero;
  for (k=0; k<num_c; ++k) {
    for (i=0; i<num_a; ++i) {
      for (j=0; j<num_b; ++j) {
	trip_prod = 1.;
	for (v=0; v<numVars; ++v) {
	  poly_rep_v=(OrthogonalPolynomial*)polynomialBasis[v].polynomial_rep();
	  non_zero = poly_rep_v->triple_product(multi_index_a[i][v],
	    multi_index_b[j][v], multi_index_c[k][v], trip_prod_v);
	  if (non_zero) trip_prod *= trip_prod_v;
	  else          break;
	}
	if (non_zero) {
	  if (expConfigOptions.expansionCoeffFlag)
	    expansionCoeffs[k] += exp_coeffs_a[i] * exp_coeffs_b[j] * trip_prod;
	  if (expConfigOptions.expansionCoeffGradFlag)
	    for (v=0; v<num_deriv_vars; ++v)
	      expansionCoeffGrads(v,k) += (exp_coeffs_a[i] * exp_grads_b(v,j)
		+ exp_coeffs_b[j] * exp_grads_a(v,i)) * trip_prod;
	}
      }
    }
    norm_sq_k = norm_squared(multi_index_c[k]);
    if (expConfigOptions.expansionCoeffFlag)
      expansionCoeffs[k] /= norm_sq_k;
    if (expConfigOptions.expansionCoeffGradFlag)
      for (v=0; v<num_deriv_vars; ++v)
	expansionCoeffGrads(v,k) /= norm_sq_k;
  }
  multiIndex = multi_index_c; numExpansionTerms = multiIndex.size();
}


void OrthogPolyApproximation::append_tensor_expansions(size_t start_index)
{
  // for use in decrement_coefficients()
  prevExpCoeffs = expansionCoeffs; prevExpCoeffGrads = expansionCoeffGrads;

  // update expansion{Coeffs,CoeffGrads} using a hierarchical update
  // rather than building from scratch
  SparseGridDriver*  ssg_driver = (SparseGridDriver*)driverRep;
  const IntArray&     sm_coeffs = ssg_driver->smolyak_coefficients();
  const IntArray& sm_coeffs_ref = ssg_driver->smolyak_coefficients_reference();
#ifdef DEBUG
  PCout << "In OrthogPolyApproximation::append_tensor_expansions() with "
	<< "start index " << start_index << "\nsm_coeffs:\n" << sm_coeffs
	<< "sm_coeffs_ref:\n" << sm_coeffs_ref << std::endl;
#endif // DEBUG

  // add trial expansions
  size_t index, num_tensor_grids = sm_coeffs.size();
  int coeff, delta_coeff;
  for (index=start_index; index<num_tensor_grids; ++index) {
    coeff = sm_coeffs[index];
    if (coeff)
      overlay_expansion(tpMultiIndexMap[index], tpExpansionCoeffs[index],
			tpExpansionCoeffGrads[index], coeff);
#ifdef DEBUG
    PCout << "Trial set sm_coeff = " << coeff << "\ntpExpansionCoeffs:\n";
    write_data(PCout, tpExpansionCoeffs[index]);
    PCout << "\ntpMultiIndexMap:\n" << tpMultiIndexMap[index] << '\n';
#endif // DEBUG
  }
  // update other expansion contributions with a changed smolyak coefficient
  for (index=0; index<start_index; ++index) {
    // add new, subtract previous
    delta_coeff = sm_coeffs[index] - sm_coeffs_ref[index];
#ifdef DEBUG
    PCout << "Old set delta_coeff = " << delta_coeff
	  << "\ntpExpansionCoeffs:\n";
    write_data(PCout, tpExpansionCoeffs[index]);
    PCout << "\ntpMultiIndexMap:\n" << tpMultiIndexMap[index] << '\n';
#endif // DEBUG
    if (delta_coeff)
      overlay_expansion(tpMultiIndexMap[index], tpExpansionCoeffs[index],
			tpExpansionCoeffGrads[index], delta_coeff);
  }
}


void OrthogPolyApproximation::
update_pareto(const UShort2DArray& new_pareto, UShort2DArray& total_pareto)
{
  //if (total_pareto.empty())
  //  total_pareto = new_pareto;
  //else {
    size_t i, num_new_p = new_pareto.size(),
      num_total_p = total_pareto.size();
    std::list<UShort2DArray::iterator> rm_iters; // sorted, unique
    UShort2DArray::iterator jit;
    for (i=0; i<num_new_p; ++i) {
      const UShortArray& new_i = new_pareto[i];
      bool new_i_dominated = false;
      for (jit=total_pareto.begin(); jit!=total_pareto.end(); ++jit) {
	bool new_i_dominated_by_j, total_j_dominated;
	assess_dominance(new_i, *jit, new_i_dominated_by_j, total_j_dominated);
	if (new_i_dominated_by_j)
	  { new_i_dominated = true; break; }
	if (total_j_dominated)
	  rm_iters.push_back(jit);
      }
      // 
      // prune newly dominated in reverse order (vector iterators
      // following a point of insertion or deletion are invalidated)
      while (!rm_iters.empty())
	{ total_pareto.erase(rm_iters.back()); rm_iters.pop_back(); }
      // add nondominated
      if (!new_i_dominated)
	total_pareto.push_back(new_i);
    }
  //}
}


bool OrthogPolyApproximation::
assess_dominance(const UShort2DArray& new_pareto,
		 const UShort2DArray& total_pareto)
{
  bool new_dominated = true;
  size_t i, j, num_new_p = new_pareto.size(),
    num_total_p = total_pareto.size();
  for (i=0; i<num_new_p; ++i) {
    const UShortArray& new_i = new_pareto[i];
    bool new_i_dominated = false;
    for (j=0; j<num_total_p; ++j) {
      bool i_dominated_by_j, j_dominated;
      assess_dominance(new_i, total_pareto[j], i_dominated_by_j, j_dominated);
      if (i_dominated_by_j)
	{ new_i_dominated = true; break; }
    }
    if (!new_i_dominated) {
      new_dominated = false;
#ifdef DEBUG
      PCout << "Nondominated new pareto member =\n" << new_i;
#else
      break;
#endif // DEBUG
    }
  }
  return new_dominated;
}


void OrthogPolyApproximation::
assess_dominance(const UShortArray& new_order,
		 const UShortArray& existing_order,
		 bool& new_dominated, bool& existing_dominated)
{
  // can't use std::vector::operator< (used for component-wise sorting)
  size_t i, n = new_order.size();
  bool equal = true, existing_dominated_temp = true;
  new_dominated = true;
  for (i=0; i<n; ++i)
    if (new_order[i] > existing_order[i])
      { equal = false; new_dominated = false; }
    else if (existing_order[i] > new_order[i])
      { equal = false; existing_dominated_temp = false; }
  // asymmetric logic since incumbent wins a tie
  existing_dominated = (!equal && existing_dominated_temp);
}


void OrthogPolyApproximation::integration_checks()
{
  if (surrData.anchor()) {
    PCerr << "Error: anchor point not supported for numerical integration in "
	  << "OrthogPolyApproximation::integration()." << std::endl;
    abort_handler(-1);
  }
  if (!driverRep) {
    PCerr << "Error: pointer to integration driver required in "
	  << "OrthogPolyApproximation::compute_coefficients()." << std::endl;
    abort_handler(-1);
  }
  size_t num_data_pts = surrData.size();
  if (num_data_pts != driverRep->type1_weight_sets().length()) {
    PCerr << "Error: number of current points (" << num_data_pts << ") is "
	  << "not consistent with\n       number of points/weights ("
	  << driverRep->type1_weight_sets().length() << ") from integration "
	  << "driver in\n       OrthogPolyApproximation::compute_coefficients"
	  << "()." << std::endl;
    abort_handler(-1);
  }
}


void OrthogPolyApproximation::
integration_data(size_t tp_index, SDVArray& tp_data_vars,
		 SDRArray& tp_data_resp, RealVector& tp_weights)
{
  // extract tensor vars/resp from surrData and tensor weights from collocWts1D
  SparseGridDriver*   ssg_driver = (SparseGridDriver*)driverRep;
  const UShortArray&    sm_index = ssg_driver->smolyak_multi_index()[tp_index];
  const UShort2DArray&       key = ssg_driver->collocation_key()[tp_index];
  const SizetArray& colloc_index = ssg_driver->collocation_indices()[tp_index];
  const Real3DArray& colloc_wts_1d
    = ssg_driver->type1_collocation_weights_array();
  const SDVArray& data_vars = surrData.variables_data();
  const SDRArray& data_resp = surrData.response_data();
  size_t i, j, index, num_tp_pts = colloc_index.size();
  tp_data_vars.resize(num_tp_pts); tp_data_resp.resize(num_tp_pts);
  tp_weights.resize(num_tp_pts);
  for (i=0; i<num_tp_pts; ++i) {
    // tensor-product vars/resp
    index = colloc_index[i];
    tp_data_vars[i] = data_vars[index];
    tp_data_resp[i] = data_resp[index];
    // tensor-product weight
    Real& tp_wts_i = tp_weights[i]; tp_wts_i = 1.;
    const UShortArray& key_i = key[i];
    for (j=0; j<numVars; ++j)
      tp_wts_i *= colloc_wts_1d[sm_index[j]][j][key_i[j]];
  }
}


/** The coefficients of the PCE for the response are calculated using a
    spectral projection of the response against each multivariate orthogonal
    polynomial basis fn using the inner product ratio <f,Psi>/<Psi^2>, where
    inner product <a,b> is the n-dimensional integral of a*b*weighting over
    the support range of the n-dimensional (composite) weighting function.
    1-D quadrature rules are defined for specific 1-D weighting functions
    and support ranges and approximate the integral of f*weighting as the
    Sum_i of w_i f_i.  To extend this to n-dimensions, a tensor product
    quadrature rule, cubature, or Smolyak sparse grid rule is applied.  
    It is not necessary to approximate the integral for the denominator
    numerically, since this is available analytically. */
void OrthogPolyApproximation::
integrate_expansion(const UShort2DArray& multi_index,
		    const SDVArray& data_vars, const SDRArray& data_resp,
		    const RealVector& wt_sets, RealVector& exp_coeffs,
		    RealMatrix& exp_coeff_grads)
{
  // Perform numerical integration via tensor-product quadrature/cubature/
  // Smolyak sparse grids.  Quadrature/cubature use a single application of
  // point and weight sets computed by TensorProductDriver/CubatureDriver, and
  // sparse grids could do this as well, but it is better to integrate the
  // sparse grid on a per-tensor-product basis folowed by summing the
  // corresponding PC expansions.
  size_t i, j, k, num_exp_terms = multi_index.size(),
    num_pts = std::min(data_vars.size(), data_resp.size()),
    num_deriv_vars = data_resp[0].response_gradient().length();
  Real wt_resp_fn_i, Psi_ij; Real* exp_grad;
  RealVector wt_resp_grad_i;
  if (expConfigOptions.expansionCoeffFlag) { // shape if needed and zero out
    if (exp_coeffs.length() != num_exp_terms)
      exp_coeffs.size(num_exp_terms); // init to 0
    else
      exp_coeffs = 0.;
  }
  if (expConfigOptions.expansionCoeffGradFlag) {
    if (exp_coeff_grads.numRows() != num_deriv_vars ||
	exp_coeff_grads.numCols() != num_exp_terms)
      exp_coeff_grads.shape(num_deriv_vars, num_exp_terms); // init to 0
    else
      exp_coeff_grads = 0.;
    wt_resp_grad_i.sizeUninitialized(num_deriv_vars);
  }
  for (i=0; i<num_pts; ++i) {
    if (expConfigOptions.expansionCoeffFlag)
      wt_resp_fn_i = wt_sets[i] * data_resp[i].response_function();
    if (expConfigOptions.expansionCoeffGradFlag) {
      wt_resp_grad_i = data_resp[i].response_gradient(); // copy
      wt_resp_grad_i.scale(wt_sets[i]);
    }
    const RealVector& c_vars_i = data_vars[i].continuous_variables();
    for (j=0; j<num_exp_terms; ++j) {
      Psi_ij = multivariate_polynomial(c_vars_i, multi_index[j]);
      if (expConfigOptions.expansionCoeffFlag)
	exp_coeffs[j] += Psi_ij * wt_resp_fn_i;
      if (expConfigOptions.expansionCoeffGradFlag) {
	exp_grad = exp_coeff_grads[j];
	for (k=0; k<num_deriv_vars; ++k)
	  exp_grad[k] += Psi_ij * wt_resp_grad_i[k];
      }
    }
  }

  for (i=0; i<num_exp_terms; ++i) {
    Real norm_sq = norm_squared(multi_index[i]);
    if (expConfigOptions.expansionCoeffFlag)
      exp_coeffs[i] /= norm_sq;
    if (expConfigOptions.expansionCoeffGradFlag) {
      exp_grad = exp_coeff_grads[i];
      for (k=0; k<num_deriv_vars; ++k)
	exp_grad[k] /= norm_sq;
    }
  }
#ifdef DEBUG
  PCout << "expansion_coeffs:\n" << exp_coeffs << "expansion_coeff_grads:\n"
	<< exp_coeff_grads<< "\n\n";
#endif // DEBUG
}


/** In this case, regression is used in place of spectral projection.  That
    is, instead of calculating the PCE coefficients using inner products, 
    linear least squares is used to estimate the PCE coefficients which
    best match a set of response samples.  The least squares estimation is
    performed using DGELSS (SVD) or DGGLSE (equality-constrained) from
    LAPACK, based on anchor point and derivative data availability. */
void OrthogPolyApproximation::regression()
{
  // compute order of data contained within surrData
  short data_order = 1;
  if (surrData.anchor()) {
    if (!surrData.anchor_gradient().empty())    data_order |= 2;
    if (!surrData.anchor_hessian().empty())     data_order |= 4;
  }
  else {
    if (!surrData.response_gradient(0).empty()) data_order |= 2;
    if (!surrData.response_hessian(0).empty())  data_order |= 4;
  }
  // verify support for basisConfigOptions.useDerivs, which indicates usage of
  // derivative data with respect to expansion variables (aleatory or combined)
  // within the expansion coefficient solution process, which must be
  // distinguished from usage of derivative data with respect to non-expansion
  // variables (the expansionCoeffGradFlag case).
  if (basisConfigOptions.useDerivs) {
    if (!(data_order & 2)) {
      PCerr << "Error: useDerivs configuration option lacks data support in "
	    << "OrthogPolyApproximation::regression()" << std::endl;
      abort_handler(-1);
    }
    if (expConfigOptions.expansionCoeffGradFlag) {
      PCerr << "Error: useDerivs configuration option conflicts with gradient "
	    << "expansion request in OrthogPolyApproximation::regression()"
	    << std::endl;
      abort_handler(-1);
    }
    if (data_order & 4)
      PCerr << "Warning: useDerivs configuration option does not yet support "
	    << "Hessian data in OrthogPolyApproximation::regression()"
	    << std::endl;
  }
  // compute data counts
  size_t i, j, k, num_surr_data_pts = surrData.size(),
    num_failed_surr_fn = 0, num_failed_surr_grad = 0;
  SizetShortMap::const_iterator fit; bool faults_differ = false;
  short                failed_anchor_data = surrData.failed_anchor_data();
  const SizetShortMap& failed_resp_data   = surrData.failed_response_data();
  for (fit=failed_resp_data.begin(); fit!=failed_resp_data.end(); ++fit) {
    short fail_asv = fit->second;
    if (fail_asv & 1) ++num_failed_surr_fn;
    if (fail_asv & 2) ++num_failed_surr_grad;
    if ( (fail_asv & 3) != 3 ) faults_differ = true; // Psi matrices will differ
  }
  size_t anchor_eqns  = 0,
    num_data_pts_fn   = num_surr_data_pts - num_failed_surr_fn,
    num_data_pts_grad = num_surr_data_pts - num_failed_surr_grad;
  bool anchor_fn = false, anchor_grad = false, anchor_pt = false,
    add_val, add_grad;
  if (surrData.anchor()) {
    if (!(failed_anchor_data & 1))
      { anchor_fn   = anchor_pt = true; anchor_eqns += 1; }
    if ((data_order & 2) && !(failed_anchor_data & 2))
      { anchor_grad = anchor_pt = true; anchor_eqns += numVars; }
  }

  // TO DO: sanity check for different cases (expansion{Coeff,CoeffGrad}Flag)
  // to ensure sufficient data following fault omissions
  //if ( < numExpansionTerms) {
  //  PCerr << "Error: " << std::endl;
  //  abort_handler(-1);
  //}

  bool fn_constrained_lls = (basisConfigOptions.useDerivs &&
    num_data_pts_fn + anchor_eqns < numExpansionTerms), lapack_err = false;
  Teuchos::LAPACK<int, Real> la;
  double *A_matrix, *work;
  int num_cols_A = numExpansionTerms, info = 0; // GELSS/GGLSE output flag

  if (anchor_pt || fn_constrained_lls) {
    // Use DGGLSE for equality-constrained LLS soln using GRQ factorization.
    // Solves min ||b - Ax||_2 s.t. Cx = d (Note: b,C switched from LAPACK docs)
    // where {b,d} are single vectors (multiple RHS not supported).

    // baseline counts to be augmented:
    int num_cons   = anchor_eqns,      // number of equality constraints
        num_rows_A = (basisConfigOptions.useDerivs) ?
        num_data_pts_grad*numVars : 0; // number of rows in matrix A
    // anchor_pt and fn_constrained_lls differ in num_data_pts_fn assignment:
    if (fn_constrained_lls) // constraints: values + anchor data, LLS: grads
      num_cons   += num_data_pts_fn;
    else                    // constraints: anchor data, LLS: values + grads
      num_rows_A += num_data_pts_fn;

    // Matrix of polynomial terms in a contiguous block of memory packed in
    // column-major ordering as required by F77 LAPACK subroutines.
    A_matrix         = new double [num_rows_A*num_cols_A]; // "A" in A*x = b
    // Vector of response values that correspond to the samples in matrix A.
    double* b_vector = new double [num_rows_A]; // "b" in A*x = b
    // Solution vector
    double* x_vector = new double [num_cols_A]; // "x" in b - A*x, C*x = d

    if (expConfigOptions.expansionCoeffFlag) {// anchor_pt or fn_constrained_lls

      // Matrix of constraints unrolled into a vector
      double* C_matrix = new double [num_cons*num_cols_A]; // "C" in C*x = d
      // RHS of constraints
      double* d_vector = new double [num_cons];   // "d" in C*x = d

      // Get the optimal work array size
      int lwork        = -1; // special code for workspace query
      work             = new double [1]; // temporary work array
      la.GGLSE(num_rows_A, num_cols_A, num_cons, A_matrix, num_rows_A, C_matrix,
	       num_cons, b_vector, d_vector, x_vector, work, lwork, &info);
      lwork = (int)work[0]; // optimal work array size returned by query
      delete [] work;
      work             = new double [lwork]; // Optimal work array

      size_t a_cntr = 0, b_cntr = 0, c_cntr = 0, d_cntr = 0;
      for (i=0; i<numExpansionTerms; ++i) {
	const UShortArray& mi = multiIndex[i];
	// pack surrData::anchorResp values/gradients in C,d
	pack_polynomial_data(surrData.anchor_continuous_variables(), mi,
			     anchor_fn, C_matrix, c_cntr, anchor_grad,
			     C_matrix, c_cntr);
	// pack constraints in C,d and LLS data in A,b.
	for (j=0, fit=failed_resp_data.begin(); j<num_surr_data_pts; ++j) {
	  add_val = true; add_grad = basisConfigOptions.useDerivs;
	  fail_booleans(fit, j, add_val, add_grad);
	  if (fn_constrained_lls) // values are constraints, grads are LLS data
	    pack_polynomial_data(surrData.continuous_variables(j), mi, add_val,
				 C_matrix, c_cntr, add_grad, A_matrix, a_cntr);
	  else                    // values + grads are LLS data
	    pack_polynomial_data(surrData.continuous_variables(j), mi, add_val,
				 A_matrix, a_cntr, add_grad, A_matrix, a_cntr);
	}
      }
      // pack surrData::anchorResp values/gradients in C,d.
      pack_response_data(surrData.anchor_response(), anchor_fn, d_vector,
			 d_cntr, anchor_grad, d_vector, d_cntr);
      // pack constraints in C,d and LLS data in A,b.
      const SDRArray& sdr_array = surrData.response_data();
      for (j=0, fit=failed_resp_data.begin(); j<num_surr_data_pts; ++j) {
	add_val = true; add_grad = basisConfigOptions.useDerivs;
	fail_booleans(fit, j, add_val, add_grad);
	if (fn_constrained_lls) // values are constraints, grads are LLS data
	  pack_response_data(sdr_array[j], add_val, d_vector, d_cntr,
			     add_grad, b_vector, b_cntr);
	else                    // values + grads are LLS data
	  pack_response_data(sdr_array[i], add_val, b_vector, b_cntr,
			     add_grad, b_vector, b_cntr);
      }
#ifdef DEBUG
      RealMatrix A2(Teuchos::View, A_matrix, num_rows_A, num_rows_A,num_cols_A),
	         C2(Teuchos::View, C_matrix, num_cons,   num_cons,  num_cols_A);
      RealVector b2(Teuchos::View, b_vector, num_rows_A),
	         d2(Teuchos::View, d_vector, num_cons);
      PCout << "A_matrix:\n"; write_data(PCout, A2, false, true, true);
      PCout << "C_matrix:\n"; write_data(PCout, C2, false, true, true);
      PCout << "b_vector:\n"; write_data(PCout, b2);
      PCout << "d_vector:\n"; write_data(PCout, d2);
#endif // DEBUG

      // Least squares computation using LAPACK's DGGLSE subroutine which uses
      // a GRQ factorization method for solving the eq-constrained LLS problem
      info = 0;
      la.GGLSE(num_rows_A, num_cols_A, num_cons, A_matrix, num_rows_A,
	       C_matrix, num_cons, b_vector, d_vector, x_vector, work,
	       lwork, &info);
      if (info)
	lapack_err = true;
      copy_data(x_vector, numExpansionTerms, expansionCoeffs);

      delete [] C_matrix;
      delete [] d_vector;
    }

    if (expConfigOptions.expansionCoeffGradFlag) {
      // this supports only the anchor_pt option, since expansionCoeffGradFlag
      // and useDerivs in fn_constrained_lls are mutually exclusive

      if (faults_differ) {
	delete [] A_matrix;
	delete [] b_vector;
	num_rows_A       = num_data_pts_grad;
	A_matrix         = new double [num_rows_A*num_cols_A];
	double* b_vector = new double [num_rows_A];
      }
      num_cons = (anchor_grad) ? 1 : 0;
      double* C_matrix = new double [num_cons*num_cols_A]; // "C" in C*x = d
      double* d_vector = new double [num_cons];            // "d" in C*x = d

      // Get the optimal work array size
      int lwork = -1; // special code for workspace query
      if (expConfigOptions.expansionCoeffFlag)
	delete [] work;
      work      = new double [1]; // temporary work array
      la.GGLSE(num_rows_A, num_cols_A, num_cons, A_matrix, num_rows_A, C_matrix,
	       num_cons, b_vector, d_vector, x_vector, work, lwork, &info);
      lwork     = (int)work[0]; // optimal work array size returned by query
      delete [] work;
      work      = new double [lwork]; // Optimal work array

      size_t num_deriv_vars = expansionCoeffGrads.numRows();
      for (i=0; i<num_deriv_vars; ++i) {
	// must be recomputed each time since DGGLSE solves in place
	size_t a_cntr = 0, b_cntr = 0;
	for (j=0; j<numExpansionTerms; ++j) {
	  const UShortArray& mi = multiIndex[j];
	  for (k=0, fit=failed_resp_data.begin(); k<num_surr_data_pts; ++k) {
	    add_val = false; add_grad = true;
	    fail_booleans(fit, k, add_val, add_grad);
	    if (add_grad) { // doesn't fit pack_polynomial_data() model
	      A_matrix[a_cntr] = multivariate_polynomial(
		surrData.continuous_variables(k), mi);
	      ++a_cntr;
	    }
	  }
	  if (anchor_grad) // doesn't fit pack_polynomial_data() model
	    C_matrix[j] = multivariate_polynomial(
	      surrData.anchor_continuous_variables(), mi);
	}
	// the Ax=b RHS is the i-th grad components from surrData::respData
	for (j=0, fit=failed_resp_data.begin(); j<num_surr_data_pts; ++j) {
	  add_val = false; add_grad = true;
	  fail_booleans(fit, j, add_val, add_grad);
	  if (add_grad)
	    { b_vector[b_cntr] = surrData.response_gradient(j)[i]; ++b_cntr; }
	}
	// the Cx=d RHS is the i-th anchor gradient component
	if (anchor_grad)
	  d_vector[0] = surrData.anchor_gradient()[i];
	// solve for the PCE coefficients for the i-th gradient component
	info = 0;
	la.GGLSE(num_rows_A, num_cols_A, num_cons, A_matrix, num_rows_A,
		 C_matrix, num_cons, b_vector, d_vector, x_vector, work,
		 lwork, &info);
	if (info)
	  lapack_err = true;
	for (j=0; j<numExpansionTerms; ++j)
	  expansionCoeffGrads(i,j) = x_vector[j];
      }
      delete [] C_matrix;
      delete [] d_vector;
    }
    delete [] b_vector;
    delete [] x_vector;
  }
  else if (expConfigOptions.expansionCoeffGradFlag) {
    // Use DGELSS for LLS soln using SVD.  Solves min ||b - Ax||_2
    // where {b} may have multiple RHS -> multiple {x} solutions.
    if (faults_differ) { // *** TO DO ***
      PCerr << "Error: SVD solution with multiple RHS does not yet support "
	   << "inconsistency in value/gradient fault tolerance." << std::endl;
      abort_handler(-1);
    }

    int    num_rows_A    = num_data_pts_fn;
    size_t num_grad_rhs  = expansionCoeffGrads.numRows();
    size_t num_coeff_rhs = (expConfigOptions.expansionCoeffFlag) ? 1 : 0;
    int    num_rhs = num_coeff_rhs + num_grad_rhs; // input: # of RHS vectors
    double rcond = -1.; // input:  use macheps to rank singular vals of A
    int    rank  =  0;  // output: effective rank of matrix A
    // Matrix of polynomial terms unrolled into a vector.
    A_matrix          = new double [num_rows_A*num_cols_A]; // "A" in A*x = b
    // input: vectors of response data that correspond to samples in matrix A.
    double* b_vectors = new double [num_rows_A*num_rhs]; // "b" in A*x = b
    // output: vector of singular values, dimensioned for overdetermined system
    double* s_vector  = new double [num_cols_A];

    // Get the optimal work array size
    int lwork         = -1; // special code for workspace query
    work              = new double [1]; // temporary work array
    la.GELSS(num_rows_A, num_cols_A, num_rhs, A_matrix, num_rows_A, b_vectors,
	     num_rows_A, s_vector, rcond, &rank, work, lwork, &info);
    lwork = (int)work[0]; // optimal work array size returned by query
    delete [] work;
    work              = new double [lwork]; // Optimal work array

    // The "A" matrix is a contiguous block of memory packed in column-major
    // ordering as required by F77 for the GELSS subroutine from LAPACK.  For
    // example, the 6 elements of A(2,3) are stored in the order A(1,1), A(2,1),
    // A(1,2), A(2,2), A(1,3), A(2,3).
    size_t a_cntr = 0, b_cntr = 0;
    for (i=0; i<numExpansionTerms; ++i) {
      const UShortArray& mi = multiIndex[i];
      for (j=0, fit=failed_resp_data.begin(); j<num_surr_data_pts; ++j) {
	add_val = true; add_grad = false; // no gradients in A
	fail_booleans(fit, j, add_val, add_grad);
	pack_polynomial_data(surrData.continuous_variables(j), mi, add_val,
			     A_matrix, a_cntr, add_grad, A_matrix, a_cntr);
      }
    }

    // response data (values/gradients) define the multiple RHS which are
    // matched in the LS soln.  b_vectors is num_data_pts (rows) x num_rhs
    // (cols), arranged in column-major order.
    for (i=0, fit=failed_resp_data.begin(); i<num_surr_data_pts; ++i) {
      add_val  = expConfigOptions.expansionCoeffFlag;
      add_grad = expConfigOptions.expansionCoeffGradFlag;
      fail_booleans(fit, i, add_val, add_grad);
      if (add_val)
	{ b_vectors[b_cntr] = surrData.response_function(i); }
      if (add_grad) {
	const RealVector& resp_grad = surrData.response_gradient(i);
	for (j=0; j<num_grad_rhs; ++j) // i-th point, j-th grad component
	  b_vectors[(j+num_coeff_rhs)*num_data_pts_fn+b_cntr] = resp_grad[j];
      }
      if (add_val || add_grad) ++b_cntr; // TO DO: review logic
    }
#ifdef DEBUG
    RealMatrix A2(Teuchos::View, A_matrix, num_rows_A, num_rows_A, num_cols_A),
               b2(Teuchos::View, b_vectors, num_rows_A, num_rows_A, num_rhs);
    PCout << "A_matrix:\n";  write_data(PCout, A2, false, true, true);
    PCout << "b_vectors:\n"; write_data(PCout, b2, false, true, true);
#endif // DEBUG

    // Least squares computation using LAPACK's DGELSS subroutine which uses a
    // SVD method for solving the least squares problem
    info = 0;
    la.GELSS(num_rows_A, num_cols_A, num_rhs, A_matrix, num_rows_A, b_vectors,
	     num_rows_A, s_vector, rcond, &rank, work, lwork, &info);
    if (info)
      lapack_err = true;

    // DGELSS returns the x solution in the numExpansionTerms x num_rhs
    // submatrix of b_vectors
    if (expConfigOptions.expansionCoeffFlag)
      copy_data(b_vectors, numExpansionTerms, expansionCoeffs);
    if (expConfigOptions.expansionCoeffGradFlag)
      for (i=0; i<numExpansionTerms; ++i)
	for (j=0; j<num_grad_rhs; ++j)
	  expansionCoeffGrads(j,i)
	    = b_vectors[(j+num_coeff_rhs)*num_data_pts_fn+i]; // TO DO: review

#ifdef DEBUG
    // For SVD, the condition number can be extracted from the singular values
    PCout << "\nCondition number for LLS using LAPACK SVD (GELSS) is "
	  << s_vector[0]/s_vector[num_cols_A-1] << '\n';
#endif // DEBUG

    delete [] b_vectors;
    delete [] s_vector;
  }
  else {
    // Use DGELSS for LLS soln using SVD.  Solves min ||b - Ax||_2
    // with possible gradient enhancement for a single RHS
    int    num_rows_A = (basisConfigOptions.useDerivs) ?
      num_data_pts_fn + num_data_pts_grad * numVars : num_data_pts_fn;
    int    num_rhs    =  1;  // input: # of RHS vectors
    double rcond      = -1.; // input:  use macheps to rank singular vals of A
    int    rank       =  0;  // output: effective rank of matrix A
    // Matrix of polynomial terms unrolled into a vector.
    A_matrix          = new double [num_rows_A*num_cols_A]; // "A" in A*x = b
    // input: vectors of response data that correspond to samples in matrix A.
    double* b_vectors = new double [num_rows_A*num_rhs]; // "b" in A*x = b
    // output: vector of singular values, dimensioned for overdetermined system
    double* s_vector  = new double [num_cols_A];

    // Get the optimal work array size
    int lwork         = -1; // special code for workspace query
    work              = new double [1]; // temporary work array
    la.GELSS(num_rows_A, num_cols_A, num_rhs, A_matrix, num_rows_A, b_vectors,
	     num_rows_A, s_vector, rcond, &rank, work, lwork, &info);
    lwork = (int)work[0]; // optimal work array size returned by query
    delete [] work;
    work              = new double [lwork]; // Optimal work array

    // The "A" matrix is a contiguous block of memory packed in column-major
    // ordering as required by F77 for the GELSS subroutine from LAPACK.  For
    // example, the 6 elements of A(2,3) are stored in the order A(1,1), A(2,1),
    // A(1,2), A(2,2), A(1,3), A(2,3).
    size_t a_cntr = 0, b_cntr = 0;
    for (i=0; i<numExpansionTerms; ++i) {
      const UShortArray& mi = multiIndex[i];
      for (j=0, fit=failed_resp_data.begin(); j<num_surr_data_pts; ++j) {
	add_val = true; add_grad = basisConfigOptions.useDerivs;
	fail_booleans(fit, j, add_val, add_grad);
	pack_polynomial_data(surrData.continuous_variables(j), mi, add_val,
			     A_matrix, a_cntr, add_grad, A_matrix, a_cntr);
      }
    }

    // response data (values/gradients) define the multiple RHS which are
    // matched in the LS soln.  b_vectors is num_data_pts (rows) x num_rhs
    // (cols), arranged in column-major order.
    const SDRArray& sdr_array = surrData.response_data();
    for (i=0, fit=failed_resp_data.begin(); i<num_surr_data_pts; ++i) {
      add_val = true; add_grad = basisConfigOptions.useDerivs;
      fail_booleans(fit, i, add_val, add_grad);
      pack_response_data(sdr_array[i], add_val, b_vectors, b_cntr, add_grad,
			 b_vectors, b_cntr);
    }
#ifdef DEBUG
    RealMatrix A2(Teuchos::View, A_matrix, num_rows_A, num_rows_A, num_cols_A),
               b2(Teuchos::View, b_vectors, num_rows_A, num_rows_A, num_rhs);
    PCout << "A_matrix:\n";  write_data(PCout, A2, false, true, true);
    PCout << "b_vectors:\n"; write_data(PCout, b2, false, true, true);
#endif // DEBUG

    // Least squares computation using LAPACK's DGELSS subroutine which uses a
    // SVD method for solving the least squares problem
    info = 0;
    la.GELSS(num_rows_A, num_cols_A, num_rhs, A_matrix, num_rows_A, b_vectors,
	     num_rows_A, s_vector, rcond, &rank, work, lwork, &info);
    if (info)
      lapack_err = true;

    // DGELSS returns the x solution in the numExpansionTerms x num_rhs
    // submatrix of b_vectors
    copy_data(b_vectors, numExpansionTerms, expansionCoeffs);

#ifdef DEBUG
    // For SVD, the condition number can be extracted from the singular values
    PCout << "\nCondition number for LLS using LAPACK SVD (GELSS) is "
	  << s_vector[0]/s_vector[num_cols_A-1] << '\n';
#endif // DEBUG

    delete [] b_vectors;
    delete [] s_vector;
  }
  delete [] A_matrix;
  delete [] work;

  if (lapack_err) { // if numerical problems in LLS, abort with error message
    PCerr << "Error: nonzero return code from LAPACK linear least squares in "
	  << "OrthogPolyApproximation::compute_coefficients()" << std::endl;
    abort_handler(-1);
  }
}


/** The coefficients of the PCE for the response are calculated using a
    spectral projection of the response against each multivariate orthogonal
    polynomial basis fn using the inner product ratio <f,Psi>/<Psi^2>,
    where inner product <a,b> is the n-dimensional integral of a*b*weighting
    over the support range of the n-dimensional (composite) weighting
    function.  When interpreting the weighting function as a probability
    density function, <a,b> = expected value of a*b, which can be evaluated
    by sampling from the probability density function and computing the mean
    statistic.  It is not necessary to compute the mean statistic for the
    denominator, since this is available analytically. */
void OrthogPolyApproximation::expectation()
{
  // "lhs" or "random", no weights needed
  size_t i, j, k, num_surr_data_pts = surrData.size(), num_failed_surr_fn = 0,
    num_failed_surr_grad = 0, num_deriv_vars = expansionCoeffGrads.numRows();
  SizetShortMap::const_iterator fit;
  short                failed_anchor_data = surrData.failed_anchor_data();
  const SizetShortMap& failed_resp_data   = surrData.failed_response_data();
  for (fit=failed_resp_data.begin(); fit!=failed_resp_data.end(); ++fit) {
    if (fit->second & 1) ++num_failed_surr_fn;
    if (fit->second & 2) ++num_failed_surr_grad;
  }
  size_t num_data_pts_fn = num_surr_data_pts - num_failed_surr_fn,
    num_data_pts_grad    = num_surr_data_pts - num_failed_surr_grad,
    num_total_pts_fn = num_data_pts_fn, num_total_pts_grad = num_data_pts_grad;
  bool anchor_fn = false, anchor_grad = false, anchor_pt = false;
  if (surrData.anchor()) {
    if (expConfigOptions.expansionCoeffFlag     && !(failed_anchor_data & 1))
      { anchor_fn   = anchor_pt = true; ++num_total_pts_fn; }
    if (expConfigOptions.expansionCoeffGradFlag && !(failed_anchor_data & 2))
      { anchor_grad = anchor_pt = true; ++num_total_pts_grad; }
  }

  /*
  // The following implementation evaluates all PCE coefficients
  // using a consistent expectation formulation
  for (i=0; i<numExpansionTerms; ++i) {
    Real& exp_coeff_i = expansionCoeffs[i];
    exp_coeff_i = (anchor_pt) ?
      surrData.anchor_function() * multivariate_polynomial(
        surrData.anchor_continuous_variables(), multiIndex[i]) : 0.0;
    for (j=0; j<num_data_pts; ++j)
      exp_coeff_i += surrData.response_function(j) * 
        multivariate_polynomial(surrData.continuous_variables(j),multiIndex[i]);
    exp_coeff_i /= num_total_pts * norm_squared(multiIndex[i]);
#ifdef DEBUG
    PCout << "coeff[" << i << "] = " << exp_coeff_i
	  << " norm squared[" << i <<"] = " << norm_squared(multiIndex[i])
	  << '\n';
#endif // DEBUG
  }
  */

  // This alternate implementation evaluates the first PCE coefficient (the
  // response mean) as an expectation and then removes the mean from the
  // expectation evaluation of all subsequent coefficients.  This approach
  // has been observed to result in better results for small sample sizes.
  Real empty_r;
  Real& mean      = (expConfigOptions.expansionCoeffFlag) ?
    expansionCoeffs[0] : empty_r;
  Real* mean_grad = (expConfigOptions.expansionCoeffGradFlag) ?
    expansionCoeffGrads[0] : NULL;
  if (expConfigOptions.expansionCoeffFlag) {
    if (anchor_fn)   mean = surrData.anchor_function();
    else             expansionCoeffs = 0.;
  }
  if (expConfigOptions.expansionCoeffGradFlag) {
    if (anchor_grad) copy_data(surrData.anchor_gradient().values(),
			       num_deriv_vars, mean_grad);
    else             expansionCoeffGrads = 0.;
  }
  for (k=0, fit=failed_resp_data.begin(); k<num_surr_data_pts; ++k) {
    bool add_val  = expConfigOptions.expansionCoeffFlag,
         add_grad = expConfigOptions.expansionCoeffGradFlag;
    if (fit != failed_resp_data.end() && fit->first == j) {
      short fail_asv = fit->second;
      if (fail_asv & 1) add_val  = false;
      if (fail_asv & 2) add_grad = false;
      ++fit;
    }
    if (add_val)
      mean += surrData.response_function(k);
    if (add_grad) {
      const RealVector& curr_pt_grad = surrData.response_gradient(k);
      for (j=0; j<num_deriv_vars; ++j)
	mean_grad[j] += curr_pt_grad[j];
    }
  }
  if (expConfigOptions.expansionCoeffFlag)
    mean /= num_total_pts_fn;
  if (expConfigOptions.expansionCoeffGradFlag)
    for (j=0; j<num_deriv_vars; ++j)
      mean_grad[j] /= num_total_pts_grad;

  Real chaos_sample, resp_fn_minus_mean, norm_sq; Real* exp_grad_i;
  RealVector resp_grad_minus_mean;
  if (expConfigOptions.expansionCoeffGradFlag)
    resp_grad_minus_mean.sizeUninitialized(num_deriv_vars);
  if (anchor_pt) {
    if (anchor_fn)
      resp_fn_minus_mean = surrData.anchor_function() - mean;
    if (anchor_grad) {
      const RealVector& anch_grad = surrData.anchor_gradient();
      for (j=0; j<num_deriv_vars; ++j)
	resp_grad_minus_mean[j] = anch_grad[j] - mean_grad[j];
    }
    const RealVector& c_vars = surrData.anchor_continuous_variables();
    for (i=1; i<numExpansionTerms; ++i) {
      chaos_sample = multivariate_polynomial(c_vars, multiIndex[i]);
      if (anchor_fn)
	expansionCoeffs[i] = resp_fn_minus_mean * chaos_sample;
      if (anchor_grad) {
	exp_grad_i = expansionCoeffGrads[i];
	for (j=0; j<num_deriv_vars; ++j)
	  exp_grad_i[j] = resp_grad_minus_mean[j] * chaos_sample;
      }
    }
  }
  for (k=0, fit=failed_resp_data.begin(); k<num_surr_data_pts; ++k) {
    bool add_val  = expConfigOptions.expansionCoeffFlag,
         add_grad = expConfigOptions.expansionCoeffGradFlag;
    if (fit != failed_resp_data.end() && fit->first == j) {
      short fail_asv = fit->second;
      if (fail_asv & 1) add_val  = false;
      if (fail_asv & 2) add_grad = false;
      ++fit;
    }
    if (add_val)
      resp_fn_minus_mean = surrData.response_function(k) - mean;
    if (add_grad) {
      const RealVector& resp_grad = surrData.response_gradient(k);
      for (j=0; j<num_deriv_vars; ++j)
	resp_grad_minus_mean[j] = resp_grad[j] - mean_grad[j];
    }
    const RealVector& c_vars = surrData.continuous_variables(k);
    for (i=1; i<numExpansionTerms; ++i) {
      chaos_sample = multivariate_polynomial(c_vars, multiIndex[i]);
      if (add_val)
	expansionCoeffs[i] += resp_fn_minus_mean * chaos_sample;
      if (add_grad) {
	exp_grad_i = expansionCoeffGrads[i];
	for (j=0; j<num_deriv_vars; ++j)
	  exp_grad_i[j] += resp_grad_minus_mean[j] * chaos_sample;
      }
    }
  }
  for (i=1; i<numExpansionTerms; ++i) {
    norm_sq = norm_squared(multiIndex[i]);
    if (expConfigOptions.expansionCoeffFlag)
      expansionCoeffs[i] /= norm_sq * num_total_pts_fn;
    if (expConfigOptions.expansionCoeffGradFlag) {
      exp_grad_i = expansionCoeffGrads[i];
      for (j=0; j<num_deriv_vars; ++j)
	exp_grad_i[j] /= norm_sq * num_total_pts_grad;
    }
#ifdef DEBUG
    PCout << "coeff[" << i << "] = " << expansionCoeffs[i]
        //<< "coeff_grad[" << i <<"] = " << exp_grad_i
	  << " norm squared[" << i <<"] = " << norm_sq << '\n';
#endif // DEBUG
  }
}


Real OrthogPolyApproximation::value(const RealVector& x)
{
  // Error check for required data
  if (!expConfigOptions.expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::value()" << std::endl;
    abort_handler(-1);
  }

  // sum expansion to get response value prediction
  Real approx_val = 0.;
  for (size_t i=0; i<numExpansionTerms; ++i)
    approx_val += expansionCoeffs[i]*multivariate_polynomial(x, multiIndex[i]);
  return approx_val;
}


const RealVector& OrthogPolyApproximation::
gradient_basis_variables(const RealVector& x)
{
  // could define a default_dvv and call gradient_basis_variables(x, dvv),
  // but we want this fn to be as fast as possible

  // Error check for required data
  if (!expConfigOptions.expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::gradient_basis_variables()" << std::endl;
    abort_handler(-1);
  }

  if (approxGradient.length() != numVars)
    approxGradient.size(numVars); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  size_t i, j;
  for (i=0; i<numExpansionTerms; ++i) {
    const RealVector& term_i_grad
      = multivariate_polynomial_gradient(x, multiIndex[i]);
    Real& coeff_i = expansionCoeffs[i];
    for (j=0; j<numVars; ++j)
      approxGradient[j] += coeff_i * term_i_grad[j];
  }
  return approxGradient;
}


const RealVector& OrthogPolyApproximation::
gradient_basis_variables(const RealVector& x, const SizetArray& dvv)
{
  // Error check for required data
  if (!expConfigOptions.expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::gradient_basis_variables()" << std::endl;
    abort_handler(-1);
  }

  size_t i, j, num_deriv_vars = dvv.size();
  if (approxGradient.length() != num_deriv_vars)
    approxGradient.size(num_deriv_vars); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  for (i=0; i<numExpansionTerms; ++i) {
    const RealVector& term_i_grad
      = multivariate_polynomial_gradient(x, multiIndex[i], dvv);
    Real& coeff_i = expansionCoeffs[i];
    for (j=0; j<num_deriv_vars; ++j)
      approxGradient[j] += coeff_i * term_i_grad[j];
  }
  return approxGradient;
}


const RealVector& OrthogPolyApproximation::
gradient_nonbasis_variables(const RealVector& x)
{
  // Error check for required data
  if (!expConfigOptions.expansionCoeffGradFlag) {
    PCerr << "Error: expansion coefficient gradients not defined in OrthogPoly"
	  << "Approximation::gradient_coefficient_variables()" << std::endl;
    abort_handler(-1);
  }

  size_t i, j, num_deriv_vars = expansionCoeffGrads.numRows();
  if (approxGradient.length() != num_deriv_vars)
    approxGradient.size(num_deriv_vars); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  for (i=0; i<numExpansionTerms; ++i) {
    Real term_i = multivariate_polynomial(x, multiIndex[i]);
    const Real* exp_coeff_grad_i = expansionCoeffGrads[i];
    for (j=0; j<num_deriv_vars; ++j)
      approxGradient[j] += exp_coeff_grad_i[j] * term_i;
  }
  return approxGradient;
}


Real OrthogPolyApproximation::stored_value(const RealVector& x)
{
  // Error check for required data
  size_t i, num_stored_terms = storedMultiIndex.size();
  if (!num_stored_terms || storedExpCoeffs.length() != num_stored_terms) {
    PCerr << "Error: stored expansion coefficients not available in "
	  << "OrthogPolyApproximation::stored_value()" << std::endl;
    abort_handler(-1);
  }

  // sum expansion to get response value prediction
  Real approx_val = 0.;
  for (i=0; i<num_stored_terms; ++i)
    approx_val += storedExpCoeffs[i] *
      multivariate_polynomial(x, storedMultiIndex[i]);
  return approx_val;
}


const RealVector& OrthogPolyApproximation::
stored_gradient_basis_variables(const RealVector& x)
{
  // Error check for required data
  size_t i, j, num_stored_terms = storedMultiIndex.size();
  if (!num_stored_terms || storedExpCoeffs.length() != num_stored_terms) {
    PCerr << "Error: stored expansion coefficients not available in OrthogPoly"
	  << "Approximation::stored_gradient_basis_variables()" << std::endl;
    abort_handler(-1);
  }

  if (approxGradient.length() != numVars)
    approxGradient.size(numVars); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  for (i=0; i<num_stored_terms; ++i) {
    const RealVector& term_i_grad
      = multivariate_polynomial_gradient(x, storedMultiIndex[i]);
    Real& coeff_i = storedExpCoeffs[i];
    for (j=0; j<numVars; ++j)
      approxGradient[j] += coeff_i * term_i_grad[j];
  }
  return approxGradient;
}


const RealVector& OrthogPolyApproximation::
stored_gradient_nonbasis_variables(const RealVector& x)
{
  // Error check for required data
  size_t i, j, num_stored_terms = storedMultiIndex.size(),
    num_deriv_vars = storedExpCoeffGrads.numRows();
  if (!num_stored_terms || storedExpCoeffGrads.numCols() != num_stored_terms) {
    PCerr << "Error: stored expansion coeff grads not available in OrthogPoly"
	  << "Approximation::stored_gradient_nonbasis_variables()" << std::endl;
    abort_handler(-1);
  }

  if (approxGradient.length() != num_deriv_vars)
    approxGradient.size(num_deriv_vars); // init to 0
  else
    approxGradient = 0.;

  // sum expansion to get response gradient prediction
  for (i=0; i<num_stored_terms; ++i) {
    Real term_i = multivariate_polynomial(x, storedMultiIndex[i]);
    const Real* coeff_grad_i = storedExpCoeffGrads[i];
    for (j=0; j<num_deriv_vars; ++j)
      approxGradient[j] += coeff_grad_i[j] * term_i;
  }
  return approxGradient;
}


/** In this case, all expansion variables are random variables and the
    mean of the expansion is simply the first chaos coefficient. */
Real OrthogPolyApproximation::mean()
{
  // Error check for required data
  if (!expConfigOptions.expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::mean()" << std::endl;
    abort_handler(-1);
  }

  if (expansionMoments.empty())
    expansionMoments.sizeUninitialized(2);
  Real& mean = expansionMoments[0];
  mean = expansionCoeffs[0];
  return mean;
}


/** In this case, a subset of the expansion variables are random
    variables and the mean of the expansion involves evaluating the
    expectation over this subset. */
Real OrthogPolyApproximation::mean(const RealVector& x)
{
  // Error check for required data
  if (!expConfigOptions.expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::mean()" << std::endl;
    abort_handler(-1);
  }

  // sum expansion to get response prediction
  if (expansionMoments.empty())
    expansionMoments.sizeUninitialized(2);
  Real& mean = expansionMoments[0];
  mean = expansionCoeffs[0];

  size_t i;
  SizetList::iterator it;
  for (i=1; i<numExpansionTerms; ++i) {
    bool include = true;
    for (it=randomIndices.begin(); it!=randomIndices.end(); ++it)
      if (multiIndex[i][*it]) // expectation of this expansion term is zero
	{ include = false; break; }
    if (include) {
      Real Psi = 1.0;
      for (it=nonRandomIndices.begin(); it!=nonRandomIndices.end(); ++it) {
	size_t index = *it;
	unsigned short order_1d = multiIndex[i][index];
	if (order_1d)
	  Psi *= polynomialBasis[index].type1_value(x[index], order_1d);
      }
      mean += Psi*expansionCoeffs[i];
#ifdef DEBUG
      PCout << "Mean estimate inclusion: term index = " << i
	    << " Psi = " << Psi << " PCE coeff = " << expansionCoeffs[i]
	    << " total = " << mean << '\n';
#endif // DEBUG
    }
  }

  return mean;
}


/** In this function, all expansion variables are random variables and
    any design/state variables are omitted from the expansion.  In
    this case, the derivative of the expectation is the expectation of
    the derivative.  The mixed derivative case (some design variables
    are inserted and some are augmented) requires no special treatment. */
const RealVector& OrthogPolyApproximation::mean_gradient()
{
  // d/ds \mu_R = d/ds \alpha_0 = <dR/ds>

  // Error check for required data
  if (!expConfigOptions.expansionCoeffGradFlag) {
    PCerr << "Error: expansion coefficient gradients not defined in "
	  << "OrthogPolyApproximation::mean_gradient()." << std::endl;
    abort_handler(-1);
  }

  //meanGradient = expansionCoeffGrads[0];
  return meanGradient
    = Teuchos::getCol(Teuchos::Copy, expansionCoeffGrads, 0);
  return meanGradient;
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
const RealVector& OrthogPolyApproximation::
mean_gradient(const RealVector& x, const SizetArray& dvv)
{
  size_t i, j, deriv_index, num_deriv_vars = dvv.size();
  if (meanGradient.length() != num_deriv_vars)
    meanGradient.sizeUninitialized(num_deriv_vars);
  meanGradient = 0.0;

  SizetList::iterator it;
  size_t cntr = 0; // insertions carried in order within expansionCoeffGrads
  for (i=0; i<num_deriv_vars; ++i) {
    deriv_index = dvv[i] - 1; // OK since we are in an "All" view
    if (randomVarsKey[deriv_index]) { // deriv w.r.t. design var insertion
      // Error check for required data
      if (!expConfigOptions.expansionCoeffGradFlag) {
	PCerr << "Error: expansion coefficient gradients not defined in "
	      << "OrthogPolyApproximation::mean_gradient()." << std::endl;
	abort_handler(-1);
      }
      meanGradient[i] = expansionCoeffGrads[0][cntr];
    }
    else if (!expConfigOptions.expansionCoeffFlag) {// error check for reqd data
      PCerr << "Error: expansion coefficients not defined in "
	    << "OrthogPolyApproximation::mean_gradient()" << std::endl;
      abort_handler(-1);
    }
    for (j=1; j<numExpansionTerms; ++j) {
      bool include = true;
      for (it=randomIndices.begin(); it!=randomIndices.end(); ++it)
	if (multiIndex[j][*it]) // expectation of this expansion term is zero
	  { include = false; break; }
      if (include) {
	// In both cases below, the term to differentiate is alpha_j(s) Psi_j(s)
	// since <Psi_j>_xi = 1 for included terms.  The difference occurs based
	// on whether a particular s_i dependence appears in alpha (for
	// inserted) or Psi (for augmented).
	if (randomVarsKey[deriv_index]) {
	  // -------------------------------------------
	  // derivative w.r.t. design variable insertion
	  // -------------------------------------------
	  Real Psi = 1.0;
	  for (it=nonRandomIndices.begin(); it!=nonRandomIndices.end(); ++it) {
	    size_t index = *it;
	    unsigned short order_1d = multiIndex[j][index];
	    if (order_1d)
	      Psi *= polynomialBasis[index].type1_value(x[index], order_1d);
	  }
	  meanGradient[i] += Psi*expansionCoeffGrads[j][cntr];
	}
	else {
	  // ----------------------------------------------
	  // derivative w.r.t. design variable augmentation
	  // ----------------------------------------------
	  Real term_j_grad_i = 1.0;
	  for (it=nonRandomIndices.begin(); it!=nonRandomIndices.end(); ++it) {
	    size_t index = *it;
	    term_j_grad_i *= (index == deriv_index) ?
	      polynomialBasis[index].type1_gradient(x[index],
						    multiIndex[j][index]) :
	      polynomialBasis[index].type1_value(x[index],
						 multiIndex[j][index]);
	  }
	  meanGradient[i] += expansionCoeffs[j]*term_j_grad_i;
	}
      }
    }
    if (randomVarsKey[deriv_index]) // derivative w.r.t. design var insertion
      ++cntr;
  }

  return meanGradient;
}


/** In this case, all expansion variables are random variables and the
    variance of the expansion is the sum over all but the first term
    of the coefficients squared times the polynomial norms squared. */
Real OrthogPolyApproximation::variance()
{
  if (expansionMoments.empty())
    expansionMoments.sizeUninitialized(2);
  expansionMoments[1] = covariance(this);
  return expansionMoments[1];
}


/** In this case, a subset of the expansion variables are random variables
    and the variance of the expansion involves summations over this subset. */
Real OrthogPolyApproximation::variance(const RealVector& x)
{
  if (expansionMoments.empty())
    expansionMoments.sizeUninitialized(2);
  expansionMoments[1] = covariance(x, this);
  return expansionMoments[1];
}


Real OrthogPolyApproximation::
covariance(PolynomialApproximation* poly_approx_2)
{
  // Error check for required data
  if (!expConfigOptions.expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::covariance()" << std::endl;
    abort_handler(-1);
  }

  const RealVector& exp_coeffs_2
    = ((OrthogPolyApproximation*)poly_approx_2)->expansionCoeffs;
  Real var = 0.;
  for (size_t i=1; i<numExpansionTerms; ++i)
    var += expansionCoeffs[i] * exp_coeffs_2[i] * norm_squared(multiIndex[i]);
  return var;
}


Real OrthogPolyApproximation::
covariance(const RealVector& x, PolynomialApproximation* poly_approx_2)
{
  // Error check for required data
  if (!expConfigOptions.expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::covariance()" << std::endl;
    abort_handler(-1);
  }

  const RealVector& exp_coeffs_2
    = ((OrthogPolyApproximation*)poly_approx_2)->expansionCoeffs;
  Real var = 0.;
  size_t i, j;
  SizetList::iterator it;
  for (i=1; i<numExpansionTerms; ++i) {
    // For r = random_vars and nr = non_random_vars,
    // sigma^2_R(nr) = < (R(r,nr) - \mu_R(nr))^2 >_r
    // -> include only those terms from R(r,nr) which do not appear in \mu_R(nr)
    bool include_i = false;
    for (it=randomIndices.begin(); it!=randomIndices.end(); ++it)
      if (multiIndex[i][*it]) // term does not appear in mean(nr)
	{ include_i = true; break; }
    if (include_i) {
      Real norm_sq_i = norm_squared_random(multiIndex[i]);
      for (j=1; j<numExpansionTerms; ++j) {

	// random part of polynomial must be identical to contribute to variance
	// (else orthogonality drops term)
	bool include_j = true;
	for (it=randomIndices.begin(); it!=randomIndices.end(); ++it)
	  if (multiIndex[i][*it] != multiIndex[j][*it])
	    { include_j = false; break; }
	if (include_j) {
	  Real var_ij = expansionCoeffs[i] * exp_coeffs_2[j] * norm_sq_i;
	  for (it=nonRandomIndices.begin(); it!=nonRandomIndices.end(); ++it) {
	    size_t index = *it;
	    unsigned short order_1d_i = multiIndex[i][index],
	      order_1d_j = multiIndex[j][index];
	    BasisPolynomial& poly_1d = polynomialBasis[index];
	    if (order_1d_i)
	      var_ij *= poly_1d.type1_value(x[index], order_1d_i);
	    if (order_1d_j)
	      var_ij *= poly_1d.type1_value(x[index], order_1d_j);
	  }
	  var += var_ij;
#ifdef DEBUG
	  PCout << "Variance estimate inclusion: term index = " << i
		<< " variance = " << var_ij << " total = " << var <<'\n';
#endif // DEBUG
	}
      }
    }
  }

  return var;
}


/** In this function, all expansion variables are random variables and
    any design/state variables are omitted from the expansion.  The
    mixed derivative case (some design variables are inserted and some
    are augmented) requires no special treatment. */
const RealVector& OrthogPolyApproximation::variance_gradient()
{
  // d/ds \sigma^2_R = Sum_{j=1}^P <Psi^2_j> d/ds \alpha^2_j
  //                 = 2 Sum_{j=1}^P \alpha_j <dR/ds, Psi_j>

  // Error check for required data
  if (!expConfigOptions.expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::variance_gradient()" << std::endl;
    abort_handler(-1);
  }
  if (!expConfigOptions.expansionCoeffGradFlag) {
    PCerr << "Error: expansion coefficient gradients not defined in "
	  << "OrthogPolyApproximation::variance_gradient()." << std::endl;
    abort_handler(-1);
  }

  size_t i, j, num_deriv_vars = expansionCoeffGrads.numRows();
  if (varianceGradient.length() != num_deriv_vars)
    varianceGradient.sizeUninitialized(num_deriv_vars);
  varianceGradient = 0.0;
  for (i=1; i<numExpansionTerms; ++i) {
    Real term_i = 2. * expansionCoeffs[i] * norm_squared(multiIndex[i]);
    for (j=0; j<num_deriv_vars; ++j)
      varianceGradient[j] += term_i * expansionCoeffGrads[i][j];
  }
  return varianceGradient;
}


/** In this function, a subset of the expansion variables are random
    variables and any augmented design/state variables (i.e., not
    inserted as random variable distribution parameters) are included
    in the expansion.  This function must handle the mixed case, where
    some design/state variables are augmented (and are part of the
    expansion) and some are inserted (derivatives are obtained from
    expansionCoeffGrads). */
const RealVector& OrthogPolyApproximation::
variance_gradient(const RealVector& x, const SizetArray& dvv)
{
  // Error check for required data
  if (!expConfigOptions.expansionCoeffFlag) {
    PCerr << "Error: expansion coefficients not defined in "
	  << "OrthogPolyApproximation::variance_gradient()" << std::endl;
    abort_handler(-1);
  }

  size_t i, j, k, deriv_index, num_deriv_vars = dvv.size();
  if (varianceGradient.length() != num_deriv_vars)
    varianceGradient.sizeUninitialized(num_deriv_vars);
  varianceGradient = 0.0;

  SizetList::iterator it;
  size_t cntr = 0; // insertions carried in order within expansionCoeffGrads
  for (i=0; i<num_deriv_vars; ++i) {
    deriv_index = dvv[i] - 1; // OK since we are in an "All" view
    if (randomVarsKey[deriv_index] && !expConfigOptions.expansionCoeffGradFlag){
      PCerr << "Error: expansion coefficient gradients not defined in "
	    << "OrthogPolyApproximation::variance_gradient()." << std::endl;
      abort_handler(-1);
    }
    for (j=1; j<numExpansionTerms; ++j) {
      bool include_j = false;
      for (it=randomIndices.begin(); it!=randomIndices.end(); ++it)
	if (multiIndex[j][*it]) // term does not appear in mean(nr)
	  { include_j = true; break; }
      if (include_j) {
	Real norm_sq_j = norm_squared_random(multiIndex[j]);
	for (k=1; k<numExpansionTerms; ++k) {
	  // random part of polynomial must be identical to contribute to
	  // variance (else orthogonality drops term)
	  bool include_k = true;
	  for (it=randomIndices.begin(); it!=randomIndices.end(); ++it)
	    if (multiIndex[j][*it] != multiIndex[k][*it])
	      { include_k = false; break; }
	  if (include_k) {
	    // In both cases below, the term to differentiate is
	    // alpha_j(s) alpha_k(s) <Psi_j^2>_xi Psi_j(s) Psi_k(s) and the
	    // difference occurs based on whether a particular s_i dependence
	    // appears in alpha (for inserted) or Psi (for augmented).
	    if (randomVarsKey[deriv_index]) {
	      // -------------------------------------------
	      // derivative w.r.t. design variable insertion
	      // -------------------------------------------
	      Real var_jk = norm_sq_j *
		(expansionCoeffs[j] * expansionCoeffGrads[k][cntr] +
		 expansionCoeffs[k] * expansionCoeffGrads[j][cntr]);
	      for (it=nonRandomIndices.begin(); it!=nonRandomIndices.end();
		   ++it) {
		size_t index = *it;
		unsigned short order_1d_j = multiIndex[j][index],
		  order_1d_k = multiIndex[k][index];
		BasisPolynomial& poly_1d = polynomialBasis[index];
		if (order_1d_j)
		  var_jk *= poly_1d.type1_value(x[index], order_1d_j);
		if (order_1d_k)
		  var_jk *= poly_1d.type1_value(x[index], order_1d_k);
	      }
	      varianceGradient[i] += var_jk;
	    }
	    else {
	      // ----------------------------------------------
	      // derivative w.r.t. design variable augmentation
	      // ----------------------------------------------
	      Real var_jk = expansionCoeffs[j] * expansionCoeffs[k] * norm_sq_j;
	      Real Psi_j = 1., Psi_k = 1., dPsi_j_ds_i = 1., dPsi_k_ds_i = 1.;
	      for (it=nonRandomIndices.begin(); it!=nonRandomIndices.end();
		   ++it) {
		size_t index = *it;
		unsigned short order_1d_j = multiIndex[j][index],
		  order_1d_k = multiIndex[k][index];
		BasisPolynomial& poly_1d = polynomialBasis[index];
		if (order_1d_j)
		  Psi_j *= poly_1d.type1_value(x[index], order_1d_j);
		if (order_1d_k)
		  Psi_k *= poly_1d.type1_value(x[index], order_1d_k);
		dPsi_j_ds_i *= (index == deriv_index) ?
		  poly_1d.type1_gradient(x[index], order_1d_j) :
		  poly_1d.type1_value(   x[index], order_1d_j);
		dPsi_k_ds_i *= (index == deriv_index) ?
		  poly_1d.type1_gradient(x[index], order_1d_k) :
		  poly_1d.type1_value(   x[index], order_1d_k);
	      }
	      varianceGradient[i] += var_jk * 
		(Psi_j*dPsi_k_ds_i + dPsi_j_ds_i*Psi_k);
	    }
	  }
	}
      }
    }
    if (randomVarsKey[deriv_index]) // derivative w.r.t. design var insertion
      ++cntr;
  }

  return varianceGradient;
}


/** This test works in combination with DEBUG settings in
    (Legendre,Laguerre,Jacobi,GenLaguerre)OrthogPolynomial::type1_gradient(). */
void OrthogPolyApproximation::gradient_check()
{
  BasisPolynomial hermite_poly(HERMITE_ORTHOG), legendre_poly(LEGENDRE_ORTHOG),
    laguerre_poly(LAGUERRE_ORTHOG), jacobi_poly(JACOBI_ORTHOG),
    gen_laguerre_poly(GEN_LAGUERRE_ORTHOG), chebyshev_poly(CHEBYSHEV_ORTHOG);
  // alpha/beta selections mirror dakota_uq_rosenbrock_pce.in
  jacobi_poly.alpha_stat(1.5);
  jacobi_poly.beta_stat(2.);
  gen_laguerre_poly.alpha_stat(2.5);

  Real x = 0.5; // valid for all support ranges: [-1,1], [0,Inf], [-Inf, Inf]
  PCout << "-------------------------------------------------\n";
  for (size_t n=0; n<=10; n++) {
    PCout << "Gradients at " << x << " for order " << n << '\n';
    hermite_poly.type1_gradient(x, n);
    legendre_poly.type1_gradient(x, n);
    laguerre_poly.type1_gradient(x, n);
    jacobi_poly.type1_gradient(x, n);
    gen_laguerre_poly.type1_gradient(x, n);
    chebyshev_poly.type1_gradient(x, n);
    PCout << "-------------------------------------------------\n";
  }
}


Real OrthogPolyApproximation::norm_squared(const UShortArray& indices)
{
  // the norm squared of a particular multivariate polynomial is the product of
  // the norms squared of the numVars univariate polynomials that comprise it.
  Real multivar_norm_sq = 1.;
  unsigned short order;
  for (size_t i=0; i<numVars; ++i) {
    order = indices[i];
    if (order)
      multivar_norm_sq *= polynomialBasis[i].norm_squared(order);
  }
  return multivar_norm_sq;
}


Real OrthogPolyApproximation::norm_squared_random(const UShortArray& indices)
{
  // the norm squared of a particular multivariate polynomial is the product of
  // the norms squared of the numVars univariate polynomials that comprise it.
  Real multivar_norm_sq = 1.;
  unsigned short order;
  for (size_t i=0; i<numVars; ++i) {
    if (randomVarsKey[i]) {
      order = indices[i];
      if (order)
	multivar_norm_sq *= polynomialBasis[i].norm_squared(order);
    }
  }
  return multivar_norm_sq;
}


void OrthogPolyApproximation::
compute_numerical_response_moments(size_t num_moments)
{
  size_t i, num_pts = surrData.size();
  bool anchor_pt = surrData.anchor();
  if (anchor_pt) ++num_pts;

  // define data_coeffs
  RealVector data_coeffs(num_pts);
  if (anchor_pt) {
    data_coeffs[0] = surrData.anchor_function();
    for (i=1; i<num_pts; ++i)
      data_coeffs[i] = surrData.response_function(i-1);
  }
  else
    for (i=0; i<num_pts; ++i)
      data_coeffs[i] = surrData.response_function(i);

  if (storedExpCombineType && !storedExpCoeffs.empty()) {
    // update data_coeffs using evaluations from stored expansions
    switch (storedExpCombineType) {
    case ADD_COMBINE:
      if (anchor_pt) {
	data_coeffs[0] += stored_value(surrData.anchor_continuous_variables());
	for (i=1; i<num_pts; ++i)
	  data_coeffs[i] += stored_value(surrData.continuous_variables(i-1));
      }
      else
	for (i=0; i<num_pts; ++i)
	  data_coeffs[i] += stored_value(surrData.continuous_variables(i));
      break;
    case MULT_COMBINE:
      if (anchor_pt) {
	data_coeffs[0] *= stored_value(surrData.anchor_continuous_variables());
	for (i=1; i<num_pts; ++i)
	  data_coeffs[i] *= stored_value(surrData.continuous_variables(i-1));
      }
      else
	for (i=0; i<num_pts; ++i)
	  data_coeffs[i] *= stored_value(surrData.continuous_variables(i));
      break;
    }
    // stored data may now be cleared
    storedMultiIndex.clear();
    if (expConfigOptions.expansionCoeffFlag)
      storedExpCoeffs.resize(0);
    if (expConfigOptions.expansionCoeffGradFlag)
      storedExpCoeffGrads.reshape(0,0);
  }

  // update numericalMoments based on data_coeffs
  compute_numerical_moments(num_moments, data_coeffs, numericalMoments);
}


void OrthogPolyApproximation::compute_component_effects()
{
  // sobolIndices are index via binary number represenation
  // e.g. if there are 5 variables -> 5 bit represenation
  // if a variable belongs to a subset \alpha, it has a value of 1; otherwise 0
  // | 0 | 0 | 0 | 0 | 1 | represents the subset that only contains variable 1
  // sobolIndices[0] is set to 1; to calculate it is redundant

  // Index the set using binary number representation
  size_t index_bin, i, j;
  // Compute a pseudo-variance that makes no distinction between probabilistic
  // variables and non-probabilistic variables
  Real p_var = 0, p_var_i;
  for (i=1; i<numExpansionTerms; i++) 
    p_var += norm_squared(multiIndex[i])*expansionCoeffs(i)*expansionCoeffs(i);

  // iterate through multiIndex and store sensitivities
  sobolIndices    = 0.; // initialize
  sobolIndices[0] = 1.; // just a place holder; zero index is never invoked
  for (i=1; i<numExpansionTerms; ++i) {
    index_bin = 0;
    p_var_i = norm_squared(multiIndex[i]) * expansionCoeffs(i) *
      expansionCoeffs(i) / p_var;
    for (j=0; j<numVars; ++j)
      if (multiIndex[i][j]) // convert this subset into binary number
	index_bin += (size_t)pow(2.,(int)j);
    // If term is main effect (exists in map), keep; otherwise, discard
    if (sobolIndexMap.find(index_bin) != sobolIndexMap.end())
      sobolIndices[sobolIndexMap[index_bin]] += p_var_i;
  }
#ifdef DEBUG
  PCout << "In OrthogPolyApproximation::compute_component_effects(), "
	<< "sobolIndices =\n"; write_data(PCout, sobolIndices);
#endif // DEBUG
}


void OrthogPolyApproximation::compute_total_effects() 
{
  // iterate through existing indices if all component indices are available
  totalSobolIndices = 0.;
  if (expConfigOptions.vbdControl == ALL_VBD) {
    for (IntIntMIter itr=sobolIndexMap.begin(); itr!=sobolIndexMap.end(); ++itr)
      for (int k=0; k<numVars; k++) 
        if (itr->first & (1 << k))
          totalSobolIndices[k] += sobolIndices[itr->second];
  }
  // otherwise, iterate over the expansion terms as it is more 
  // computationally efficient than performing ANOVA operators
  else {
    // Index the set using binary number representation
    size_t index_bin, i, j;
    Real p_var = 0, p_var_i;
    for (i=1; i<numExpansionTerms; i++) 
      p_var += norm_squared(multiIndex[i]) * expansionCoeffs(i)
	* expansionCoeffs(i);
  
    // Computing total indices by iterating through expansion terms is simpler
    // and more computationally efficient than computing via ANOVA operators 
    for (i=1; i<numExpansionTerms; ++i) {
      index_bin = 0;
      p_var_i = norm_squared(multiIndex[i]) * expansionCoeffs(i) *
        expansionCoeffs(i) / p_var;
      for (j=0; j<numVars; ++j) {
        if (multiIndex[i][j]) {
          // convert this subset multiIndex[i] into binary number
          index_bin += (size_t)pow(2.,(int)j);
          // for any constituent variable j in exansion term i, the expansion
          // term contributes to the total sensitivty of variable j
          totalSobolIndices[j] += p_var_i;
        }
      }
    }
  }
#ifdef DEBUG
  PCout << "In OrthogPolyApproximation::compute_total_effects(), "
	<< "totalSobolIndices =\n"; write_data(PCout, totalSobolIndices);
#endif // DEBUG
}


const RealVector& OrthogPolyApproximation::dimension_decay_rates()
{
  if (decayRates.empty())
    decayRates.sizeUninitialized(numVars);

  // define max_orders for each var for sizing LLS matrices/vectors
  size_t i, j;
  UShortArray max_orders(numVars, 0);
  for (i=0; i<numExpansionTerms; ++i)
    for (j=0; j<numVars; ++j)
      if (multiIndex[i][j] > max_orders[j])
	max_orders[j] = multiIndex[i][j];
  // size A_vectors and b_vectors
  RealVectorArray A_vectors(numVars), b_vectors(numVars);
  for (i=0; i<numVars; ++i) {
    A_vectors[i].sizeUninitialized(max_orders[i]);
    b_vectors[i].sizeUninitialized(max_orders[i]);
  }

  // populate A_vectors and b_vectors
  unsigned short order, non_zero, var_index, order_index;
  bool univariate;
  for (i=1; i<numExpansionTerms; ++i) {
    univariate = true; non_zero = 0;
    for (j=0; j<numVars; ++j) {
      if (multiIndex[i][j]) {
	++non_zero;
	if (non_zero > 1) { univariate = false; break; }
	else { order = multiIndex[i][j]; var_index = j; order_index = order-1; }
      }
    }
    if (univariate) {
      // find a for y = ax + b with x = term order, y = log(coeff), and
      // b = known intercept for order x = 0
      Real norm   = std::sqrt(polynomialBasis[var_index].norm_squared(order)),
	abs_coeff = std::abs(expansionCoeffs[i]);
#ifdef DECAY_DEBUG
      PCout << "Univariate contribution: order = " << order << " coeff = "
	    << abs_coeff << " norm = " << norm << '\n';
#endif // DECAY_DEBUG
      A_vectors[var_index][order_index] = (Real)order;
      b_vectors[var_index][order_index] = (abs_coeff > 1.e-25) ?
	std::log10(abs_coeff * norm) : std::log10(norm) - 25.;
    }
  }
#ifdef DECAY_DEBUG
  PCout << "raw b_vectors:\n";
  for (i=0; i<numVars; ++i)
    { PCout << "Variable " << i+1 << '\n'; write_data(PCout, b_vectors[i]); }
#endif // DECAY_DEBUG

  // first coefficient is used in each of the LLS solves
  Real log_coeff0 = std::log10(std::abs(expansionCoeffs[0])), tol = -10.;
  short last_index_above = -1, new_size;
  //Teuchos::LAPACK<int, Real> la;
  //int rank = 0, info = 0, lwork; RealVector s_vector(1), work;
  for (i=0; i<numVars; ++i) {
    RealVector& A_i = A_vectors[i]; RealVector& b_i = b_vectors[i];

    // Handle case of flatline at numerical precision by ignoring subsequent
    // values below a tolerance (allow first, prune second)
    // > high decay rate will de-emphasize refinement, but consider zeroing
    //   out refinement for a direction that is converged below tol (?)
    // > for now, truncate max_orders and scale back {A,b}_vectors
    order = max_orders[i];
    for (j=0; j<order; ++j)
      if (b_i[j] > tol)
	last_index_above = j;
    new_size = last_index_above+2; // include one value below tolerance
    if (new_size < order) {
      max_orders[i] = order = new_size;
      A_i.resize(order); // needed for psuedo-inv but not LAPACK
      b_i.resize(order); // needed for psuedo-inv but not LAPACK
    }

    // subtract intercept b for y = Ax+b  ==>  Ax = y-b
    for (j=0; j<order; ++j)
      b_i[j] -= log_coeff0;

    // employ simple 1-D pseudo inverse for LLS:
    //   A^T A x = A^T(y-b)  ==>  x = A^T(y-b) / A^T A
    // negate negative slope in log space such that large>0 is fast
    // convergence, small>0 is slow convergence, and <0 is divergence
    decayRates[i] = -A_i.dot(b_i) / A_i.dot(A_i);

    /* perform LAPACK LLS for each variable
    int num_rows_A = max_orders[i];
    lwork = -1;  // special code for workspace query
    work.sizeUninitialized(1);
    la.GELSS(num_rows_A, 1, 1, A_vectors[i].values(), num_rows_A,
	     b_vectors[i].values(), num_rows_A, s_vector.values(),
	     -1, &rank, work.values(), lwork, &info);
    lwork = (int)work[0]; // optimal work array size returned by query
    work.sizeUninitialized(lwork);
    la.GELSS(num_rows_A, 1, 1, A_vectors[i].values(), num_rows_A,
	     b_vectors[i].values(), num_rows_A, s_vector.values(),
	     -1, &rank, work.values(), lwork, &info);
    // large>0 is fast converge, small>0 is slow converge, <0 is diverge
    decayRates[i] = -b_vectors[i][0];
    */
  }

#ifdef DECAY_DEBUG
  PCout << "Intercept log(abs(coeff0)) = " << log_coeff0 << '\n';
  PCout << "b_vectors after truncation & intercept subtraction:\n";
  for (i=0; i<numVars; ++i)
    { PCout << "Variable " << i+1 << '\n'; write_data(PCout, b_vectors[i]); }
  PCout << "Individual approximation decay:\n"; write_data(PCout, decayRates);
#endif // DECAY_DEBUG

  return decayRates;
}


void OrthogPolyApproximation::print_coefficients(std::ostream& s) const
{
  size_t i, j;
  char tag[10];

  // terms and term identifiers
  for (i=0; i<numExpansionTerms; ++i) {
    s << "\n  " << std::setw(WRITE_PRECISION+7) << expansionCoeffs[i];
    for (j=0; j<numVars; ++j) {
      switch (basisTypes[j]) {
      case HERMITE_ORTHOG:
	std::sprintf(tag,  "He%i", multiIndex[i][j]);
	break;
      case LEGENDRE_ORTHOG:
	std::sprintf(tag,   "P%i", multiIndex[i][j]);
	break;
      case LAGUERRE_ORTHOG:
	std::sprintf(tag,   "L%i", multiIndex[i][j]);
	break;
      case JACOBI_ORTHOG:
	std::sprintf(tag, "Pab%i", multiIndex[i][j]);
	break;
      case GEN_LAGUERRE_ORTHOG:
	std::sprintf(tag,  "La%i", multiIndex[i][j]);
	break;
      case CHEBYSHEV_ORTHOG:
	std::sprintf(tag,   "T%i", multiIndex[i][j]);
	break;
      case NUM_GEN_ORTHOG:
	std::sprintf(tag, "Num%i", multiIndex[i][j]);
	break;
      default:
	PCerr << "Error: bad polynomial type = " << basisTypes[j] << " in "
	      << "OrthogPolyApproximation::print_coefficients()." << std::endl;
	abort_handler(-1);
	break;
      }
      s << std::setw(5) << tag;
    }
  }
  s << '\n';
}

} // namespace Pecos
