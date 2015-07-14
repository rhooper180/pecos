/*  _______________________________________________________________________

    PECOS: Parallel Environment for Creation Of Stochastics
    Copyright (c) 2011, Sandia National Laboratories.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Pecos directory.
    _______________________________________________________________________ */

//- Class:	 NegBinomialRandomVariable
//- Description: Encapsulates random variable data and utilities
//- Owner:       Mike Eldred
//- Revised by:  
//- Version:

#ifndef NEG_BINOMIAL_RANDOM_VARIABLE_HPP
#define NEG_BINOMIAL_RANDOM_VARIABLE_HPP

#include "RandomVariable.hpp"

namespace Pecos {


/// Derived random variable class for binomial random variables.

/** Manages alpha and beta parameters. */

class NegBinomialRandomVariable: public RandomVariable
{
public:

  //
  //- Heading: Constructors and destructor
  //

  /// default constructor
  NegBinomialRandomVariable();
  /// alternate constructor
  NegBinomialRandomVariable(int num_trials, Real prob_per_trial);
  /// destructor
  ~NegBinomialRandomVariable();

  //
  //- Heading: Virtual function redefinitions
  //

  Real cdf(Real x) const;
  Real ccdf(Real x) const;
  Real inverse_cdf(Real p_cdf) const;
  Real inverse_ccdf(Real p_ccdf) const;

  Real pdf(Real x) const;

  Real parameter(short dist_param) const;
  void parameter(short dist_param, Real val);

  RealRealPair moments() const;
  RealRealPair bounds() const;

  //
  //- Heading: Member functions
  //

  void update(int num_trials, Real prob_per_trial);

  //
  //- Heading: Static member functions (global utilities)
  //

  static Real pdf(Real x, int num_trials, Real prob_per_trial);
  static Real cdf(Real x, int num_trials, Real prob_per_trial);

  static void moments_from_params(int num_trials, Real prob_per_trial,
				  Real& mean, Real& std_dev);

protected:

  //
  //- Heading: Data
  //

  /// r parameter of negative binomial random variable
  int numTrials;
  /// p parameter of negative binomial random variable
  Real probPerTrial;

  /// pointer to the Boost negative_binomial_distribution instance
  negative_binomial_dist* negBinomialDist;
};


inline NegBinomialRandomVariable::NegBinomialRandomVariable():
  RandomVariable(BaseConstructor()), numTrials(1), probPerTrial(1.),
  negBinomialDist(NULL)
{ }


inline NegBinomialRandomVariable::
NegBinomialRandomVariable(int num_trials, Real prob_per_trial):
  RandomVariable(BaseConstructor()),
  numTrials(num_trials), probPerTrial(prob_per_trial),
  negBinomialDist(new negative_binomial_dist((Real)num_trials, prob_per_trial))
{ }


inline NegBinomialRandomVariable::~NegBinomialRandomVariable()
{ }


inline Real NegBinomialRandomVariable::cdf(Real x) const
{ return bmth::cdf(*negBinomialDist, x); }


inline Real NegBinomialRandomVariable::ccdf(Real x) const
{ return bmth::cdf(complement(*negBinomialDist, x)); }


inline Real NegBinomialRandomVariable::inverse_cdf(Real p_cdf) const
{ return bmth::quantile(*negBinomialDist, p_cdf); }


inline Real NegBinomialRandomVariable::inverse_ccdf(Real p_ccdf) const
{ return bmth::quantile(complement(*negBinomialDist, p_ccdf)); }


inline Real NegBinomialRandomVariable::pdf(Real x) const
{ return bmth::pdf(*negBinomialDist, x); }


inline Real NegBinomialRandomVariable::parameter(short dist_param) const
{
  switch (dist_param) {
  case NBI_TRIALS:      return (Real)numTrials; break;
  case NBI_P_PER_TRIAL: return probPerTrial;    break;
  default:
    PCerr << "Error: update failure for distribution parameter " << dist_param
	  << " in NegBinomialRandomVariable::parameter()." << std::endl;
    abort_handler(-1); return 0.; break;
  }
}


inline void NegBinomialRandomVariable::parameter(short dist_param, Real val)
{
  switch (dist_param) {
  case NBI_TRIALS:      numTrials = (int)val; break;
  case NBI_P_PER_TRIAL: probPerTrial = val;   break;
  default:
    PCerr << "Error: update failure for distribution parameter " << dist_param
	  << " in NegBinomialRandomVariable::parameter()." << std::endl;
    abort_handler(-1); break;
  }
}


inline RealRealPair NegBinomialRandomVariable::moments() const
{
  Real mean, std_dev;
  moments_from_params(numTrials, probPerTrial, mean, std_dev);
  return RealRealPair(mean, std_dev);
}


inline RealRealPair NegBinomialRandomVariable::bounds() const
{ return RealRealPair(0., std::numeric_limits<Real>::infinity()); }


inline void NegBinomialRandomVariable::
update(int num_trials, Real prob_per_trial)
{
  if (!negBinomialDist ||
      numTrials != num_trials || probPerTrial != prob_per_trial) {
    numTrials = num_trials; probPerTrial = prob_per_trial;
    if (negBinomialDist) delete negBinomialDist;
    negBinomialDist = new negative_binomial_dist((Real)numTrials, probPerTrial);
  }
}

// Static member functions:

inline Real NegBinomialRandomVariable::
pdf(Real x, int num_trials, Real prob_per_trial)
{
  negative_binomial_dist neg_binomial1((Real)num_trials, prob_per_trial);
  return bmth::pdf(neg_binomial1, x);
}


inline Real NegBinomialRandomVariable::
cdf(Real x, int num_trials, Real prob_per_trial)
{
  negative_binomial_dist neg_binomial1((Real)num_trials, prob_per_trial);
  return bmth::cdf(neg_binomial1, x);
}


inline void NegBinomialRandomVariable::
moments_from_params(int num_trials, Real prob_per_trial,
		    Real& mean, Real& std_dev)
{
  Real n1mp = (Real)num_trials * (1. - prob_per_trial);
  mean = n1mp / prob_per_trial; std_dev = std::sqrt(n1mp) / prob_per_trial;
}

} // namespace Pecos

#endif
