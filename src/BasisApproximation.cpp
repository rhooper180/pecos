/*  _______________________________________________________________________

    PECOS: Parallel Environment for Creation Of Stochastics
    Copyright (c) 2008, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Pecos directory.
    _______________________________________________________________________ */

#include "BasisApproximation.hpp"
#include "InterpPolyApproximation.hpp"
#include "OrthogPolyApproximation.hpp"

static const char rcsId[]="@(#) $Id: BasisApproximation.cpp 4768 2007-12-17 17:49:32Z mseldre $";

namespace Pecos {


/** This constructor is the one which must build the base class data
    for all derived classes.  get_basis_approx() instantiates a derived
    class letter and the derived constructor selects this base class
    constructor in its initialization list (to avoid recursion in the
    base class constructor calling get_basis_approx() again).  Since the
    letter IS the representation, its rep pointer is set to NULL (an
    uninitialized pointer causes problems in ~BasisApproximation). */
BasisApproximation::BasisApproximation(BaseConstructor, size_t num_vars):
  numVars(num_vars), basisApproxRep(NULL), referenceCount(1)
{
#ifdef REFCOUNT_DEBUG
  PCout << "BasisApproximation::BasisApproximation(BaseConstructor) called to "
        << "build base class for letter." << std::endl;
#endif
}


/** The default constructor: basisApproxRep is NULL in this case.  This
    makes it necessary to check for NULL in the copy constructor,
    assignment operator, and destructor. */
BasisApproximation::BasisApproximation():
  basisApproxRep(NULL), referenceCount(1)
{
#ifdef REFCOUNT_DEBUG
  PCout << "BasisApproximation::BasisApproximation() called to build empty "
        << "envelope." << std::endl;
#endif
}


/** Envelope constructor only needs to extract enough data to properly
    execute get_basis_approx, since BasisApproximation(BaseConstructor)
    builds the actual base class data for the derived basis functions. */
BasisApproximation::
BasisApproximation(const String& basis_approx_type, size_t num_vars):
  referenceCount(1)
{
#ifdef REFCOUNT_DEBUG
  PCout << "BasisApproximation::BasisApproximation(string&) called to "
        << "instantiate envelope." << std::endl;
#endif

  // Set the rep pointer to the appropriate derived type
  basisApproxRep = get_basis_approx(basis_approx_type, num_vars);
  if ( !basisApproxRep ) // bad type or insufficient memory
    abort_handler(-1);
}


/** Used only by the envelope constructor to initialize basisApproxRep to the 
    appropriate derived type. */
BasisApproximation* BasisApproximation::
get_basis_approx(const String& basis_approx_type, size_t num_vars)
{
#ifdef REFCOUNT_DEBUG
  PCout << "Envelope instantiating letter in get_basis_approx(string&)."
        << std::endl;
#endif

  if (basis_approx_type == "fourier")
    return NULL;//new FourierBasisApproximation(num_vars);
  else if (basis_approx_type == "eigen")
    return NULL;//new SVDLeftEigenBasisApproximation(num_vars);
  else if (basis_approx_type == "global_interpolation_polynomial")
    return new InterpPolyApproximation(num_vars);
  else if (basis_approx_type == "global_orthogonal_polynomial")
    return new OrthogPolyApproximation(num_vars);
  else {
    PCerr << "Error: BasisApproximation type " << basis_approx_type
	  << " not available." << std::endl;
    return NULL;
  }
}


/** Copy constructor manages sharing of basisApproxRep and incrementing
    of referenceCount. */
BasisApproximation::BasisApproximation(const BasisApproximation& basis_approx)
{
  // Increment new (no old to decrement)
  basisApproxRep = basis_approx.basisApproxRep;
  if (basisApproxRep) // Check for an assignment of NULL
    basisApproxRep->referenceCount++;

#ifdef REFCOUNT_DEBUG
  PCout << "BasisApproximation::BasisApproximation(BasisApproximation&)"
	<< std::endl;
  if (basisApproxRep)
    PCout << "basisApproxRep referenceCount = "
	  << basisApproxRep->referenceCount << std::endl;
#endif
}


/** Assignment operator decrements referenceCount for old basisApproxRep,
    assigns new basisApproxRep, and increments referenceCount for new
    basisApproxRep. */
BasisApproximation BasisApproximation::
operator=(const BasisApproximation& basis_approx)
{
  if (basisApproxRep != basis_approx.basisApproxRep) { // std case: old != new
    // Decrement old
    if (basisApproxRep) // Check for null pointer
      if (--basisApproxRep->referenceCount == 0) 
	delete basisApproxRep;
    // Assign and increment new
    basisApproxRep = basis_approx.basisApproxRep;
    if (basisApproxRep) // Check for an assignment of NULL
      basisApproxRep->referenceCount++;
  }
  // else if assigning same rep, then do nothing since referenceCount
  // should already be correct

#ifdef REFCOUNT_DEBUG
  PCout << "BasisApproximation::operator=(BasisApproximation&)" << std::endl;
  if (basisApproxRep)
    PCout << "basisApproxRep referenceCount = "
	  << basisApproxRep->referenceCount << std::endl;
#endif

  return *this; // calls copy constructor since returned by value
}


/** Destructor decrements referenceCount and only deletes basisApproxRep
    when referenceCount reaches zero. */
BasisApproximation::~BasisApproximation()
{ 
  // Check for NULL pointer 
  if (basisApproxRep) {
    --basisApproxRep->referenceCount;
#ifdef REFCOUNT_DEBUG
    PCout << "basisApproxRep referenceCount decremented to "
	  << basisApproxRep->referenceCount << std::endl;
#endif
    if (basisApproxRep->referenceCount == 0) {
#ifdef REFCOUNT_DEBUG
      PCout << "deleting basisApproxRep" << std::endl;
#endif
      delete basisApproxRep;
    }
  }
}


const Real& BasisApproximation::get_value(const RealVector& x)
{
  if (!basisApproxRep) {
    PCerr << "Error: get_value() not available for this basis approximation "
	  << "type." << std::endl;
    abort_handler(-1);
  }

  return basisApproxRep->get_value(x);
}


const RealVector& BasisApproximation::get_gradient(const RealVector& x)
{
  if (!basisApproxRep) {
    PCerr << "Error: get_gradient() not available for this basis approximation "
	  << "type." << std::endl;
    abort_handler(-1);
  }

  return basisApproxRep->get_gradient(x);
}


const RealSymMatrix& BasisApproximation::get_hessian(const RealVector& x)
{
  if (!basisApproxRep) {
    PCerr << "Error: get_hessian() not available for this basis approximation "
	  << "type." << std::endl;
    abort_handler(-1);
  }
    
  return basisApproxRep->get_hessian(x);
}


int BasisApproximation::min_coefficients() const
{
  if (!basisApproxRep) { // no default implementation
    PCerr << "Error: min_coefficients() not defined for this basis "
	  << "approximation type." << std::endl;
    abort_handler(-1);
  }

  return basisApproxRep->min_coefficients(); // fwd to letter
}


void BasisApproximation::find_coefficients()
{
  if (basisApproxRep)
    basisApproxRep->find_coefficients(); 
  else {
    PCerr << "Error: find_coefficients() not available for this basis "
	  << "approximation type." << std::endl;
    abort_handler(-1);
  }
}


void BasisApproximation::print_coefficients(std::ostream& s) const
{
  if (basisApproxRep)
    basisApproxRep->print_coefficients(s);
  else {
    PCerr << "Error: print_coefficients() not available for this basis "
	  << "approximation type." << std::endl;
    abort_handler(-1);
  }
}


const RealVector& BasisApproximation::approximation_coefficients() const
{
  if (!basisApproxRep) {
    PCerr << "Error: approximation_coefficients() not available for this "
	  << "basis approximation type." << std::endl;
    abort_handler(-1);
  }
   
  return basisApproxRep->approximation_coefficients(); // fwd to letter
}


void BasisApproximation::
approximation_coefficients(const RealVector& approx_coeffs)
{
  if (basisApproxRep)
    basisApproxRep->approximation_coefficients(approx_coeffs); // fwd to letter
  else {
    PCerr << "Error: approximation_coefficients() not available for this "
	  << "basis approximation type." << std::endl;
    abort_handler(-1);
  }
}

} // namespace Pecos