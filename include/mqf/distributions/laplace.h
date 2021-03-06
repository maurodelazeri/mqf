#ifndef INCLUDED_MQF_DISTRIBUTIONS_LAPLACE
#define INCLUDED_MQF_DISTRIBUTIONS_LAPLACE
#include <cmath>
#include "../distribution.h"
#include "../random_variable.h"

namespace mqf {
namespace Distributions {

	struct Laplace : Density<> {
		double mu, b;
				
		explicit Laplace( double mu = 0.0, double b = 1.0 ) : mu(mu), b(b) {}

		double mean() const {
			return mu;
		}

		double median() const {
			return mu;
		}

		double mode() const {
			return mu;
		}

		double variance() const {
			return 2.0 * b * b;
		}

		double operator()( Value x ) const {
			return std::exp(-std::fabs(x-mu)/b) / (2.0*b);
		}

		double cumulative( Value x ) const {
			double y = (x - mu) / b;
			if( x < mu ) {
				return 0.5 * std::exp(y);
			}
			return 1.0 - 0.5 * std::exp(-y);
		}
	};

	RV<Laplace> operator+( const RV<Laplace>& lhs, double rhs ) {
		return MakeRV( Laplace( lhs.dist.mu + rhs, lhs.dist.b ) );
	}

	RV<Laplace> operator+( double lhs, const RV<Laplace>& rhs ) {
		return rhs + lhs;
	}

	RV<Laplace> operator*( const RV<Laplace>& lhs, double rhs ) {
		return MakeRV( Laplace( lhs.dist.mu * rhs, lhs.dist.b * rhs ) );
	}

	RV<Laplace> operator*( double lhs, const RV<Laplace>& rhs ) {
		return rhs * lhs;
	}

}

/* mle

1/N sum_i abs(x_i - mu) = b

sum_i (x_i - mu)/abs(x_i - mu) = 0 = sum_i sgn(x_i - mu)


set mu = median.

*/

	template<typename>
	struct MomentEstimation;

	template<>
	struct MomentEstimation<Distributions::Laplace> {
		using Dist = Distributions::Laplace;
		template<typename It>
		Dist operator()( It p1, It p2 ) const {
			auto mu = sampleMean(p1,p2);
			auto var = sampleVariance(p1,p2,mu);
			return Dist( mu, std::sqrt( 0.5 * var ) );
		}
	};

}

#endif
