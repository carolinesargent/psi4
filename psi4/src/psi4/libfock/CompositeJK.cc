/*
 * @BEGIN LICENSE
 *
 * Psi4: an open-source quantum chemistry software package
 *
 * Copyright (c) 2007-2023 The Psi4 Developers.
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This file is part of Psi4.
 *
 * Psi4 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * Psi4 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along
 * with Psi4; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * @END LICENSE
 */

#include "jk.h"
#include "psi4/libqt/qt.h"
#include "psi4/libfock/cubature.h"
#include "psi4/libfock/points.h"
#include "psi4/libmints/vector.h"
#include "psi4/libmints/basisset.h"
#include "psi4/libmints/electrostatic.h"
#include "psi4/libmints/mintshelper.h"
#include "psi4/libmints/molecule.h"
#include "psi4/libmints/integral.h"
#include "psi4/liboptions/liboptions.h"
#include "psi4/lib3index/dftensor.h"

#include <unordered_set>
#include <vector>
#include <map>
#include <algorithm>
#ifdef _OPENMP
#include <omp.h>
#endif

using namespace psi;

namespace psi {

Matrix compute_numeric_overlap(const DFTGrid &grid, const std::shared_ptr<BasisSet> &primary) {

    // DOI 10.1063/1.3646921, EQ. 9

    // note that the S_num matrix is defined slightly differently in our code
    // to account for the possibility of negative grid weights
    // here, we use S_num = X_sign*(X_nosign)^T
    // where:
    //   1. X_nosign uses sqrt(abs(w)) instead of sqrt(w) for the X matrix
    //   2. X_sign uses sign(w) * sqrt(abs(w)), where sign returns the sign of w, instead of sqrt(w) for the X matrix

    int nbf = primary->nbf();
    BasisFunctions bf_computer(primary, grid.max_points(), grid.max_functions());
    Matrix S_num("Numerical Overlap", nbf, nbf);
    auto S_nump = S_num.pointer();

    // This loop could be parallelized over blocks of grid points. However, the cost of the loop is
    // so small (< 10 seconds for a 200 heavy atom system), parallelism isn't necessary

    for (const auto &block : grid.blocks()) {

        // grid points in this block
        int npoints_block = block->npoints();
        int nbf_block = block->local_nbf();
        auto w = block->w();

        // compute basis functions at these grid points
        bf_computer.compute_functions(block);
        auto point_values = bf_computer.basis_values()["PHI"];

        // lambda for returning sign of double
        auto sign = [ ](double val) {
            return (val >= 0.0) ? 1.0 : -1.0;
        };

        // resize the buffer of basis function values
        Matrix X_block_nosign("phi_g,u", npoints_block, nbf_block);  // points x nbf_block
        Matrix X_block_sign("phi_g,u", npoints_block, nbf_block);  // points x nbf_block

        auto X_block_nosignp = X_block_nosign.pointer();
        auto X_block_signp = X_block_sign.pointer();
        for (size_t p = 0; p < npoints_block; p++) {
            for (size_t k = 0; k < nbf_block; k++) {
                X_block_nosignp[p][k] = point_values->get(p, k) * std::sqrt(std::abs(w[p]));
                X_block_signp[p][k] = sign(w[p])*X_block_nosignp[p][k];
            }
        }

        // significant basis functions at these grid points
        const auto &bf_map = block->functions_local_to_global();

        auto S_num_block = linalg::doublet(X_block_sign, X_block_nosign, true, false);
        auto S_num_blockp = S_num_block.pointer();

        for (size_t mu_local = 0; mu_local < nbf_block; mu_local++) {
            size_t mu = bf_map[mu_local];
            for (size_t nu_local = 0; nu_local < nbf_block; nu_local++) {
                size_t nu = bf_map[nu_local];
                S_nump[mu][nu] += S_num_blockp[mu_local][nu_local];
            }
        }

    }

    S_num.hermitivitize();

    return S_num;

}

Matrix compute_esp_bound(const BasisSet &primary) {

    // DOI 10.1016/j.chemphys.2008.10.036, EQ. 20
    // This is a pretty loose ESP bound, which should eventually be swapped out for something tighter
    // The bound is also only based on the overlap between the basis functions, not the distance
    // between the basis functions and the grid point.

    int nshell = primary.nshell();

    Matrix esp_bound("Shell Integral Bound", nshell, nshell);
    auto esp_boundp = esp_bound.pointer();

    auto dist = primary.molecule()->distance_matrix();
    auto distp = dist.pointer();

    for (size_t s1=0; s1 < nshell; s1++) {
        int c1 = primary.shell_to_center(s1);
        int np1 = primary.shell(s1).nprimitive();
        for (size_t s2=0; s2 < nshell; s2++) {
            int c2 = primary.shell_to_center(s2);
            int np2 = primary.shell(s2).nprimitive();

            double r2 = distp[c1][c2] * distp[c1][c2] ;
            for(size_t pi1 = 0; pi1 < np1; pi1++) {
                for(size_t pi2 = 0; pi2 < np2; pi2++) {
                    double exp1 = primary.shell(s1).exp(pi1);
                    double exp2 = primary.shell(s2).exp(pi2);
                    double coef1 = primary.shell(s1).coef(pi1);
                    double coef2 = primary.shell(s2).coef(pi2);
                    esp_boundp[s1][s2] += coef1 * coef2 * std::exp(-1 * r2 * exp1 * exp2 / (exp1 + exp2)) * 2 * M_PI / (exp1 + exp2);
                }
            }

            esp_boundp[s1][s2] = std::abs(esp_boundp[s1][s2]);
        }
    }

    return esp_bound;

}
CompositeJK::CompositeJK(std::shared_ptr<BasisSet> primary, std::shared_ptr<BasisSet> auxiliary, Options& options) : JK(primary), auxiliary_(auxiliary), options_(options) {
    timer_on("CompositeJK: Setup");
    common_init(); 
    timer_off("CompositeJK: Setup");
}

CompositeJK::~CompositeJK() {}

void CompositeJK::common_init() {

    // => General Setup <= //

    // thread count
    nthreads_ = 1;
#ifdef _OPENMP
    nthreads_ = Process::environment.get_n_threads();
#endif

    // incremental Fock build
    incfock_ = options_.get_bool("INCFOCK");
    incfock_count_ = 0;
    do_incfock_iter_ = false;
    if (options_.get_int("INCFOCK_FULL_FOCK_EVERY") <= 0) {
        throw PSIEXCEPTION("Invalid input for option INCFOCK_FULL_FOCK_EVERY (<= 0)");
    }

    computed_shells_per_iter_["Quartets"] = {};
    
    // derive separate J+K algorithms from scf_type
    auto jk_type = options_.get_str("SCF_TYPE");
    j_type_ = jk_type.substr(0, jk_type.find("+"));
    k_type_ = jk_type.substr(jk_type.find("+") + 1, jk_type.length());

    // occurs if no composite K algorithm was specified; useful for LDA/GGA DFT runs
    if (k_type_ == j_type_) {
      k_type_ = "NONE";
    }

    // other options
    density_screening_ = options_.get_str("SCREENING") == "DENSITY";
    set_cutoff(options_.get_double("INTS_TOLERANCE"));
    early_screening_ = k_type_ == "COSX" ? true : false;

    // pre-construct per-thread TwoBodyAOInt objects for computing 3- and 4-index ERIs
    timer_on("CompositeJK: ERI Computers");
    
    auto zero = BasisSet::zero_ao_basis_set();

    // initialize 4-Center ERIs
    eri_computers_["4-Center"].emplace({});
    eri_computers_["4-Center"].resize(nthreads_);

    IntegralFactory factory(primary_, primary_, primary_, primary_);
    eri_computers_["4-Center"][0] = std::shared_ptr<TwoBodyAOInt>(factory.eri());

    // initialize 3-Center ERIs
    eri_computers_["3-Center"].emplace({});
    eri_computers_["3-Center"].resize(nthreads_);

    IntegralFactory rifactory(auxiliary_, zero, primary_, primary_);
    eri_computers_["3-Center"][0] = std::shared_ptr<TwoBodyAOInt>(rifactory.eri());

    // create each threads' ERI computers
    for(int rank = 1; rank < nthreads_; rank++) {
        eri_computers_["4-Center"][rank] = std::shared_ptr<TwoBodyAOInt>(eri_computers_["4-Center"].front()->clone());
        eri_computers_["3-Center"][rank] = std::shared_ptr<TwoBodyAOInt>(eri_computers_["3-Center"].front()->clone());
    }

    timer_off("CompositeJK: ERI Computers");

    // => Set up separate J algorithm <= //

    // Direct DF-J
    if (j_type_ == "DFDIRJ") {
        // pre-compute coulomb fitting metric
        timer_on("CompositeJK: DFDIRJ Coulomb Metric");

        FittingMetric J_metric_obj(auxiliary_, true);
        J_metric_obj.form_fitting_metric();
        J_metric_ = J_metric_obj.get_metric();

        computed_shells_per_iter_["Triplets"] = {};
        
        timer_off("CompositeJK: DFDIRJ Coulomb Metric");
    } else {
        throw PSIEXCEPTION("Invalid Composite J algorithm selected!");
    }

    // => Set up separate K algorithm <= //

    // Linear Exchange (LinK)
    if (k_type_ == "LINK") {
        // set up LinK integral tolerance
        if (options_["LINK_INTS_TOLERANCE"].has_changed()) {
            linK_ints_cutoff_ = options_.get_double("LINK_INTS_TOLERANCE");
        } else {
            linK_ints_cutoff_ = cutoff_;
        }
    
    // Chain-of-Spheres Exchange (COSX)
    } else if (k_type_ == "COSX") {
        timer_on("CompositeJK: COSX Grid Construction");

        // TODO: specify bool "DFT_REMOVE_DISTANT_POINTS" in the DFTGrid constructors

        // Create a small DFTGrid for the initial SCF iterations
        std::map<std::string, std::string> grid_init_str_options = {
            {"DFT_PRUNING_SCHEME", options_.get_str("COSX_PRUNING_SCHEME")},
            {"DFT_RADIAL_SCHEME",  "TREUTLER"},
            {"DFT_NUCLEAR_SCHEME", "TREUTLER"},
            {"DFT_GRID_NAME",      ""},
            {"DFT_BLOCK_SCHEME",   "OCTREE"},
        };
        std::map<std::string, int> grid_init_int_options = {
            {"DFT_SPHERICAL_POINTS", options_.get_int("COSX_SPHERICAL_POINTS_INITIAL")},
            {"DFT_RADIAL_POINTS",    options_.get_int("COSX_RADIAL_POINTS_INITIAL")},
            {"DFT_BLOCK_MIN_POINTS", 100},
            {"DFT_BLOCK_MAX_POINTS", 256},
        };
        std::map<std::string, double> grid_init_float_options = {
            {"DFT_BASIS_TOLERANCE",   options_.get_double("COSX_BASIS_TOLERANCE")},
            {"DFT_BS_RADIUS_ALPHA",   1.0},
            {"DFT_PRUNING_ALPHA",     1.0},
            {"DFT_BLOCK_MAX_RADIUS",  3.0},
            {"DFT_WEIGHTS_TOLERANCE", 1e-15},
        };
        grid_init_ = std::make_shared<DFTGrid>(primary_->molecule(), primary_, grid_init_int_options, grid_init_str_options, grid_init_float_options, options_);

        // Create a large DFTGrid for the final SCF iteration
        std::map<std::string, std::string> grid_final_str_options = {
            {"DFT_PRUNING_SCHEME", options_.get_str("COSX_PRUNING_SCHEME")},
            {"DFT_RADIAL_SCHEME",  "TREUTLER"},
            {"DFT_NUCLEAR_SCHEME", "TREUTLER"},
            {"DFT_GRID_NAME",      ""},
            {"DFT_BLOCK_SCHEME",   "OCTREE"},
        };
        std::map<std::string, int> grid_final_int_options = {
            {"DFT_SPHERICAL_POINTS", options_.get_int("COSX_SPHERICAL_POINTS_FINAL")},
            {"DFT_RADIAL_POINTS",    options_.get_int("COSX_RADIAL_POINTS_FINAL")},
            {"DFT_BLOCK_MIN_POINTS", 100},
            {"DFT_BLOCK_MAX_POINTS", 256},
        };
        std::map<std::string, double> grid_final_float_options = {
            {"DFT_BASIS_TOLERANCE",   options_.get_double("COSX_BASIS_TOLERANCE")},
            {"DFT_BS_RADIUS_ALPHA",   1.0},
            {"DFT_PRUNING_ALPHA",     1.0},
            {"DFT_BLOCK_MAX_RADIUS",  3.0},
            {"DFT_WEIGHTS_TOLERANCE", 1e-15},
        };
        grid_final_ = std::make_shared<DFTGrid>(primary_->molecule(), primary_, grid_final_int_options, grid_final_str_options, grid_final_float_options, options_);

        // Print out warning if grid with negative grid weights is used 
        // Original Nesse COSX formulation does not support negative grid weights
        // which can happen with certain grid configurations
        // the Psi4 COSX implementation is slightly modified to work with negative grid weights
        // See https://github.com/psi4/psi4/issues/2890 for discussion
        auto warning_printed_init = false;
        for (const auto &init_block : grid_init_->blocks()) {
            const auto w = init_block->w();
            for (int ipoint = 0; ipoint < init_block->npoints(); ++ipoint) {
                if (w[ipoint] < 0.0) {
	                outfile->Printf("  INFO: The definition of the current initial grid includes negative weights, which the original COSX formulation does not support!\n    If this is of concern, please choose another initial grid through adjusting either COSX_PRUNING_SCHEME or COSX_SPHERICAL_POINTS_INITIAL.\n\n");
	                warning_printed_init = true;
		            break;
	            }
            }
	        if (warning_printed_init) break;
        }

        auto warning_printed_final = false;
        for (const auto &final_block : grid_final_->blocks()) {
            const auto w = final_block->w();
            for (int ipoint = 0; ipoint < final_block->npoints(); ++ipoint) {
                if (w[ipoint] < 0.0) {
	                outfile->Printf("  INFO: The definition of the current final grid includes negative weights, which the original COSX formulation does not support!\n    If this is of concern, please choose another final grid through adjusting either COSX_PRUNING_SCHEME or COSX_SPHERICAL_POINTS_FINAL.\n\n");
                    warning_printed_final = true;
	                break;
	            }
            }
	        if (warning_printed_final) break;
        }

        timer_off("CompositeJK: COSX Grid Construction");

        // => Overlap Fitting Metric <= //

        // Fit an overlap metric (Q) for both grids to reduce numerical error

        // DOI 10.1063/1.3646921, EQ. 18
        // Note: the above reference defines Q as S_an @ S_num^{-1} @ X
        // Here, Q refers to just S_ @ S_num^{-1} (no X)
        // This Q is contracted with X later to agree with the literature definition

        timer_on("CompositeJK: COSX Numeric Overlap");

        // compute the numeric overlap matrix for each grid
        auto S_num_init = compute_numeric_overlap(*grid_init_, primary_);
        auto S_num_final = compute_numeric_overlap(*grid_final_, primary_ );

        timer_off("CompositeJK: COSX Numeric Overlap");

        timer_on("CompositeJK: COSX Analytic Overlap");

        // compute the analytic overlap matrix
        MintsHelper helper(primary_, options_);
        auto S_an = helper.ao_overlap();

        timer_off("CompositeJK: COSX Analytic Overlap");

        // form the overlap metric (Q) for each grid

        timer_on("CompositeJK: COSX Overlap Metric Solve");

        int nbf = primary_->nbf();
        std::vector<int> ipiv(nbf);

        // solve: Q_init_ = S_an @ S_num_init_^{-1}
        Q_init_ = S_an->clone();
        C_DGESV(nbf, nbf, S_num_init.pointer()[0], nbf, ipiv.data(), Q_init_->pointer()[0], nbf);

        // solve: Q_final_ = S_an @ S_num_final_^{-1}
        Q_final_ = S_an->clone();
        C_DGESV(nbf, nbf, S_num_final.pointer()[0], nbf, ipiv.data(), Q_final_->pointer()[0], nbf);

        timer_off("CompositeJK: COSX Overlap Metric Solve");

    // Do nothing special if no composite K algorithm
    } else if (k_type_ == "NONE") {
        ;
    } else {
        throw PSIEXCEPTION("Invalid Composite K algorithm selected!");
    }
}

void CompositeJK::set_do_K(bool do_K) {
    // if doing K, we need an associated composite K build algorithm
    if (do_K && k_type_ == "NONE") {
        std::string error_message = "No composite K build algorithm was specified, but K matrix is required for current method! Please specify a composite K build algorithm by setting SCF_TYPE to ";
        error_message += j_type_;
        error_message += "+{K_ALGO}.";

        throw PSIEXCEPTION(error_message);
    } else if (!do_K && k_type_ != "NONE") {
        std::string info_message = "  INFO: A K algorithm (";
        info_message += k_type_;
        info_message += ") was specified in SCF_TYPE, but the current method does not use a K matrix!\n";
        info_message += "  Thus, the specified K algorithm will be unused.\n\n";

        outfile->Printf(info_message);
    }

    do_K_ = do_K;
}

size_t CompositeJK::num_computed_shells() {
    return num_computed_shells_;
}

size_t CompositeJK::memory_estimate() {
    return 0;  // Memory is O(N^2), which psi4 counts as effectively 0
}

void CompositeJK::print_header() const {
    std::string screen_type = options_.get_str("SCREENING");
    if (print_) {
        outfile->Printf("  ==> CompositeJK: Mix-and-Match J+K Algorithm Combos <==\n\n");

        outfile->Printf("    J tasked:          %11s\n", (do_J_ ? "Yes" : "No"));
        if (do_J_) outfile->Printf("    J algorithm:       %11s\n", j_type_.c_str());
        outfile->Printf("    K tasked:          %11s\n", (do_K_ ? "Yes" : "No"));
        if (do_K_) outfile->Printf("    K algorithm:       %11s\n", k_type_.c_str());
        outfile->Printf("    wK tasked:         %11s\n", (do_wK_ ? "Yes" : "No"));
        if (do_wK_) outfile->Printf("    Omega:             %11.3E\n", omega_);
        outfile->Printf("    Integrals threads: %11d\n", nthreads_);
        outfile->Printf("    Memory [MiB]:      %11ld\n", (memory_ *8L) / (1024L * 1024L));
        outfile->Printf("    Incremental Fock:  %11s\n", (incfock_ ? "Yes" : "No"));
        outfile->Printf("    Screening Type:    %11s\n", screen_type.c_str());

        if (do_J_) {
            if (j_type_ == "DFDIRJ") { print_DirectDFJ_header(); }
        }
        if (do_K_) {
            if (k_type_ == "LINK") { print_linK_header(); }
            else if (k_type_ == "COSX") { print_COSX_header(); }
        }
        outfile->Printf("\n");
    }
}

void CompositeJK::print_DirectDFJ_header() const {
    if (print_) {
        outfile->Printf("\n");
        outfile->Printf("  ==> DF-DirJ: Integral-Direct Density-Fitted J <==\n\n");

        outfile->Printf("    J Screening Cutoff:%11.0E\n", cutoff_);
    }
}

void CompositeJK::print_linK_header() const {
    if (print_) {
        outfile->Printf("\n");
        outfile->Printf("  ==> LinK: Linear Exchange K <==\n\n");

        outfile->Printf("    K Screening Cutoff:%11.0E\n", linK_ints_cutoff_);
    }
}

void CompositeJK::print_COSX_header() const {
    if (print_) {
        outfile->Printf("\n");
        outfile->Printf("  ==> COSX: Chain-of-Spheres Semi-Numerical K <==\n\n");

        outfile->Printf("    K Screening Cutoff: %11.0E\n", options_.get_double("COSX_INTS_TOLERANCE"));
        outfile->Printf("    K Density Cutoff:   %11.0E\n", options_.get_double("COSX_DENSITY_TOLERANCE"));
        outfile->Printf("    K Basis Cutoff:     %11.0E\n", options_.get_double("COSX_BASIS_TOLERANCE"));
        outfile->Printf("    K Overlap Fitting:  %11s\n", (options_.get_bool("COSX_OVERLAP_FITTING") ? "Yes" : "No"));
    }
}

void CompositeJK::preiterations() {}

void CompositeJK::incfock_setup() {
    if (do_incfock_iter_) {
        auto njk = D_ao_.size();

        // If there is no previous pseudo-density, this iteration is normal
        if (initial_iteration_ || D_prev_.size() != njk) {
            initial_iteration_ = true;

            D_ref_ = D_ao_;
            zero();
        } else { // Otherwise, the iteration is incremental
            for (size_t jki = 0; jki < njk; jki++) {
                D_ref_[jki] = D_ao_[jki]->clone();
                D_ref_[jki]->subtract(D_prev_[jki]);
            }
        }
    } else {
        D_ref_ = D_ao_;
        zero();
    }
}

void CompositeJK::incfock_postiter() {
    // Save a copy of the density for the next iteration
    D_prev_.clear();
    for(auto const &Di : D_ao_) {
        D_prev_.push_back(Di->clone());
    }
}

void CompositeJK::compute_JK() {
    // wK not supported in CompositeJK yet
    // range-separated semi-numerical exchange needs https://github.com/psi4/psi4/pull/2473
    if (do_wK_) throw PSIEXCEPTION("CompositeJK algorithms do not support wK integrals yet!");

    // explicit setup of Incfock for this SCF iteration
    if (incfock_) {
        timer_on("CompositeJK: INCFOCK Preprocessing");

        auto reset = options_.get_int("INCFOCK_FULL_FOCK_EVERY");
        auto incfock_conv = options_.get_double("INCFOCK_CONVERGENCE");
        auto Dnorm = Process::environment.globals["SCF D NORM"];
        // Do IFB on this iteration?
        do_incfock_iter_ = (Dnorm >= incfock_conv) && !initial_iteration_ && (incfock_count_ % reset != reset - 1);

        if (!initial_iteration_ && (Dnorm >= incfock_conv)) incfock_count_ += 1;

        incfock_setup();
        
        timer_off("CompositeJK: INCFOCK Preprocessing");
    } else {
        D_ref_ = D_ao_;
        zero();
    }

    // update ERI engine density matrices for density screening
    if (density_screening_) {
        for (auto eri_computer : eri_computers_["4-Center"]) {
            eri_computer->update_density(D_ref_);
        }
    }

    // => Perform matrix calculations <= //

    // Coulomb Matrix
    if (do_J_) {
        timer_on("CompositeJK: J");

        // Direct DF-J
        if (j_type_ == "DFDIRJ") {
            build_DirectDFJ(D_ref_, J_ao_);
        }

        timer_off("CompositeJK: J");
    }

    // Exchange Matrix
    if (do_K_) {
        timer_on("CompositeJK: K");

        // LinK
        if (k_type_ == "LINK") {
            build_linK(D_ref_, K_ao_);
        // COSX
        } else if (k_type_ == "COSX") {
            build_COSK(D_ref_, K_ao_);
        }

        timer_off("CompositeJK: K");
    }

    // => Finalize Incremental Fock if required <= //

    if (incfock_) {
        timer_on("CompositeJK: INCFOCK Postprocessing");
        incfock_postiter();
        timer_off("CompositeJK: INCFOCK Postprocessing");
    }

    if (initial_iteration_) initial_iteration_ = false;
}

void CompositeJK::postiterations() {}

// build the J matrix using Weigend's integral-direct density fitting algorithm
// algorithm is in Figure 1 of https://doi.org/10.1039/B204199P
void CompositeJK::build_DirectDFJ(std::vector<std::shared_ptr<Matrix>>& D, std::vector<std::shared_ptr<Matrix>>& J) {
    
    timer_on("Setup");

    // => Sizing <= //
    int njk = D.size();
    int nbf = primary_->nbf();
    int nshell = primary_->nshell();
    int nbf_aux = auxiliary_->nbf();
    int nshell_aux = auxiliary_->nshell();

    // benchmarking 
    size_t nshellpair = eri_computers_["3-Center"][0]->shell_pairs().size();
    size_t nshelltriplet = nshell_aux * nshellpair;
    size_t computed_triplets1 = 0, computed_triplets2 = 0;

    // screening threshold
    double thresh2 = options_.get_double("INTS_TOLERANCE") * options_.get_double("INTS_TOLERANCE");

    // per-thread G Vector buffers (for accumulating thread contributions to G)
    // G is the contraction of the density matrix with the 3-index ERIs
    std::vector<std::vector<SharedVector>> GT(njk, std::vector<SharedVector>(nthreads_));

    // H is the contraction of G with the inverse coulomb metric
    std::vector<SharedVector> H(njk);

    // per-thread J Matrix buffers (for accumulating thread contributions to J)
    std::vector<std::vector<SharedMatrix>> JT(njk, std::vector<SharedMatrix>(nthreads_));

    // initialize per-thread objects
    for(size_t jki = 0; jki < njk; jki++) {
        for(size_t thread = 0; thread < nthreads_; thread++) {
            JT[jki][thread] = std::make_shared<Matrix>(nbf, nbf);
            GT[jki][thread] = std::make_shared<Vector>(nbf_aux);
        }
        H[jki] = std::make_shared<Vector>(nbf_aux);
    }

    // diagonal shell maxima of J_metric_ for screening
    std::vector<double> J_metric_shell_diag(nshell_aux, 0.0);
    for (size_t s = 0; s < nshell_aux; s++) {
        int bf_start = auxiliary_->shell(s).function_index();
        int bf_end = bf_start + auxiliary_->shell(s).nfunction();
        for (size_t bf = bf_start; bf < bf_end; bf++) {
            J_metric_shell_diag[s] = std::max(J_metric_shell_diag[s], J_metric_->get(bf, bf));
        }
    }

    // shell maxima of D for screening
    Matrix Dshell(nshell, nshell);
    auto Dshellp = Dshell.pointer();

    for(size_t M = 0; M < nshell; M++) {
        int nm = primary_->shell(M).nfunction();
        int mstart = primary_->shell(M).function_index();
        for(size_t N = 0; N < nshell; N++) {
            int nn = primary_->shell(N).nfunction();
            int nstart = primary_->shell(N).function_index();
            for(size_t jki = 0; jki < njk; jki++) {
                auto Dp = D[jki]->pointer();
                for(size_t m = mstart; m < mstart + nm; m++) {
                    for(size_t n = nstart; n < nstart + nn; n++) {
                        Dshellp[M][N] = std::max(Dshellp[M][N], std::abs(Dp[m][n]));
                    }
                }
            }
        }
    }

    timer_off("Setup");

    // Number of computed shell triplets is tracked for benchmarking purposes
    num_computed_shells_ = 0L;

    //  => First Contraction <= //

    // contract D with three-index DF ERIs to get G:
    // G_{p} = D_{mn} * (mn|p)
    // G_{p} correlates to gamma_P in Figure 1 of Weigend's paper

    timer_on("ERI1");
#pragma omp parallel for schedule(guided) num_threads(nthreads_) reduction(+ : computed_triplets1)
    for (size_t MNP = 0; MNP < nshelltriplet; MNP++) {

        size_t MN = MNP % nshellpair;
        size_t P = MNP / nshellpair;
        int rank = 0;
#ifdef _OPENMP
        rank = omp_get_thread_num();
#endif
        auto bra = eri_computers_["3-Center"][rank]->shell_pairs()[MN];
        size_t M = bra.first;
        size_t N = bra.second;
        if(Dshellp[M][N] * Dshellp[M][N] * J_metric_shell_diag[P] * eri_computers_["3-Center"][rank]->shell_pair_value(M,N) < thresh2) {
            continue;
        }
        computed_triplets1++;
        int np = auxiliary_->shell(P).nfunction();
        int pstart = auxiliary_->shell(P).function_index();
        int nm = primary_->shell(M).nfunction();
        int mstart = primary_->shell(M).function_index();
        int nn = primary_->shell(N).nfunction();
        int nstart = primary_->shell(N).function_index();
        eri_computers_["3-Center"][rank]->compute_shell(P, 0, M, N);
        const auto & buffer = eri_computers_["3-Center"][rank]->buffers()[0];

        for(size_t jki = 0; jki < njk; jki++) {

            auto GTp = GT[jki][rank]->pointer();
            auto Dp = D[jki]->pointer();

            for (int p = pstart, index = 0; p < pstart + np; p++) {
                for (int m = mstart; m < mstart + nm; m++) {
                    for (int n = nstart; n < nstart + nn; n++, index++) {
                        GTp[p] += buffer[index] * Dp[m][n];
                        if (N != M) GTp[p] += buffer[index] * Dp[n][m];
                    }
                }
            }

        }

    }

    timer_off("ERI1");

    //  => Second Contraction <= //

    //  linear solve for H:
    //  G_{p} = H_{q} (q|p)
    //  H_{p} correlates to gamma_Q in Figure 1 of Weigend's paper

    timer_on("Metric");

    std::vector<int> ipiv(nbf_aux);

    for(size_t jki = 0; jki < njk; jki++) {
        for(size_t thread = 0; thread < nthreads_; thread++) {
            H[jki]->add(*GT[jki][thread]);
        }
        C_DGESV(nbf_aux, 1, J_metric_->clone()->pointer()[0], nbf_aux, ipiv.data(), H[jki]->pointer(), nbf_aux);
    }


    // I believe C_DSYSV should be faster than C_GESV, but I've found the opposite to be true.
    // This performance issue should be investigated, but is not consequential here.
    // The cost of either linear solve is dwarfed by the actual integral computation.
    //
    //std::vector<double> work(3 * nbf_aux);
    //int errcode = C_DSYSV('U', nbf_aux, 1, J_metric_->clone()->pointer()[0], nbf_aux, ipiv.data(), H[jki]->pointer(), nbf_aux, work.data(), 3 * nbf_aux);

    // shell maxima of H for screening
    Vector H_shell_max(nshell_aux);
    auto H_shell_maxp = H_shell_max.pointer();

    for(size_t jki = 0; jki < njk; jki++) {

        auto Hp = H[jki]->pointer();

        for (int P = 0; P < nshell_aux; P++) {
            int np = auxiliary_->shell(P).nfunction();
            int pstart = auxiliary_->shell(P).function_index();
            for (int p = pstart; p < pstart + np; p++) {
                H_shell_maxp[P] = std::max(H_shell_maxp[P], std::abs(Hp[p]));
            }
        }

    }

    timer_off("Metric");

    //  => Third Contraction <= //

    // contract H with three-index DF ERIs to get J
    // J_{mn} = H_{p} (mn|p)
    // J_{mn} correlates to J_[uv] in Figure 1 of Weigend's paper

    timer_on("ERI2");

#pragma omp parallel for schedule(guided) num_threads(nthreads_) reduction(+ : computed_triplets2)
    for (size_t MNP = 0; MNP < nshelltriplet; MNP++) {

        size_t MN = MNP % nshellpair;
        size_t P = MNP / nshellpair;
        int rank = 0;
#ifdef _OPENMP
        rank = omp_get_thread_num();
#endif
        auto bra = eri_computers_["3-Center"][rank]->shell_pairs()[MN];
        size_t M = bra.first;
        size_t N = bra.second;
        if(H_shell_maxp[P] * H_shell_maxp[P] * J_metric_shell_diag[P] * eri_computers_["3-Center"][rank]->shell_pair_value(M,N) < thresh2) {
            continue;
        }
        computed_triplets2++;
        int np = auxiliary_->shell(P).nfunction();
        int pstart = auxiliary_->shell(P).function_index();
        int nm = primary_->shell(M).nfunction();
        int mstart = primary_->shell(M).function_index();
        int nn = primary_->shell(N).nfunction();
        int nstart = primary_->shell(N).function_index();

        eri_computers_["3-Center"][rank]->compute_shell(P, 0, M, N);
        const auto & buffer = eri_computers_["3-Center"][rank]->buffers()[0];

        for(size_t jki = 0; jki < njk; jki++) {

            auto JTp = JT[jki][rank]->pointer();
            auto Hp = H[jki]->pointer();

            for (int p = pstart, index = 0; p < pstart + np; p++) {
                for (int m = mstart; m < mstart + nm; m++) {
                    for (int n = nstart; n < nstart + nn; n++, index++) {
                        JTp[m][n] += buffer[index] * Hp[p];
                        if (N != M) JTp[n][m] += buffer[index] * Hp[p];
                    }
                }
            }

        }

    }

    timer_off("ERI2");

    num_computed_shells_ = computed_triplets1 + computed_triplets2;
    if (get_bench()) {
        computed_shells_per_iter_["Triplets"].push_back(num_computed_shells());
    }

    for(size_t jki = 0; jki < njk; jki++) {
        for (size_t thread = 0; thread < nthreads_; thread++) {
            J[jki]->add(JT[jki][thread]);
        }
        J[jki]->hermitivitize();
    }

}

// build the K matrix using Ochsenfelds's Linear Exchange (LinK) algorithm 
// To follow this code, compare with figure 1 of DOI: 10.1063/1.476741
void CompositeJK::build_linK(std::vector<SharedMatrix>& D, std::vector<SharedMatrix>& K) {

    if (!lr_symmetric_) {
        throw PSIEXCEPTION("Non-symmetric K matrix builds are currently not supported in the LinK algorithm.");
    }

    // ==> Prep Auxiliary Quantities <== //

    // => Sizing <= //
    int nshell = primary_->nshell();
    int nbf = primary_->nbf();
    int nthread = nthreads_;

    // => Atom Blocking <= //
    std::vector<int> shell_endpoints_for_atom;
    std::vector<int> basis_endpoints_for_shell;

    int atomic_ind = -1;
    for (int P = 0; P < nshell; P++) {
        if (primary_->shell(P).ncenter() > atomic_ind) {
            shell_endpoints_for_atom.push_back(P);
            atomic_ind++;
        }
        basis_endpoints_for_shell.push_back(primary_->shell_to_basis_function(P));
    }
    shell_endpoints_for_atom.push_back(nshell);
    basis_endpoints_for_shell.push_back(nbf);

    size_t natom = shell_endpoints_for_atom.size() - 1;

    size_t max_functions_per_atom = 0L;
    for (size_t atom = 0; atom < natom; atom++) {
        size_t size = 0L;
        for (int P = shell_endpoints_for_atom[atom]; P < shell_endpoints_for_atom[atom + 1]; P++) {
            size += primary_->shell(P).nfunction();
        }
        max_functions_per_atom = std::max(max_functions_per_atom, size);
    }

    if (debug_) {
        outfile->Printf("  ==> LinK: Atom Blocking <==\n\n");
        for (size_t atom = 0; atom < natom; atom++) {
            outfile->Printf("  Atom: %3d, Atom Start: %4d, Atom End: %4d\n", atom, shell_endpoints_for_atom[atom],
                            shell_endpoints_for_atom[atom + 1]);
            for (int P = shell_endpoints_for_atom[atom]; P < shell_endpoints_for_atom[atom + 1]; P++) {
                int size = primary_->shell(P).nfunction();
                int off = primary_->shell(P).function_index();
                int off2 = basis_endpoints_for_shell[P];
                outfile->Printf("    Shell: %4d, Size: %4d, Offset: %4d, Offset2: %4d\n", P, size, off,
                                off2);
            }
        }
        outfile->Printf("\n");
    }

    // ==> Prep Atom Pairs <== //
    // Atom-pair blocking inherited from DirectJK code
    // TODO: Test shell-pair blocking

    std::vector<std::pair<int, int>> atom_pairs;
    for (size_t Patom = 0; Patom < natom; Patom++) {
        for (size_t Qatom = 0; Qatom <= Patom; Qatom++) {
            bool found = false;
            for (int P = shell_endpoints_for_atom[Patom]; P < shell_endpoints_for_atom[Patom + 1]; P++) {
                for (int Q = shell_endpoints_for_atom[Qatom]; Q < shell_endpoints_for_atom[Qatom + 1]; Q++) {
                    if (eri_computers_["4-Center"][0]->shell_pair_significant(P, Q)) {
                        found = true;
                        atom_pairs.emplace_back(Patom, Qatom);
                        break;
                    }
                }
                if (found) break;
            }
        }
    }

    // ==> Start "Pre-ordering and pre-selection to find significant elements in P_uv" in Fig. 1 of paper <== //
    
    // ==> Prep Bra-Bra Shell Pairs <== //

    // A comparator used for sorting integral screening values
    auto screen_compare = [](const std::pair<int, double> &a,
                                    const std::pair<int, double> &b) { return a.second > b.second; };

    std::vector<std::vector<int>> significant_bras(nshell);
    double max_integral = eri_computers_["4-Center"][0]->max_integral();

#pragma omp parallel for
    for (size_t P = 0; P < nshell; P++) {
        std::vector<std::pair<int, double>> PQ_shell_values;
        for (size_t Q = 0; Q < nshell; Q++) {
            double pq_pq = std::sqrt(eri_computers_["4-Center"][0]->shell_ceiling2(P, Q, P, Q));
            double schwarz_value = std::sqrt(pq_pq * max_integral);
            if (schwarz_value >= cutoff_) {
                PQ_shell_values.emplace_back(Q, schwarz_value);
            }
        }
        std::sort(PQ_shell_values.begin(), PQ_shell_values.end(), screen_compare);

        for (const auto& value : PQ_shell_values) {
            significant_bras[P].push_back(value.first);
        }
    }

    // ==> Prep Bra-Ket Shell Pairs <== //

    // => Calculate Shell Ceilings <= //
    std::vector<double> shell_ceilings(nshell, 0.0);

    // sqrt(Umax|Umax) in Ochsenfeld Eq. 3
#pragma omp parallel for
    for (int P = 0; P < nshell; P++) {
        for (int Q = 0; Q <= P; Q++) {
            double val = std::sqrt(eri_computers_["4-Center"][0]->shell_ceiling2(P, Q, P, Q));
            shell_ceilings[P] = std::max(shell_ceilings[P], val);
#pragma omp critical
            shell_ceilings[Q] = std::max(shell_ceilings[Q], val);
        }
    }

    std::vector<std::vector<int>> significant_kets(nshell);

    // => Use shell ceilings to compute significant ket-shells for each bra-shell <= //
    // => Implementation of Eq. 4 in paper <= //
#pragma omp parallel for
    for (size_t P = 0; P < nshell; P++) {
        std::vector<std::pair<int, double>> PR_shell_values;
        for (size_t R = 0; R < nshell; R++) {
            double screen_val = shell_ceilings[P] * shell_ceilings[R] * eri_computers_["4-Center"][0]->shell_pair_max_density(P, R);
            if (screen_val >= linK_ints_cutoff_) {
                PR_shell_values.emplace_back(R, screen_val);
            }
        }
        std::sort(PR_shell_values.begin(), PR_shell_values.end(), screen_compare);

        for (const auto& value : PR_shell_values) {
            significant_kets[P].push_back(value.first);
        }
    }

    size_t natom_pair = atom_pairs.size();

    // ==> End "Pre-ordering and pre-selection to find significant elements in P_uv" in Fig. 1 of paper <== //
    
    // ==> Intermediate Buffers <== //

    // Temporary buffers used during the K contraction process to
    // Take full advantage of permutational symmetry of ERIs
    std::vector<std::vector<SharedMatrix>> KT;

    // To prevent race conditions, give every thread a buffer
    for (int thread = 0; thread < nthread; thread++) {
        std::vector<SharedMatrix> K2;
        for (size_t ind = 0; ind < D.size(); ind++) {
            // (pq|rs) can be contracted into Kpr, Kps, Kqr, Kqs (hence the 4)
            K2.push_back(std::make_shared<Matrix>("KT (linK)", 4 * max_functions_per_atom, nbf));
        }
        KT.push_back(K2);
    }

    // ==> Start "Loop over significant 'bra'-shell pairs uh" in Fig. 1 of paper <== //
    
    // Number of computed shell quartets is tracked for benchmarking purposes
    num_computed_shells_ = 0L;
    size_t computed_shells = 0L;

    // ==> Integral Formation Loop <== //

#pragma omp parallel for num_threads(nthread) schedule(dynamic) reduction(+ : computed_shells)
    for (size_t ipair = 0L; ipair < natom_pair; ipair++) { // O(N) shell-pairs in asymptotic limit
        int Patom = atom_pairs[ipair].first;
        int Qatom = atom_pairs[ipair].second;

        // Number of shells per atom
        int nPshell = shell_endpoints_for_atom[Patom + 1] - shell_endpoints_for_atom[Patom];
        int nQshell = shell_endpoints_for_atom[Qatom + 1] - shell_endpoints_for_atom[Qatom];

        // First shell per atom
        int Pstart = shell_endpoints_for_atom[Patom];
        int Qstart = shell_endpoints_for_atom[Qatom];

        // Number of basis functions per atom
        int nPbasis = basis_endpoints_for_shell[Pstart + nPshell] - basis_endpoints_for_shell[Pstart];
        int nQbasis = basis_endpoints_for_shell[Qstart + nQshell] - basis_endpoints_for_shell[Qstart];

        int thread = 0;
#ifdef _OPENMP
        thread = omp_get_thread_num();
#endif

        // Keep track of contraction indices for stripeout (Towards end of this function)
        std::vector<std::unordered_set<int>> P_stripeout_list(nPshell);
        std::vector<std::unordered_set<int>> Q_stripeout_list(nQshell);

        bool touched = false;
        for (int P = Pstart; P < Pstart + nPshell; P++) {
            for (int Q = Qstart; Q < Qstart + nQshell; Q++) {

                if (Q > P) continue;
                if (!eri_computers_["4-Center"][0]->shell_pair_significant(P, Q)) continue;

                int dP = P - Pstart;
                int dQ = Q - Qstart;

                // => Start "Formation of Significant Shell Pair List ML" in Fig. 1 of Paper <= //

                // Significant ket shell pairs RS for bra shell pair PQ
                // represents the merge of ML_P and ML_Q (mini-lists) as defined in Oschenfeld
                // Unordered set structure allows for automatic merging as new elements are added
                std::unordered_set<int> ML_PQ;

                // Form ML_P as part of ML_PQ
                for (const int R : significant_kets[P]) {
                    bool is_significant = false;
                    for (const int S : significant_bras[R]) {
                        double screen_val = eri_computers_["4-Center"][0]->shell_pair_max_density(P, R) * std::sqrt(eri_computers_["4-Center"][0]->shell_ceiling2(P, Q, R, S));

                        if (screen_val >= linK_ints_cutoff_) {
                            if (!is_significant) is_significant = true;
                            int RS = (R >= S) ? (R * nshell + S) : (S * nshell + R);
                            if (RS > P * nshell + Q) continue;
                            ML_PQ.emplace(RS);
                            Q_stripeout_list[dQ].emplace(S);
                        }
                        else break;
                    }
                    if (!is_significant) break;
                }

                // Form ML_Q as part of ML_PQ
                for (const int R : significant_kets[Q]) {
                    bool is_significant = false;
                    for (const int S : significant_bras[R]) {
                        double screen_val = eri_computers_["4-Center"][0]->shell_pair_max_density(Q, R) * std::sqrt(eri_computers_["4-Center"][0]->shell_ceiling2(P, Q, R, S));

                        if (screen_val >= linK_ints_cutoff_) {
                            if (!is_significant) is_significant = true;
                            int RS = (R >= S) ? (R * nshell + S) : (S * nshell + R);
                            if (RS > P * nshell + Q) continue;
                            ML_PQ.emplace(RS);
                            P_stripeout_list[dP].emplace(S);
                        }
                        else break;
                    }
                    if (!is_significant) break;
                }

                // Loop over significant RS pairs
                for (const int RS : ML_PQ) {

                    int R = RS / nshell;
                    int S = RS % nshell;

                    if (!eri_computers_["4-Center"][0]->shell_pair_significant(R, S)) continue;
                    if (!eri_computers_["4-Center"][0]->shell_significant(P, Q, R, S)) continue;

                    if (eri_computers_["4-Center"][thread]->compute_shell(P, Q, R, S) == 0)
                        continue;
                    computed_shells++;

                    const double* buffer = eri_computers_["4-Center"][thread]->buffer();

                    // Number of basis functions in shells P, Q, R, S
                    int shell_P_nfunc = primary_->shell(P).nfunction();
                    int shell_Q_nfunc = primary_->shell(Q).nfunction();
                    int shell_R_nfunc = primary_->shell(R).nfunction();
                    int shell_S_nfunc = primary_->shell(S).nfunction();

                    // Basis Function Starting index for shell
                    int shell_P_start = primary_->shell(P).function_index();
                    int shell_Q_start = primary_->shell(Q).function_index();
                    int shell_R_start = primary_->shell(R).function_index();
                    int shell_S_start = primary_->shell(S).function_index();

                    // Basis Function offset from first basis function in the atom
                    int shell_P_offset = basis_endpoints_for_shell[P] - basis_endpoints_for_shell[Pstart];
                    int shell_Q_offset = basis_endpoints_for_shell[Q] - basis_endpoints_for_shell[Qstart];

                    for (size_t ind = 0; ind < D.size(); ind++) {
                        double** Kp = K[ind]->pointer();
                        double** Dp = D[ind]->pointer();
                        double** KTp = KT[thread][ind]->pointer();
                        const double* buffer2 = buffer;

                        if (!touched) {
                            ::memset((void*)KTp[0L * max_functions_per_atom], '\0', nPbasis * nbf * sizeof(double));
                            ::memset((void*)KTp[1L * max_functions_per_atom], '\0', nPbasis * nbf * sizeof(double));
                            ::memset((void*)KTp[2L * max_functions_per_atom], '\0', nQbasis * nbf * sizeof(double));
                            ::memset((void*)KTp[3L * max_functions_per_atom], '\0', nQbasis * nbf * sizeof(double));
                        }

                        // Four pointers needed for PR, PS, QR, QS
                        double* K1p = KTp[0L * max_functions_per_atom];
                        double* K2p = KTp[1L * max_functions_per_atom];
                        double* K3p = KTp[2L * max_functions_per_atom];
                        double* K4p = KTp[3L * max_functions_per_atom];

                        double prefactor = 1.0;
                        if (P == Q) prefactor *= 0.5;
                        if (R == S) prefactor *= 0.5;
                        if (P == R && Q == S) prefactor *= 0.5;

                        // => Computing integral contractions to K buffers <= //
                        for (int p = 0; p < shell_P_nfunc; p++) {
                            for (int q = 0; q < shell_Q_nfunc; q++) {
                                for (int r = 0; r < shell_R_nfunc; r++) {
                                    for (int s = 0; s < shell_S_nfunc; s++) {

                                        K1p[(p + shell_P_offset) * nbf + r + shell_R_start] +=
                                            prefactor * (Dp[q + shell_Q_start][s + shell_S_start]) * (*buffer2);
                                        K2p[(p + shell_P_offset) * nbf + s + shell_S_start] +=
                                            prefactor * (Dp[q + shell_Q_start][r + shell_R_start]) * (*buffer2);
                                        K3p[(q + shell_Q_offset) * nbf + r + shell_R_start] +=
                                            prefactor * (Dp[p + shell_P_start][s + shell_S_start]) * (*buffer2);
                                        K4p[(q + shell_Q_offset) * nbf + s + shell_S_start] +=
                                            prefactor * (Dp[p + shell_P_start][r + shell_R_start]) * (*buffer2);

                                        buffer2++;
                                    }
                                }
                            }
                        }
                    }
                    touched = true;
                }
            }
        }

        // => Master shell quartet loops <= //

        if (!touched) continue;

        // => Stripe out (Writing to K matrix) <= //
        for (auto& KTmat : KT[thread]) {
            KTmat->scale(2.0);
        }

        for (size_t ind = 0; ind < D.size(); ind++) {
            double** KTp = KT[thread][ind]->pointer();
            double** Kp = K[ind]->pointer();

            double* K1p = KTp[0L * max_functions_per_atom];
            double* K2p = KTp[1L * max_functions_per_atom];
            double* K3p = KTp[2L * max_functions_per_atom];
            double* K4p = KTp[3L * max_functions_per_atom];

            // K_PR and K_PS
            for (int P = Pstart; P < Pstart + nPshell; P++) {
                int dP = P - Pstart;
                int shell_P_start = primary_->shell(P).function_index();
                int shell_P_nfunc = primary_->shell(P).nfunction();
                int shell_P_offset = basis_endpoints_for_shell[P] - basis_endpoints_for_shell[Pstart];
                for (const int S : P_stripeout_list[dP]) {
                    int shell_S_start = primary_->shell(S).function_index();
                    int shell_S_nfunc = primary_->shell(S).nfunction();

                    for (int p = 0; p < shell_P_nfunc; p++) {
                        for (int s = 0; s < shell_S_nfunc; s++) {
#pragma omp atomic
                            Kp[shell_P_start + p][shell_S_start + s] += K1p[(p + shell_P_offset) * nbf + s + shell_S_start];
#pragma omp atomic
                            Kp[shell_P_start + p][shell_S_start + s] += K2p[(p + shell_P_offset) * nbf + s + shell_S_start];
                        }
                    }

                }
            }

            // K_QR and K_QS
            for (int Q = Qstart; Q < Qstart + nQshell; Q++) {
                int dQ = Q - Qstart;
                int shell_Q_start = primary_->shell(Q).function_index();
                int shell_Q_nfunc = primary_->shell(Q).nfunction();
                int shell_Q_offset = basis_endpoints_for_shell[Q] - basis_endpoints_for_shell[Qstart];
                for (const int S : Q_stripeout_list[dQ]) {
                    int shell_S_start = primary_->shell(S).function_index();
                    int shell_S_nfunc = primary_->shell(S).nfunction();

                    for (int q = 0; q < shell_Q_nfunc; q++) {
                        for (int s = 0; s < shell_S_nfunc; s++) {
#pragma omp atomic
                            Kp[shell_Q_start + q][shell_S_start + s] += K3p[(q + shell_Q_offset) * nbf + s + shell_S_start];
#pragma omp atomic
                            Kp[shell_Q_start + q][shell_S_start + s] += K4p[(q + shell_Q_offset) * nbf + s + shell_S_start];
                        }
                    }

                }
            }

        }  // End stripe out

    }  // End master task list

    for (auto& Kmat : K) {
        Kmat->hermitivitize();
    }

    num_computed_shells_ = computed_shells;
    if (get_bench()) {
        computed_shells_per_iter_["Quartets"].push_back(num_computed_shells());
    }
}


// build the K matrix using Neeses's Chain-of-Spheres Exchange algorithm
// algorithm is originally proposed in https://doi.org/10.1016/j.chemphys.2008.10.036 
// overlap fitting is discussed in https://doi.org/10.1063/1.3646921
void CompositeJK::build_COSK(std::vector<std::shared_ptr<Matrix>>& D, std::vector<std::shared_ptr<Matrix>>& K) {

    // => Sizing <= //
    int njk = D.size();
    int nbf = primary_->nbf();
    int nshell = primary_->nshell();
    int natom = primary_->molecule()->natom();

    // => Knobs <= //
    //double gscreen = options_.get_double("COSX_INTS_TOLERANCE");
    double kscreen = options_.get_double("COSX_INTS_TOLERANCE");
    double dscreen = options_.get_double("COSX_DENSITY_TOLERANCE");
    bool overlap_fitted = options_.get_bool("COSX_OVERLAP_FITTING");

    // use a small DFTGrid grid (and overlap metric) for early SCF iterations
    // otherwise use a large DFTGrid
    auto grid = early_screening_ ? grid_init_ : grid_final_;
    auto Q = early_screening_ ? Q_init_ : Q_final_;

    // => Initialization <= //

    // per-thread ElectrostaticInt object (for computing one-electron "pseudospectral" integrals)
    std::vector<std::shared_ptr<ElectrostaticInt>> int_computers(nthreads_);

    // per-thread BasisFunctions object (for computing basis function values at grid points)
    std::vector<std::shared_ptr<BasisFunctions>> bf_computers(nthreads_);

    // per-thread K Matrix buffers (for accumulating thread contributions to K)
    std::vector<std::vector<SharedMatrix>> KT(njk, std::vector<SharedMatrix>(nthreads_));

    // initialize per-thread objects
    IntegralFactory factory(primary_);
    for(size_t thread = 0; thread < nthreads_; thread++) {
        int_computers[thread] = std::shared_ptr<ElectrostaticInt>(static_cast<ElectrostaticInt *>(factory.electrostatic().release()));
        bf_computers[thread] = std::make_shared<BasisFunctions>(primary_, grid->max_points(), grid->max_functions());
        for(size_t jki = 0; jki < njk; jki++) {
            KT[jki][thread] = std::make_shared<Matrix>(nbf, nbf);
        }
    }

    // precompute bounds for the one-electron integrals
    auto esp_bound = compute_esp_bound(*primary_);
    auto esp_boundp = esp_bound.pointer();

    // inter-atom and inter-shell distances [Bohr]
    auto dist = primary_->molecule()->distance_matrix();
    auto shell_dist = std::make_shared<Matrix>(nshell, nshell);
    for(size_t s1 = 0; s1 < nshell; s1++) {
        size_t c1 = primary_->shell_to_center(s1);
        for(size_t s2 = 0; s2 < nshell; s2++) {
            size_t c2 = primary_->shell_to_center(s2);
            shell_dist->set(s1, s2, dist.get(c1, c2));
        }
    }

    // extent of each basis shell [Bohr]
    auto shell_extents = grid->extents()->shell_extents();

    // map of shell pairs with overlapping extents
    std::vector<std::vector<int>> shell_extent_map(nshell);
    for(size_t s1 = 0; s1 < nshell; s1++) {
        for(size_t s2 = 0; s2 < nshell; s2++) {
            if (shell_dist->get(s1, s2) <= shell_extents->get(s2) + shell_extents->get(s1)) {
                shell_extent_map[s1].push_back(s2);
            }
        }
    }

    // => Integral Computation <= //

    // benchmarking statistics
    num_computed_shells_ = 0L;
    size_t int_shells_total = 0;
    size_t int_shells_computed = 0;

    timer_on("Grid Loop");

    // The primary COSK loop over blocks of grid points
#pragma omp parallel for schedule(dynamic) num_threads(nthreads_) reduction(+ : int_shells_total, int_shells_computed)
    for (size_t bi = 0; bi < grid->blocks().size(); bi++) {

        int rank = 0;
#ifdef _OPENMP
        rank = omp_get_thread_num();
#endif

        // grid points in this block
        auto block = grid->blocks()[bi];
        int npoints_block = block->npoints();
        auto x = block->x();
        auto y = block->y();
        auto z = block->z();
        auto w = block->w();

        // significant basis functions and shells at these grid points
        // significance determined via basis extent
        const auto &bf_map = block->functions_local_to_global();
        const auto &shell_map = block->shells_local_to_global();
        int nbf_block = bf_map.size();
        int ns_block = shell_map.size();

        // lists of all basis functions and shells
        //
        // The use of these "all" lists adds O(N^2) cost to the COSK grid loop (w/ small prefactor)
        // This cost is negligible relative to the esp integral computation, which is O(N) (w/ a much larger prefactor),
        // but future COSK work could remove this potential bottleneck
        std::vector<int> bf_map_all;
        std::vector<int> shell_map_all;
        for (size_t bf = 0; bf < nbf; bf++) bf_map_all.push_back(bf);
        for (size_t s = 0; s < nshell; s++) shell_map_all.push_back(s);
        int nbf_block_all = bf_map_all.size();
        int ns_block_all = shell_map_all.size();

        // => Bookkeeping <= //

        // map index in shell_map_all to first index in bf_map_all
        std::vector<int> shell_map_all_to_bf_map_all;
        if (shell_map_all.size() > 0) {
            shell_map_all_to_bf_map_all.push_back(0);
            for(size_t shell_map_ind = 0; (shell_map_ind + 1) < shell_map_all.size(); shell_map_ind++) {
                size_t MU = shell_map_all[shell_map_ind];
                shell_map_all_to_bf_map_all.push_back(primary_->shell(MU).nfunction() + shell_map_all_to_bf_map_all.back());
            }
        }

        // map index in shell_map to first index in bf_map
        std::vector<int> shell_map_to_bf_map;
        if (shell_map.size() > 0) {
            shell_map_to_bf_map.push_back(0);
            for(size_t shell_map_ind = 0; (shell_map_ind + 1) < shell_map.size(); shell_map_ind++) {
                size_t MU = shell_map[shell_map_ind];
                shell_map_to_bf_map.push_back(primary_->shell(MU).nfunction() + shell_map_to_bf_map.back());
            }
        }

        // map back from global shell index to local shell index
        std::map<size_t, size_t> shell_map_inv;
        for (size_t shell_ind = 0; shell_ind < shell_map.size(); shell_ind++) {
            shell_map_inv[shell_map[shell_ind]] = shell_ind;
        }

        // => Process Density Matrix <= //

        // significant cols of D for this grid block
        std::vector<SharedMatrix> D_block(njk);
        for(size_t jki = 0; jki < njk; jki++) {
            D_block[jki] = std::make_shared<Matrix>(nbf_block_all, nbf_block);
        }

        for(size_t jki = 0; jki < njk; jki++) {
            auto Dp = D[jki]->pointer();
            auto D_blockp = D_block[jki]->pointer();
            for (size_t tau_ind = 0; tau_ind < nbf_block_all; tau_ind++) {
                size_t tau = bf_map_all[tau_ind];
                for (size_t kappa_ind = 0; kappa_ind < nbf_block; kappa_ind++) {
                    size_t kappa = bf_map[kappa_ind];
                    D_blockp[tau_ind][kappa_ind] = Dp[tau][kappa];
                }
            }
        }

        // shell-pair maxima of D_block
        auto D_block_shell = std::make_shared<Matrix>(ns_block_all, ns_block);
        auto D_block_shellp = D_block_shell->pointer();

        for (size_t TAU_ind = 0; TAU_ind < ns_block_all; TAU_ind++) {
            size_t TAU = shell_map_all[TAU_ind];
            size_t tau_start = shell_map_all_to_bf_map_all[TAU_ind];
            size_t num_tau = primary_->shell(TAU).nfunction();
            for (size_t KAPPA_ind = 0; KAPPA_ind < ns_block; KAPPA_ind++) {
                size_t KAPPA = shell_map[KAPPA_ind];
                size_t kappa_start = shell_map_to_bf_map[KAPPA_ind];
                size_t num_kappa = primary_->shell(KAPPA).nfunction();
                for(size_t jki = 0; jki < njk; jki++) {
                    auto D_blockp = D_block[jki]->pointer();
                    for (size_t bf1 = tau_start; bf1 < tau_start + num_tau; bf1++) {
                        for (size_t bf2 = kappa_start; bf2 < kappa_start + num_kappa; bf2++) {
                            D_block_shellp[TAU_ind][KAPPA_ind] = std::max(D_block_shellp[TAU_ind][KAPPA_ind], std::abs(D_blockp[bf1][bf2]));
                        }
                    }
                }
            }
        }

        // significant TAU shells determined from sparsity of the density matrix
        // i.e. KAPPA -> TAU sparsity. Refered to by Neese as a "p-junction"
        // as discussed in section 3.1 of DOI 10.1016/j.chemphys.2008.10.036
        std::vector<int> shell_map_tau;

        for(size_t TAU = 0; TAU < ns_block_all; TAU++) {
            for(size_t KAPPA_ind = 0; KAPPA_ind < ns_block; KAPPA_ind++) {
                size_t KAPPA = shell_map[KAPPA_ind];
                if (D_block_shellp[TAU][KAPPA_ind] > dscreen) {
                    shell_map_tau.push_back(TAU);
                    break;
                }
            }
        }

        // => X Matrix <= //

        // DOI 10.1016/j.chemphys.2008.10.036, EQ. 4

        // note that the X matrix is defined slightly differently in our code
        // to account for the possibility of negative grid weights
        // here, we define X using sqrt(abs(w)) instead of sqrt(w)

	// compute basis functions at these grid points
        bf_computers[rank]->compute_functions(block);
        auto point_values = bf_computers[rank]->basis_values()["PHI"];

        // resize the buffer of basis function values
        auto X_block = std::make_shared<Matrix>(npoints_block, nbf_block);  // points x nbf_block

        auto X_blockp = X_block->pointer();
        for (size_t p = 0; p < npoints_block; p++) {
            for (size_t k = 0; k < nbf_block; k++) {
                X_blockp[p][k] = point_values->get(p, k) * std::sqrt(std::abs(w[p]));
            }
        }

        // absmax of X matrix over basis functions (row maximum) needed for screening
        Vector X_block_bfmax(npoints_block);
        auto X_block_bfmaxp = X_block_bfmax.pointer();
        for (size_t p = 0; p < npoints_block; p++) {
            for (size_t k = 0; k < nbf_block; k++) {
                X_block_bfmaxp[p] = std::max(X_block_bfmaxp[p], std::abs(X_blockp[p][k]));
            }
        }

        double X_block_max = X_block->absmax();

        // => F Matrix <= //

        // DOI 10.1016/j.chemphys.2008.10.036, EQ. 6

        // contract density with basis functions values at these grid points
        std::vector<SharedMatrix> F_block(njk);
        for(size_t jki = 0; jki < njk; jki++) {
            F_block[jki] = linalg::doublet(X_block, D_block[jki], false, true);
        }
        
        // shell maxima of F_block
        auto F_block_shell = std::make_shared<Matrix>(npoints_block, nshell);
        auto F_block_shellp = F_block_shell->pointer();

        // grid point maxima of F_block_gmax
        auto F_block_gmax = std::make_shared<Vector>(nshell);
        auto F_block_gmaxp = F_block_gmax->pointer();

        for (size_t p = 0; p < npoints_block; p++) {
            for (size_t TAU_local = 0; TAU_local < shell_map_all.size(); TAU_local++) {
                size_t TAU = shell_map_all[TAU_local];
                size_t num_tau = primary_->shell(TAU).nfunction();
                size_t tau_start = shell_map_all_to_bf_map_all[TAU_local];
                for(size_t jki = 0; jki < njk; jki++) {
                    auto F_blockp = F_block[jki]->pointer();
                    for (size_t tau = tau_start; tau < tau_start + num_tau; tau++) {
                        F_block_shellp[p][TAU_local] = std::max(F_block_shellp[p][TAU_local], std::abs(F_blockp[p][tau]));
                        F_block_gmaxp[TAU_local] = std::max(F_block_gmaxp[TAU_local], std::abs(F_blockp[p][tau]));
                    }
                }
            }
        }

        // => Q Matrix <= //

        // DOI 10.1063/1.3646921, EQ. 18

        // slice of overlap metric (Q) made up of significant basis functions at this grid point
        auto Q_block = std::make_shared<Matrix>(nbf_block, nbf_block);
        for(size_t mu_local = 0; mu_local < nbf_block; mu_local++) {
            size_t mu = bf_map[mu_local];
            for(size_t nu_local = 0; nu_local < nbf_block; nu_local++) {
                size_t nu = bf_map[nu_local];
                Q_block->set(mu_local, nu_local, Q->get(mu, nu));
            }
        }

        // now Q_block agrees with EQ. 18 (see note about Q_init_ and Q_final_ in common_init())
        Q_block = linalg::doublet(X_block, Q_block, false, true);

        // => G Matrix <= //

        // DOI 10.1016/j.chemphys.2008.10.036, EQ. 7
        // algorithm can be found in Scheme 1 of DOI 10.1016/j.chemphys.2008.10.036
        std::vector<SharedMatrix> G_block(njk);
        for(size_t jki = 0; jki < njk; jki++) {
            G_block[jki] = std::make_shared<Matrix>(nbf_block_all, npoints_block);
        }

        if(rank == 0) timer_on("ESP Integrals");

        const auto & int_buff = int_computers[rank]->buffers()[0];

        // lambda for returning sign of double
        // needed for formation of G
        auto sign = [ ](double val) {
            return (val >= 0.0) ? 1.0 : -1.0;
        };

        // calculate A_NU_TAU at all grid points in this block
        // contract A_NU_TAU with F_TAU to get G_NU
        for (size_t TAU : shell_map_tau) {
            const size_t num_tau = primary_->shell(TAU).nfunction();
            const size_t tau_start = shell_map_all_to_bf_map_all[TAU];
            const size_t center_TAU = primary_->shell_to_center(TAU);
            const double x_TAU = primary_->molecule()->x(center_TAU);
            const double y_TAU = primary_->molecule()->y(center_TAU);
            const double z_TAU = primary_->molecule()->z(center_TAU);

            // TAU -> NU sparity determined by shell extents
            for (size_t NU : shell_extent_map[TAU]) {
                const size_t num_nu = primary_->shell(NU).nfunction();
                const size_t nu_start = shell_map_all_to_bf_map_all[NU];
                const size_t center_NU = primary_->shell_to_center(NU);
                const double x_NU = primary_->molecule()->x(center_NU);
                const double y_NU = primary_->molecule()->y(center_NU);
                const double z_NU = primary_->molecule()->z(center_NU);

                // is this value of NU also a possible value of TAU for this grid block?
                // i.e. can we use permutational symmetry of this (NU|TAU) integral shell pair?
                bool symm = (NU != TAU) && std::binary_search(shell_map_tau.begin(), shell_map_tau.end(), NU);

                // we've already done these integrals
                if (symm && TAU > NU) continue;

                // benchmarking
                int_shells_total += npoints_block;

                // can we screen the whole block over K_uv = (X_ug (A_vtg (F_tg)) upper bound?
                double k_bound = X_block_max * esp_boundp[NU][TAU] * F_block_gmaxp[TAU];
                if (symm) k_bound = std::max(k_bound, X_block_max * esp_boundp[TAU][NU] * F_block_gmaxp[NU]);
                if (k_bound < kscreen) continue;

                for (size_t g = 0; g < npoints_block; g++) {

                    // grid-point specific screening
                    // account for the distance between the grid point and the shell pair
                    double dist_TAU_g = std::sqrt((x_TAU - x[g]) * (x_TAU - x[g]) + (y_TAU - y[g]) * (y_TAU - y[g]) + (z_TAU - z[g]) * (z_TAU - z[g]));
                    double dist_NU_g = std::sqrt((x_NU - x[g]) * (x_NU - x[g]) + (y_NU - y[g]) * (y_NU - y[g]) + (z_NU - z[g]) * (z_NU - z[g]));
                    double dist_NUTAU_g = std::min(dist_TAU_g - shell_extents->get(TAU), dist_NU_g - shell_extents->get(NU));
                    double dist_decay = 1.0 / std::max(1.0, dist_NUTAU_g);

                    // can we screen this single point over K_uv = (X_ug (A_vtg (F_tg))) upper bound?
                    k_bound = X_block_bfmaxp[g] * esp_boundp[NU][TAU] * dist_decay * F_block_shellp[g][TAU];
                    if (symm) k_bound = std::max(k_bound, X_block_bfmaxp[g] * esp_boundp[TAU][NU] * dist_decay * F_block_shellp[g][NU]);
                    if (k_bound < kscreen) continue;

                    // calculate pseudospectral integral shell pair (A_NU_TAU) at gridpoint g
                    int_computers[rank]->set_origin({x[g], y[g], z[g]});
                    int_computers[rank]->compute_shell(NU, TAU);

                    // benchmarking
                    int_shells_computed++;

                    // contract A_nu_tau with F_tau to get contribution to G_nu
                    // symmetry permitting, also contract A_nu_tau with F_nu to get contribution to G_tau
                    // we fold sign(w) into the formation of G to correct for the modified definition of X
                    for(size_t jki = 0; jki < njk; jki++) {
                        auto F_blockp = F_block[jki]->pointer();
                        auto G_blockp = G_block[jki]->pointer();
                        for (size_t nu = nu_start, index = 0; nu < (nu_start + num_nu); ++nu) {
                            for (size_t tau = tau_start; tau < (tau_start + num_tau); ++tau, index++) {
                                G_blockp[nu][g] += sign(w[g]) * int_buff[index] * F_blockp[g][tau];
                                if (symm) G_blockp[tau][g] += sign(w[g]) * int_buff[index] * F_blockp[g][nu];
                            }
                        }
                    }

                }
            }

        }

        if(rank == 0) timer_off("ESP Integrals");

        // Contract X (or Q if overlap fitting) with G to get contribution to K
        for(size_t jki = 0; jki < njk; jki++) {
            SharedMatrix KT_block;
            if (overlap_fitted) {
                KT_block = linalg::doublet(Q_block, G_block[jki], true, true);
            } else {
                KT_block = linalg::doublet(X_block, G_block[jki], true, true);
            }
            auto KT_blockp = KT_block->pointer();
            auto KTp = KT[jki][rank]->pointer();
            for(size_t mu_ind = 0; mu_ind < bf_map.size(); mu_ind++) {
                size_t mu = bf_map[mu_ind];
                for(size_t nu_ind = 0; nu_ind < bf_map_all.size(); nu_ind++) {
                    size_t nu = bf_map_all[nu_ind];
                    KTp[mu][nu] += KT_blockp[mu_ind][nu_ind];
                }
            }
        }

    }

    timer_off("Grid Loop");

    // Reduce per-thread contributions
    for(size_t jki = 0; jki < njk; jki++) {
        for (size_t thread = 0; thread < nthreads_; thread++) {
            K[jki]->add(KT[jki][thread]);
        }
        if (lr_symmetric_) {
            K[jki]->hermitivitize();
        }
    }

    num_computed_shells_ = int_shells_computed;
    if (get_bench()) {
        computed_shells_per_iter_["Quartets"].push_back(num_computed_shells());
    }
}

}  // namespace psi
