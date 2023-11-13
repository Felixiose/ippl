// -*- C++ -*-
/***************************************************************************
 *
 * The IPPL Framework
 *
 * This program was prepared by PSI.
 * All rights in the program are reserved by PSI.
 * Neither PSI nor the author(s)
 * makes any warranty, express or implied, or assumes any liability or
 * responsibility for the use of this software
 *
 * Visit www.amas.web.psi for more details
 *
 ***************************************************************************/
// Testing the inverse transform sampling method for a user defined distribution
// on a bounded domain.
//     Example:
//     srun ./TestInverseTransformSamplingCustom --overallocate 2.0 --info 10

#include <Kokkos_MathematicalConstants.hpp>
#include <Kokkos_MathematicalFunctions.hpp>
#include <Kokkos_Random.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>
#include "Utility/IpplTimings.h"
#include "Ippl.h"
#include "Random/Distribution.h"
#include "Random/NormalDistribution.h"
#include "Random/InverseTransformSampling.h"

const int Dim = 2;

using Mesh_t = ippl::UniformCartesian<double, Dim>;

const double pi    = Kokkos::numbers::pi_v<double>;

using view_type  = typename ippl::detail::ViewType<ippl::Vector<double, Dim>, 1>::view_type;

using GeneratorPool = typename Kokkos::Random_XorShift64_Pool<>;

using size_type = ippl::detail::size_type;


struct custom_cdf{
       KOKKOS_INLINE_FUNCTION double operator()(double x, unsigned int d, const double *params) const {
           if(d==0){
               return ippl::random::normal_cdf_func<double>(x, params[0], params[1]);
           }
           else{
               return x + (params[2] / params[3]) * Kokkos::sin(params[3] * x);
           }
       }
};

struct custom_pdf{
       KOKKOS_INLINE_FUNCTION double operator()(double x, unsigned int d, double const *params) const {
           if(d==0){
               return ippl::random::normal_pdf_func<double>(x, params[0], params[1]);
           }
           else{
               return  1.0 + params[2] * Kokkos::cos(params[3] * x);
           }
       }
};

struct custom_estimate{
        KOKKOS_INLINE_FUNCTION double operator()(double u, unsigned int d, double const *params) const {
            if(d==0){
                return ippl::random::normal_estimate_func<double>(u, params[0], params[1]);
            }
            else{
                return u;
            }
        }
};

KOKKOS_FUNCTION unsigned int doublefactorial(unsigned int n)
{
    if (n == 0 || n==1)
      return 1;
    return n*doublefactorial(n-2);
}

KOKKOS_FUNCTION double NormDistCentMom(double stdev, unsigned int p){
    // returns the central moment E[(x-\mu)^p] for Normal distribution function
    if(p%2==0){
        return pow(stdev, p)*doublefactorial(p-1);
    }
    else{
        return 0.;
    }
}

KOKKOS_FUNCTION void NormDistCentMoms(double stdev, const int P, double *moms){
    for(int p=1; p<P; p++){
        moms[p] = NormDistCentMom(stdev, p+1);
    }
}


double HarmDistCentMom(double a, double b, double r, int i){
  // pdf = ( 1 + a*cos(b*x) ) / Z
  // Z = \int 1 + a*cos(b*x) ) dx
  // This function returns E[x^i] = \int x^i pdf(x) dx for a harmonic pdf
  // For simplicity, we consider symmetric bounded domain where x \in [-r, r]
  // Integrals are computed analytically using Wolfram
  double Z = (2*(a*sin(b*r)+b*r))/b;
  if( i%2==1 )
    return 0.0;
  if( i==2 )
    return ( (2.*((3.*a*b*b*r*r-6.*a)*sin(b*r)+6.*a*b*r*cos(b*r)+pow(b*r,3.)))/(3.*pow(b,3.)) ) / Z;
  if( i==4 )
    return ( (2.*a*(b*b*r*r*(b*b*r*r-12.)+24.)*sin(b*r))/pow(b,5.)+(8.*a*r*(b*b*r*r-6.)*cos(b*r))/pow(b,4.)+(2.*pow(r,5.))/5. ) / Z;
  if( i == 6 )
    return ( (2.*a*(b*b*r*r*(b*b*r*r*(b*b*r*r-30.)+360.)-720.)*sin(b*r))/pow(b,7)+(12.*a*r*(b*b*r*r*(b*b*r*r-20.)+120.)*cos(b*r))/pow(b,6)+(2.*pow(r,7))/7. )/ Z;
  return 0;
}

void HarmDistCentMoms(double a, double b, double r, const int P, double *moms){
    for(int p=1; p<P; p++){
        moms[p] = HarmDistCentMom(a, b, r, p+1);
    }
}

