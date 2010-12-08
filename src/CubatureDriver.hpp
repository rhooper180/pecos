/*  _______________________________________________________________________

    PECOS: Parallel Environment for Creation Of Stochastics
    Copyright (c) 2008, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Pecos directory.
    _______________________________________________________________________ */

//- Class:	 CubatureDriver
//- Description: Wrapper class for cubature components within VPISparseGrid
//- Owner:       Mike Eldred
//- Revised by:  
//- Version:

#ifndef CUBATURE_DRIVER_HPP
#define CUBATURE_DRIVER_HPP

#include "IntegrationDriver.hpp"

namespace Pecos {

class DistributionParams;


/// Generates N-dimensional cubature grids for numerical evaluation of
/// expectation integrals over independent standard random variables.

/** Includes Stroud rules and extensions.  This class is used by
    Dakota::NonDCubature, but could also be used for general numerical
    integration of moments. */

class CubatureDriver: public IntegrationDriver
{
public:

  //
  //- Heading: Constructors and destructor
  //

  CubatureDriver();  ///< default constructor
  ~CubatureDriver(); ///< destructor

  //
  //- Heading: Member functions
  //

  /// initialize cubature settings except for distribution params
  void initialize_grid(const ShortArray& u_types, unsigned short order,
		       unsigned short rule);
  /// initialize all cubature settings (distribution params already
  /// set within poly_basis)
  void initialize_grid(const std::vector<BasisPolynomial>& poly_basis,
		       unsigned short order);

  /// initialize settings for parameterized cubature rules
  void initialize_grid_parameters(const ShortArray& u_types,
				  const DistributionParams& dp);

  // set integrandOrder
  void integrand_order(unsigned short order);
  // get integrandOrder
  unsigned short integrand_order() const;

  /// number of collocation points with duplicates removed
  int grid_size();
  /// compute scaled variable and weight sets for the cubature grid
  void compute_grid(RealMatrix& variable_sets);

private:

  //
  //- Heading: Convenience functions
  //

  /// verify that all values within params are identical
  bool verify_homogeneity(const RealVector& params) const;
  /// verify that all vectors within params are identical
  bool verify_homogeneity(const RealVectorArray& params) const;

  // size integrationRules and set first entry
  void integration_rule(unsigned short rule);

  //
  //- Heading: Data
  //

  /// integrand order
  unsigned short integrandOrder;
};


inline CubatureDriver::CubatureDriver(): IntegrationDriver(BaseConstructor())
{ }


inline CubatureDriver::~CubatureDriver()
{ }


inline void CubatureDriver::integrand_order(unsigned short order)
{ integrandOrder = order; }


inline unsigned short CubatureDriver::integrand_order() const
{ return integrandOrder; }


inline void CubatureDriver::integration_rule(unsigned short rule)
{ integrationRules.resize(1); integrationRules[0] = (int)rule; }


inline bool CubatureDriver::verify_homogeneity(const RealVector& params) const
{
  bool err_flag = false;
  if (!params.empty()) {
    const Real& param0 = params[0];
    for (size_t i=1; i<numVars; ++i)
      if (params[i] != param0)
	{ err_flag = true; break; }
  }
  return err_flag;
}


inline bool CubatureDriver::
verify_homogeneity(const RealVectorArray& params) const
{
  bool err_flag = false;
  if (!params.empty()) {
    const RealVector& param0 = params[0];
    for (size_t i=1; i<numVars; ++i)
      if (params[i] != param0)
	{ err_flag = true; break; }
  }
  return err_flag;
}

} // namespace Pecos

#endif
