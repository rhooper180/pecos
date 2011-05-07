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
  size_t i, j, num_interp_pts = interpPts.size();
  if (lagDenominators.empty())
    lagDenominators.resize(num_interp_pts);
  lagDenominators = 1.;
  for (i=0; i<num_interp_pts; i++) {
    const Real& interp_pt_i = interpPts[i];
    Real&       lag_denom_i = lagDenominators[i];
    for (j=0; j<num_interp_pts; j++)
      if (i != j)
	lag_denom_i *= interp_pt_i - interpPts[j];
  }
}


/** Compute value of the Lagrange polynomial corresponding to
    interpolation point i. */
const Real& LagrangeInterpPolynomial::
get_type1_value(const Real& x, unsigned short i)
{
  size_t j, num_interp_pts = interpPts.size();
  basisPolyValue = 1. / lagDenominators[i];
  for (j=0; j<num_interp_pts; j++)
    if (i != j)
      basisPolyValue *= x - interpPts[j];
  return basisPolyValue;
}


/** Compute derivative with respect to x of the Lagrange polynomial
    corresponding to interpolation point i. */
const Real& LagrangeInterpPolynomial::
get_type1_gradient(const Real& x, unsigned short i)
{ 
  size_t j, k, num_interp_pts = interpPts.size();
  Real numer = 0.;
  for (j=0; j<num_interp_pts; j++) {
    if (j != i) {
      Real prod = 1.;
      for (k=0; k<num_interp_pts; k++)
	if (k != j && k != i)
	  prod *= x - interpPts[k];
      numer += prod;
    }
  }
  basisPolyGradient = numer / lagDenominators[i];
  return basisPolyGradient;
}

} // namespace Pecos
