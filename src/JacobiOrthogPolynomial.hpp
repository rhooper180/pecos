/*  _______________________________________________________________________

    PECOS: Parallel Environment for Creation Of Stochastics
    Copyright (c) 2008, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Pecos directory.
    _______________________________________________________________________ */

//- Class:        JacobiOrthogPolynomial
//- Description:  Class for Jacobi Orthogonal Polynomial
//-               
//- Owner:        Mike Eldred, Sandia National Laboratories

#ifndef JACOBI_ORTHOG_POLYNOMIAL_HPP
#define JACOBI_ORTHOG_POLYNOMIAL_HPP

#include "OrthogonalPolynomial.hpp"


namespace Pecos {

/// Derived orthogonal polynomial class for Jacobi polynomials

/** The JacobiOrthogPolynomial class evaluates a univariate Jacobi
    polynomial P^(alpha,beta)_n of a particular order.  These
    polynomials are orthogonal with respect to the weight function
    (1-x)^alpha (1+x)^beta when integrated over the support range of
    [-1,+1].  This corresponds to the probability density function
    f(x) = (1-x)^alpha (1+x)^beta / (2^(alpha+beta+1) B(alpha+1,beta+1))
    for the beta distribution for [L,U]=[-1,1], where common
    statistical PDF notation conventions (see, e.g., the uncertain
    variables section in the DAKOTA Reference Manual) and the
    Abramowiz and Stegun orthogonal polynomial conventions are
    inverted and require conversion in this case (alpha_poly =
    beta_stat - 1; beta_poly = alpha_stat - 1 with the poly
    definitions used in both cases above).  It enables (mixed)
    multidimensional orthogonal polynomial basis functions within
    OrthogPolyApproximation.  A special case is the
    LegendreOrthogPolynomial (implemented separately), for which
    alpha_poly = beta_poly = 0. */

class JacobiOrthogPolynomial: public OrthogonalPolynomial
{
public:

  //
  //- Heading: Constructor and destructor
  //

  /// default constructor
  JacobiOrthogPolynomial();
  /// standard constructor
  JacobiOrthogPolynomial(const Real& alpha_stat, const Real& beta_stat);
  /// destructor
  ~JacobiOrthogPolynomial();

protected:

  //
  //- Heading: Virtual function redefinitions
  //

  /// retrieve the Jacobi polynomial value for a given parameter x 
  const Real& get_value(const Real& x, unsigned short order);
  /// retrieve the Jacobi polynomial gradient for a given parameter x 
  const Real& get_gradient(const Real& x, unsigned short order);

  /// return the inner product <P^(alpha,beta)_n,P^(alpha,beta)_n> =
  /// ||P^(alpha,beta)_n||^2
  const Real& norm_squared(unsigned short order);

  /// return the Gauss-Jacobi quadrature points corresponding to
  /// polynomial order n
  const RealArray& gauss_points(unsigned short order);
  /// return the Gauss-Jacobi quadrature weights corresponding to
  /// polynomial order n
  const RealArray& gauss_weights(unsigned short order);

  /// calculate and return wtFactor based on alphaPoly and betaPoly
  const Real& weight_factor();

  /// set alphaPoly
  void alpha_polynomial(const Real& alpha);
  /// set betaPoly
  void beta_polynomial(const Real& beta);
  /// set betaPoly using the conversion betaPoly = alpha_stat - 1.
  void alpha_stat(const Real& alpha);
  /// set alphaPoly using the conversion alphaPoly = beta_stat - 1.
  void beta_stat(const Real& beta);

private:

  //
  //- Heading: Data
  //

  /// the alpha parameter for the Jacobi polynomial as defined by
  /// Abramowitz and Stegun (differs from statistical PDF notation)
  Real alphaPoly;
  /// the beta parameter for the Jacobi polynomial as defined by
  /// Abramowitz and Stegun (differs from statistical PDF notation)
  Real betaPoly;
};


inline JacobiOrthogPolynomial::JacobiOrthogPolynomial():
  alphaPoly(0.), betaPoly(0.)
{ }


// TO DO
inline JacobiOrthogPolynomial::
JacobiOrthogPolynomial(const Real& alpha_stat, const Real& beta_stat):
  alphaPoly(beta_stat-1.), betaPoly(alpha_stat-1.) // inverted conventions
{ }


inline JacobiOrthogPolynomial::~JacobiOrthogPolynomial()
{ }


inline void JacobiOrthogPolynomial::alpha_polynomial(const Real& alpha)
{ alphaPoly = alpha; reset_gauss(); }


inline void JacobiOrthogPolynomial::beta_polynomial(const Real& beta)
{ betaPoly = beta; reset_gauss(); }


inline void JacobiOrthogPolynomial::alpha_stat(const Real& alpha)
{ betaPoly = alpha - 1.; reset_gauss(); }


inline void JacobiOrthogPolynomial::beta_stat(const Real& beta)
{ alphaPoly = beta - 1.; reset_gauss(); }

} // namespace Pecos

#endif