#ifndef IPPL_OPAL_PROTOTYPE_MANAGER_H
#define IPPL_OPAL_PROTOTYPE_MANAGER_H

#include <memory>

#include "FieldContainer.hpp"
#include "FieldSolver.hpp"
#include "LoadBalancer.hpp"
#include "AlpineManager.h"
#include "Manager/BaseManager.h"
#include "ParticleContainer.hpp"
#include "Random/Distribution.h"
#include "Random/InverseTransformSampling.h"
#include "Random/NormalDistribution.h"
#include "Random/Randn.h"
#include "Random/Randu.h"
#include "Random/CorrRandn.h"

using view_type = typename ippl::detail::ViewType<ippl::Vector<double, Dim>, 1>::view_type;

using Matrix_t = ippl::Vector< ippl::Vector<double, Dim>, Dim>;

template <typename T, unsigned Dim>
class OpalPrototypeManager : public AlpineManager<T, Dim> {
public:
    using ParticleContainer_t = ParticleContainer<T, Dim>;
    using FieldContainer_t = FieldContainer<T, Dim>;
    using FieldSolver_t= FieldSolver<T, Dim>;
    using LoadBalancer_t= LoadBalancer<T, Dim>;

    OpalPrototypeManager(size_type totalP_, int nt_, Vector_t<int, Dim> &nr_,
                       double lbt_, std::string& solver_, std::string& stepMethod_)
        : AlpineManager<T, Dim>(totalP_, nt_, nr_, lbt_, solver_, stepMethod_){}

    ~OpalPrototypeManager(){}

    size_type Ncount_m;
    Kokkos::Random_XorShift64_Pool<> rand_pool64_m;

    void pre_run() override {
        Inform m("Pre Run");

        if (this->solver_m == "OPEN") {
            throw IpplException("OPAL Prototype", "Open boundaries solver incompatible with this simulation!");
        }

        for (unsigned i = 0; i < Dim; i++) {
            this->domain_m[i] = ippl::Index(this->nr_m[i]);
        }

        this->decomp_m.fill(true);
        this->kw_m    = 0.5;
        this->alpha_m = 0.05;
        this->rmin_m  = 0.0;
        this->rmax_m  = 10; //2 * pi / this->kw_m;

        this->hr_m = this->rmax_m / this->nr_m;
        // Q = -\int\int f dx dv
        this->Q_m = std::reduce(this->rmax_m.begin(), this->rmax_m.end(), -1., std::multiplies<double>());
        this->origin_m = this->rmin_m;
        this->dt_m     = std::min(.05, 0.5 * *std::min_element(this->hr_m.begin(), this->hr_m.end()));
        this->it_m     = 0;
        this->time_m   = 0.0;
        this->Ncount_m = 0; // count on how many particles have already emitted

        rand_pool64_m = Kokkos::Random_XorShift64_Pool<>( (size_type)(42 + 100 * ippl::Comm->rank()) );
        
        m << "Discretization:" << endl
          << "nt " << this->nt_m << " Np= " << this->totalP_m << " grid = " << this->nr_m << endl;

        this->isAllPeriodic_m = true;

        this->setFieldContainer( std::make_shared<FieldContainer_t>( this->hr_m, this->rmin_m, this->rmax_m, this->decomp_m, this->domain_m, this->origin_m, this->isAllPeriodic_m) );

        this->setParticleContainer( std::make_shared<ParticleContainer_t>( this->fcontainer_m->getMesh(), this->fcontainer_m->getFL()) );

        this->fcontainer_m->initializeFields(this->solver_m);

        this->setFieldSolver( std::make_shared<FieldSolver_t>( this->solver_m, &this->fcontainer_m->getRho(), &this->fcontainer_m->getE(), &this->fcontainer_m->getPhi()) );

        this->fsolver_m->initSolver();

        this->setLoadBalancer( std::make_shared<LoadBalancer_t>( this->lbt_m, this->fcontainer_m, this->pcontainer_m, this->fsolver_m) );

        this->initializeParticles();

        this->fcontainer_m->getRho() = 0.0;

        this->fcontainer_m->getE() = 0.0;

        bool isFirstRepartition = true;

        this->loadbalancer_m->initializeORB(&this->fcontainer_m->getFL(), &this->fcontainer_m->getMesh());

        this->loadbalancer_m->repartition(&this->fcontainer_m->getFL(), &this->fcontainer_m->getMesh(), isFirstRepartition);

        this->dump();

        m << "Done";
    }

    void advance() override {
        if (this->stepMethod_m == "LeapFrog") {
            LeapFrogStep();
        }
	else{
            throw IpplException(TestName, "Step method is not set/recognized!");
        }
    }

