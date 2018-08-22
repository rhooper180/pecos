#include "nested_sampling.hpp"
#include "MathTools.hpp"
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/uniform_real.hpp>
#define PI 4.0*atan(1.0)

void Sampler::generate_samples( int num_dims, int num_samples, 
				RealMatrix &samples ) const{
  RealMatrix initial_samples;
  enrich_samples( num_dims, initial_samples, num_samples, samples );
}

void Sampler::enrich_samples( int num_dims, const RealMatrix &initial_samples,
			      int num_new_samples,
			      const RealMatrix & candidate_samples,
			      RealMatrix &selected_samples ) const {

  // Remove any candidate samples already in the initial sample set
  RealMatrix unique_candidate_samples;
  get_unique_samples( initial_samples, num_new_samples, candidate_samples,
		      unique_candidate_samples );

    
  // Get indices of best candidate samples
  IntVector selected_candidate_indices;
  get_enriched_sample_indices( num_dims, initial_samples, num_new_samples, 
			       unique_candidate_samples, 
			       selected_candidate_indices );
    
  // Allocate memory for selected samples
  int num_initial_samples = initial_samples.numCols(),
    num_total_samples = num_new_samples + num_initial_samples;
  selected_samples.shapeUninitialized( num_dims, num_total_samples );

  // Copy initial samples into selected samples
  RealMatrix initial_samples_view( Teuchos::View, selected_samples,
				   num_dims, num_initial_samples );
  copy_matrix( initial_samples, initial_samples_view, num_dims, 
	       num_initial_samples );

  // Copy selected candidates into selected samples below the initial samples;
  RealMatrix candidate_samples_view( Teuchos::View, selected_samples,
				     num_dims, num_new_samples, 
				     0, num_initial_samples );
  extract_submatrix_from_column_indices( unique_candidate_samples,
					 selected_candidate_indices,
					 candidate_samples_view );
    
}

/// Remove any candidate samples that are already in an initial set of samples
void Sampler::get_unique_samples( const RealMatrix &initial_samples,
				  int num_new_samples,
				  const RealMatrix &candidate_samples,
				  RealMatrix &unique_candidate_samples ){
    
  IntVector unique_candidate_indices;
  set_difference_matrix_columns( candidate_samples, initial_samples, 
				 unique_candidate_indices );
  if ( unique_candidate_indices.length() < num_new_samples ){
    std::string msg = "get_unique_candidates. Not enough unique candidate";
    msg += " samples";
    throw( std::runtime_error( msg ) );
  }

  extract_submatrix_from_column_indices( candidate_samples,
					 unique_candidate_indices,
					 unique_candidate_samples );
}

void LejaSampler::build_basis_matrix( const RealMatrix &samples, 
				      RealMatrix &basis_matrix ) const{
  pce_.build_vandermonde( samples, basis_matrix, false, false );
}

void LejaSampler::
get_enriched_sample_indices( int num_dims, 
			     const RealMatrix &initial_samples,
			     int num_new_samples,
			     const RealMatrix & candidate_samples,
			     IntVector &selected_candidate_indices ) const
{
  // This function assumes the candidate samples are unique

  // Combine initial samples and candidate samples 
  RealMatrix samples;
  hstack( initial_samples, candidate_samples, samples );
  
  // Build the basis matrix, i.e. evaluate each basis function at each point
  RealMatrix basis_matrix;
  build_basis_matrix( samples, basis_matrix );

  if ( precondition_ )
    apply_preconditioning( basis_matrix );
  
  int num_initial_samples = initial_samples.numCols();
  int num_total_samples = num_new_samples + num_initial_samples;
  RealMatrix A, L_factor, U_factor;
  IntVector selected_sample_indices;
  truncated_pivoted_lu_factorization( basis_matrix, L_factor, U_factor, 
				      selected_sample_indices,
				      num_total_samples, num_initial_samples );
  if ( selected_sample_indices.length() < num_total_samples ) {
    std::string msg = "enrich_samples: The basis matrix has rank less than";
    msg += " num_total_samples. Try increasing the degree of the basis.";
    throw( std::runtime_error( msg ) );
  }

  // Get indices of columns to keep in candidate sample matrix
  // selected_samples_indices contains indexes into the larger
  // samples matrix which consists of both initial samples and candidate samples
  resize( selected_candidate_indices, num_new_samples );
  for ( int i=0; i<num_new_samples; i++ )
    selected_candidate_indices[i] = 
      selected_sample_indices[i+num_initial_samples] - num_initial_samples;
}

void LejaSampler::set_pce( PolynomialChaosExpansion &pce ){
  pce_.copy( pce );
}

void LejaSampler::get_candidate_samples( int num_dims, int num_samples, int seed,
					 RealMatrix &candidate_samples ){
  boost::mt19937 rng;
  if (seed)
    rng.seed(seed);
  boost::uniform_real<double> uniform_sampler(0.,PI);
  reshape( candidate_samples, num_dims, num_samples );
  for (int j=0; j<num_samples; j++)
    for (int i=0; i<num_dims; i++)
      candidate_samples(i,j) = -std::cos( uniform_sampler(rng) );
}
  
void LejaSampler::enrich_samples( int num_dims, 
				  const RealMatrix &initial_samples,
				  int num_new_samples,
				  RealMatrix &samples ) const {
  RealMatrix candidate_samples;
  get_candidate_samples( num_dims, numCandidateSamples_, seed_, 
			 candidate_samples );
  Sampler::enrich_samples( num_dims, initial_samples, num_new_samples, 
			   candidate_samples, samples );
}

void LejaSampler::set_seed( int seed ){
  seed_ = seed;
}

void LejaSampler::set_num_candidate_samples( int num_candidate_samples ){
  numCandidateSamples_ = num_candidate_samples;
}

void LejaSampler::set_precondition( bool precondition ){
  precondition_ = precondition;
}

void LejaSampler::apply_preconditioning( RealMatrix &basis_matrix ) {
  RealVector wts( basis_matrix.numRows() ); // initialize to zero
  for (int j=0; j<basis_matrix.numCols(); j++)
    for (int i=0; i<basis_matrix.numRows(); i++)
      wts[i] += basis_matrix(i,j)*basis_matrix(i,j);
  
  for (int i=0; i<basis_matrix.numRows(); i++){
    wts[i] = std::sqrt( (double)basis_matrix.numCols() / wts[i] );
    for (int j=0; j<basis_matrix.numCols(); j++)
      basis_matrix(i,j) *= wts[i];
  }  
}
