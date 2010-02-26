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

  /// set numVars
  void initialize(size_t num_vars, unsigned short prec, int rule);

  /// number of collocation points with duplicates removed
  int grid_size();
  /// compute scaled variable and weight sets for the cubature grid
  void compute_grid();

private:

  //
  //- Heading: Convenience functions
  //

  //
  //- Heading: Data
  //

  /// integrand precision
  unsigned short integrandPrec;
  /// integer code for integration rule
  int integrationRule;
};


inline CubatureDriver::CubatureDriver()
{ }


inline CubatureDriver::~CubatureDriver()
{ }


inline void CubatureDriver::
initialize(size_t num_vars, unsigned short prec, int rule)
{ numVars = num_vars; integrandPrec = prec; integrationRule = rule; }

} // namespace Pecos

#endif