    double FlatTop(double ti, double A, double tR, double tF, double sigR, double sigF, double Tend){
        if(ti<tR)
            return A*exp( -pow((ti-tR)/sigR,2)/2. );
        else if( ti<tF)
            return A;
        else if(ti<Tend)
            return A*exp( -pow((ti-tF)/sigF,2)/2. );
        else
            return 0.;
    }

    size_type countInflowParticles(){
        double A = 1.0;
        double Tend = 10.*this->dt_m;
        double tR = Tend/4.; // time at which the rise ends
        double tF = 3.*Tend/4.; // time at which the fall starts
        double sigR = tR/5.;
        double sigF = (Tend-tF)/5.;

        double y0 =  FlatTop(this->time_m, A, tR, tF, sigR, sigF, Tend);
        double y1 =  FlatTop(this->time_m+this->dt_m, A, tR, tF, sigR, sigF, Tend);
        double IntInTime = 0.5*A*( pow(2.*pi*sigR*sigR, 0.5) + pow(2.*pi*sigF*sigF, 0.5) ) + A*(tF-tR);
        double da = 0.5*(y0+y1)*this->dt_m; // trapezoidal rule

        return (size_type) ( da/IntInTime*this->totalP_m/ippl::Comm->size() ); // return how many particles are emitted during dt
    }

    void initializeParticles(){
        Inform m("Initialize Particles");

        size_type nlocal = this->totalP_m/ippl::Comm->size();
        this->pcontainer_m->create(nlocal);

        auto &R = this->pcontainer_m->R.getView();
        auto &P = this->pcontainer_m->P.getView();
        this->pcontainer_m->q = this->Q_m/this->totalP_m;

        Kokkos::Random_XorShift64_Pool<> rand_pool64 = rand_pool64_m;

        ippl::detail::RegionLayout<double, Dim, Mesh_t<Dim>> rlayout;

        auto *mesh = &this->fcontainer_m->getMesh();
        auto *FL = &this->fcontainer_m->getFL();

        rlayout = ippl::detail::RegionLayout<double, Dim, Mesh_t<Dim>>( *FL, *mesh );

        const double par[6] = {1., 1., 0., 1., 0., 1.};
        using Dist_t = ippl::random::NormalDistribution<double, Dim>;
        using sampling_t = ippl::random::InverseTransformSampling<double, Dim, Kokkos::DefaultExecutionSpace, Dist_t>;

        Dist_t dist(par);
        Vector_t<double, Dim> vmin, vmax;
        vmin = -5;
        vmax = 5.;
        vmin[0] = 0.0;// make sure the velocity in 0th dimension is positive

        sampling_t samplingP(dist, vmax, vmin, rlayout, nlocal);
        samplingP.setLocalSamplesNum(nlocal);
        samplingP.generate(P, rand_pool64);

       // for position, sample normally distributed in x1,x2 and x0=x_wall=0
        Vector_t<double, Dim> origin = this->origin_m;
        Vector_t<double, Dim> mid = this->origin_m + 0.5*(this->rmax_m-this->rmin_m);

        Kokkos::parallel_for(Kokkos::RangePolicy<int>(0, nlocal), KOKKOS_LAMBDA(const size_t i) {
            for(unsigned int d=0; d<Dim; d++){
                if(d==0){
                    R(i)[d] = origin[d];
                }
                else{
                    ippl::random::randn<double, Dim>(R, rand_pool64)(i,d);
                    R(i)[d] += mid[d];
                }
            }
	});

        this->pcontainer_m->setLocalNum(0);
    }

    void GenEntParticles(){
        Inform m("Sample inflow particles");

        size_type Np = countInflowParticles(); // number of particles to be emitted per processor
        size_type Ncount = this->Ncount_m;

        // update number of active particles
        this->pcontainer_m->setLocalNum(Ncount+Np);

        this->Ncount_m += Np;
        
        size_type gNcount = 0;
        size_type gNp = 0;

        MPI_Allreduce(&this->Ncount_m, &gNcount, 1, MPI_UNSIGNED_LONG, MPI_SUM,
                          ippl::Comm->getCommunicator());

        MPI_Allreduce(&Np, &gNp, 1, MPI_UNSIGNED_LONG, MPI_SUM,
                          ippl::Comm->getCommunicator());

        m << "Sampled " << gNp << "/" << this->totalP_m << ", So far %" << 100.*gNcount/(1.*this->totalP_m) << " done" << endl;
    }