void MomentsFromSamples(view_type position, int d, int ntotal, const int P, double *moms){
    double temp = 0.0;
    Kokkos::parallel_reduce("moments", position.extent(0),
                            KOKKOS_LAMBDA(const int i, double& valL) {
        double myVal = position(i)[d];
        valL += myVal;
    }, Kokkos::Sum<double>(temp));

    double mean = 0.0;
    MPI_Allreduce(&mean, &temp, 1, MPI_DOUBLE, MPI_SUM, ippl::Comm->getCommunicator());

    moms[0] = mean/ntotal;

    for (int p = 1; p < P; p++) {
        temp = 0.0;
        Kokkos::parallel_reduce("moments", position.extent(0),
                                KOKKOS_LAMBDA(const int i, double& valL) {
            double myVal = pow(position(i)[d] - mean, p + 1);
            valL += myVal;
        }, Kokkos::Sum<double>(temp));
        moms[p] = temp / ntotal;
    }

    double gtemp[P];
    MPI_Allreduce(moms, gtemp, P, MPI_DOUBLE, MPI_SUM, ippl::Comm->getCommunicator());

    for (int p = 1; p < P; p++) {
        gtemp[p] /= ippl::Comm->size()*(ntotal/(ntotal-1)); // Divide by the number of GPUs
    }

    for (int p = 1; p < P; p++) {
        moms[p] = gtemp[p];
    }
}

void WriteErrorInMoments(double *moms, double *moms_ref, int P){
    std::stringstream fname;
    fname << "data/error_moments_custom_dist";
    fname << ".csv";

    Inform csvout(NULL, fname.str().c_str(), Inform::APPEND);
    csvout.precision(10);
    csvout.setf(std::ios::scientific, std::ios::floatfield);
    for(int i=0; i<P; i++){
        csvout << moms_ref[i] << " " << moms[i] << " " << fabs(moms_ref[i] - moms[i]) << endl;
    }
    csvout.flush();
    ippl::Comm->barrier();
}

int main(int argc, char* argv[]) {
    ippl::initialize(argc, argv);
    {
        ippl::Vector<int, 2> nr   = {100, 100};
        size_type ntotal = 1000000;

        ippl::NDIndex<2> domain;
        for (unsigned i = 0; i < Dim; i++) {
            domain[i] = ippl::Index(nr[i]);
        }

        ippl::e_dim_tag decomp[Dim];
        for (unsigned d = 0; d < Dim; ++d) {
            decomp[d] = ippl::PARALLEL;
        }

        ippl::Vector<double, Dim> rmin   = -4.;
        ippl::Vector<double, Dim> rmax   = 4.;
        ippl::Vector<double, Dim> length = rmax - rmin;

        ippl::Vector<double, Dim> hr     = length / nr;
        ippl::Vector<double, Dim> origin = rmin;

        const bool isAllPeriodic = true;

        Mesh_t mesh(domain, hr, origin);

        ippl::FieldLayout<Dim> fl(domain, decomp, isAllPeriodic);

        ippl::detail::RegionLayout<double, Dim, Mesh_t> rlayout(fl, mesh);

        int seed = 42;
        using size_type = ippl::detail::size_type;
        GeneratorPool rand_pool64((size_type)(seed + 100 * ippl::Comm->rank()));

        // example of sampling normal/uniform in one and harmonic in another with custom functors
        const int DimP = 4; // dimension of parameters in the pdf
        const double mu = 0.5;
        const double sd = 0.5;
        double *parH = new double [DimP];
        parH[0] = mu;
        parH[1] = sd;
        parH[2] = 0.5;
        parH[3] = 2.*pi/(rmax[1]-rmin[1])*4.0;
        using DistH_t = ippl::random::Distribution<double, Dim, DimP, custom_pdf, custom_cdf, custom_estimate>;
        using samplingH_t = ippl::random::InverseTransformSampling<double, Dim, Kokkos::DefaultExecutionSpace, DistH_t>;

        DistH_t distH(parH);
        samplingH_t samplingH(distH, rmax, rmin, rlayout, ntotal);
        size_type nlocal = samplingH.getLocalNum();
        view_type positionH("positionH", nlocal);
        samplingH.generate(positionH, rand_pool64);
        
        const int P = 6; // number of moments to check, i.e. E[x^i] for i = 1,...,P
        double moms1_ref[P];
        double moms1[P];
        
        // compute error in moments of 1st dimension
        moms1_ref[0] = mu;
        NormDistCentMoms(sd, P, moms1_ref);
        MomentsFromSamples(positionH, 0, ntotal, P, moms1);
        WriteErrorInMoments(moms1, moms1_ref, P);
        
        // next, compute error in moments of 2nd dimension
        double moms2_ref[P];
        double moms2[P];
        moms2_ref[0] = 0.0;
        HarmDistCentMoms(parH[2], parH[3], rmax[1], P, moms2_ref);
        MomentsFromSamples(positionH, 1, ntotal, P, moms2);
        WriteErrorInMoments(moms2, moms2_ref, P);
    }
    ippl::finalize();
    return 0;
}

