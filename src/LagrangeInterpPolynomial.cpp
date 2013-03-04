/*  _______________________________________________________________________

    PECOS: Parallel Environment for Creation Of Stochastics
    Copyright (c) 2011, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Pecos directory.
    _______________________________________________________________________ */

//- Class:        LagrangeInterpPolynomial
//- Description:  Implementation code for LagrangeInterpPolynomial class
//-               
//- Owner:        Mike Eldred, Sandia National Laboratories

#include "LagrangeInterpPolynomial.hpp"


namespace Pecos {

/** Pre-compute denominator products that are only a function of the
    interpolation points. */
void LagrangeInterpPolynomial::precompute_data()
{
  // precompute w_j for all forms of Lagrange interpolation
  size_t i, j, num_interp_pts = interpPts.size();
  if (bcWeights.empty())
    bcWeights.resize(num_interp_pts);
  bcWeights = 1.;
  for (i=0; i<num_interp_pts; ++i) {
    Real interp_pt_i = interpPts[i];
    Real&    bc_wt_i = bcWeights[i];
    for (j=0; j<num_interp_pts; ++j)
      if (i != j)
	bc_wt_i /= interp_pt_i - interpPts[j];
  }
}


/** Define the bcValueFactors (and exactIndex if needed) corresponding to x. */
void LagrangeInterpPolynomial::set_new_point(Real x, short request_order)
{
  // define compute_order from requested and available data.  If grad factors
  // are requested, value factors will also be needed (multidimensional grad
  // components involve values in non-differentiated dimensions).
  short compute_order;
  if (x == newPoint) {
    compute_order = request_order - (newPtOrder & request_order);
    if (request_order == 2 && !(newPtOrder & 1))
      compute_order |= 1;                           // augment request
    if (compute_order) newPtOrder |= compute_order; // augment total available
    else               return;
  }
  else {
    newPtOrder = compute_order = (request_order & 2) ? 3 : request_order;
    newPoint   = x; exactIndex = _NPOS;
  }

  size_t j, num_interp_pts = interpPts.size();
  if (bcWeights.length() != num_interp_pts) {
    PCerr << "Error: length of precomputed bcWeights (" << bcWeights.length()
	  << ") is inconsistent with number of collocation points ("
	  << num_interp_pts << ")." << std::endl;
    abort_handler(-1);
  }

  /* precompute l(x) for 1st form of barycentric interpolation formula
  diffProduct = 1.;
  for (j=0; j<num_interp_pts; ++j) {
    Real diff = newPoint - interpPts[j];
    if (diff == 0.)
      { exactIndex = j; break; }
    else
      diffProduct *= diff;
  }
  */

  // precompute value factors or identify exactIndex for 2nd form of
  // barycentric interpolation formula
  Real diff, diff_inv_sum;
  if (exactIndex == _NPOS) { // exact match may have been previously detected
    if (compute_order & 1) {
      if (bcValueFactors.length() != num_interp_pts)
	bcValueFactors.resize(num_interp_pts);
      bcValueFactorSum = 0.;
    }
    if (compute_order & 2)
      { diffProduct = 1.; diff_inv_sum = 0.; }
    for (j=0; j<num_interp_pts; ++j) {
      diff = newPoint - interpPts[j];
      if (diff == 0.) // no tolerance needed due to favorable stability analysis
	{ exactIndex = j; break; }
      else {
	if (compute_order & 1)
	  bcValueFactorSum += bcValueFactors[j] = bcWeights[j] / diff;
	if (compute_order & 2) {
	  diffProduct  *= diff;
	  diff_inv_sum += 1. / diff;
	}
      }
    }
  }

  // precompute gradient factors for 2nd form of barycentric interpolation
  if (compute_order & 2) {
    if (bcGradFactors.length() != num_interp_pts)
      bcGradFactors.resize(num_interp_pts);
    // bcGradFactors (like bcValueFactors) differ from the actual gradient
    // values by diffProduct, which gets applied after a tensor summation
    // using the barycentric gradient scaling.
    if (exactIndex == _NPOS)
      for (j=0; j<num_interp_pts; ++j) // bcValueFactors must be available
	bcGradFactors[j] = bcValueFactors[j] // * diffProduct
	  * (diff_inv_sum - 1./(newPoint - interpPts[j]));//back out jth inverse
    else { // Berrut and Trefethen, 2004
      // for this case, bcGradFactors are the actual gradient values
      // and no diffProduct scaling needs to be subsequently applied
      bcGradFactors[exactIndex] = 0.;
      for (j=0; j<num_interp_pts; ++j)
	if (j != exactIndex)
	  bcGradFactors[exactIndex] -= bcGradFactors[j] = bcWeights[j]
	    / bcWeights[exactIndex] / (interpPts[exactIndex] - interpPts[j]);
    }
  }
}


/** Compute value of the Lagrange polynomial (1st barycentric form)
    corresponding to interpolation point i using data from previous
    call to set_new_point(). */
Real LagrangeInterpPolynomial::type1_value(unsigned short i)
{
  // second form of the barycentric interpolation formula
  if (exactIndex == _NPOS) return bcValueFactors[i] * diffProduct;
  else                     return (exactIndex == i) ? 1. : 0.;

  /*
  // first form of the barycentric interpolation formula
  if (exactIndex == _NPOS)
    return bcWeights[i] * diffProduct / (newPoint - interpPts[i]);
  else
    return (exactIndex == i) ? 1. : 0.;
  */
}


/** Compute derivative with respect to x of the Lagrange polynomial
    (1st barycentric form) corresponding to interpolation point i
    using data from previous call to set_new_point(). */
Real LagrangeInterpPolynomial::type1_gradient(unsigned short i)
{
  // second form of the barycentric interpolation formula
  return (exactIndex == _NPOS) ?
    bcGradFactors[i] * diffProduct : bcGradFactors[i];

  /*
  // first form of the barycentric interpolation formula
  if (exactIndex == _NPOS) {
    Real sum = 0.,
      t1_i = bcWeights[i] * diffProduct / (newPoint - interpPts[i]);
    size_t j, num_interp_pts = interpPts.size();
    for (j=0; j<num_interp_pts; ++j)
      if (j != i)
	sum += t1_i / (newPoint - interpPts[j]);
    return sum;
  }
  else // double loop fallback
    return type1_gradient(newPoint, i);
  */
}


/** Compute value of the Lagrange polynomial (traditional characteristic
    polynomial form) corresponding to interpolation point i. */
Real LagrangeInterpPolynomial::type1_value(Real x, unsigned short i)
{
  size_t j, num_interp_pts = interpPts.size();
  Real t1_val = bcWeights[i];
  for (j=0; j<num_interp_pts; ++j)
    if (i != j)
      t1_val *= x - interpPts[j];
  return t1_val;
}


/** Compute derivative with respect to x of the Lagrange polynomial (traditional
    characteristic polynomial form) corresponding to interpolation point i. */
Real LagrangeInterpPolynomial::type1_gradient(Real x, unsigned short i)
{ 
  size_t j, k, num_interp_pts = interpPts.size();
  Real sum = 0., prod;
  for (j=0; j<num_interp_pts; ++j) {
    if (j != i) {
      prod = 1.;
      for (k=0; k<num_interp_pts; ++k)
	if (k != j && k != i)
	  prod *= x - interpPts[k];
      sum += prod;
    }
  }
  return sum * bcWeights[i];
}

} // namespace Pecos