    void LeapFrogStep(){
        // LeapFrog time stepping https://en.wikipedia.org/wiki/Leapfrog_integration
        // Here, we assume a constant charge-to-mass ratio of -1 for
        // all the particles hence eliminating the need to store mass as
        // an attribute
        static IpplTimings::TimerRef PTimer           = IpplTimings::getTimer("pushVelocity");
        static IpplTimings::TimerRef RTimer           = IpplTimings::getTimer("pushPosition");
        static IpplTimings::TimerRef updateTimer      = IpplTimings::getTimer("update");
        static IpplTimings::TimerRef domainDecomposition = IpplTimings::getTimer("loadBalance");
        static IpplTimings::TimerRef SolveTimer       = IpplTimings::getTimer("solve");

        double dt                               = this->dt_m;
        std::shared_ptr<ParticleContainer_t> pc = this->pcontainer_m;
        std::shared_ptr<FieldContainer_t> fc    = this->fcontainer_m;

        // generate particles that enter domain
        GenEntParticles();

        IpplTimings::startTimer(PTimer);
        pc->P = pc->P - 0.5 * dt * pc->E;
        IpplTimings::stopTimer(PTimer);

        // drift
        IpplTimings::startTimer(RTimer);
        pc->R = pc->R + dt * pc->P;
        IpplTimings::stopTimer(RTimer);

        // Since the particles have moved spatially update them to correct processors
        IpplTimings::startTimer(updateTimer);
        pc->update();
        IpplTimings::stopTimer(updateTimer);

        size_type totalP        = this->totalP_m;
        int it                  = this->it_m;
        bool isFirstRepartition = false;

        if (this->loadbalancer_m->balance(totalP, it + 1)) {
                IpplTimings::startTimer(domainDecomposition);
                auto* mesh = &fc->getRho().get_mesh();
                auto* FL = &fc->getFL();
                this->loadbalancer_m->repartition(FL, mesh, isFirstRepartition);
                IpplTimings::stopTimer(domainDecomposition);
        }

        // scatter the charge onto the underlying grid
        this->par2grid();

        // Field solve
        IpplTimings::startTimer(SolveTimer);
        this->fsolver_m->runSolver();
        IpplTimings::stopTimer(SolveTimer);

        // gather E field
        this->grid2par();

        // kick
        IpplTimings::startTimer(PTimer);
        pc->P = pc->P - 0.5 * dt * pc->E;
        IpplTimings::stopTimer(PTimer);
    }

    void dump() override {
        static IpplTimings::TimerRef dumpDataTimer = IpplTimings::getTimer("dumpData");
        IpplTimings::startTimer(dumpDataTimer);
        dumpOpal(this->fcontainer_m->getE().getView());
        IpplTimings::stopTimer(dumpDataTimer);
    }

    template <typename View>
    void dumpOpal(const View& Eview) {
        const int nghostE = this->fcontainer_m->getE().getNghost();

        using index_array_type = typename ippl::RangePolicy<Dim>::index_array_type;
        double localEx2 = 0, localExNorm = 0;
        ippl::parallel_reduce(
            "Ex stats", ippl::getRangePolicy(Eview, nghostE),
            KOKKOS_LAMBDA(const index_array_type& args, double& E2, double& ENorm) {
                // ippl::apply<unsigned> accesses the view at the given indices and obtains a
                // reference; see src/Expression/IpplOperations.h
                double val = ippl::apply(Eview, args)[0];
                double e2  = Kokkos::pow(val, 2);
                E2 += e2;

                double norm = Kokkos::fabs(ippl::apply(Eview, args)[0]);
                if (norm > ENorm) {
                    ENorm = norm;
                }
            },
            Kokkos::Sum<double>(localEx2), Kokkos::Max<double>(localExNorm));

        double globaltemp = 0.0;
        ippl::Comm->reduce(localEx2, globaltemp, 1, std::plus<double>());

        double fieldEnergy =
            std::reduce(this->fcontainer_m->getHr().begin(), this->fcontainer_m->getHr().end(), globaltemp, std::multiplies<double>());

        double ExAmp = 0.0;
        ippl::Comm->reduce(localExNorm, ExAmp, 1, std::greater<double>());

        if (ippl::Comm->rank() == 0) {
            std::stringstream fname;
            fname << "data/FieldOpal_";
            fname << ippl::Comm->size();
            fname << "_manager";
            fname << ".csv";
            Inform csvout(NULL, fname.str().c_str(), Inform::APPEND);
            csvout.precision(16);
            csvout.setf(std::ios::scientific, std::ios::floatfield);
            if ( std::fabs(this->time_m) < 1e-14 ) {
                csvout << "time, Ex_field_energy, Ex_max_norm" << endl;
            }
            csvout << this->time_m << " " << fieldEnergy << " " << ExAmp << endl;
        }
        ippl::Comm->barrier();
    }
};
#endif
