#ifndef RAPTER_SOLVER_HPP
#define RAPTER_SOLVER_HPP

#include "Eigen/Sparse"

#ifdef RAPTER_USE_PCL
#   include <pcl/console/parse.h>
#endif // RAPTER_USE_PCL

#ifdef RAPTER_WITH_BONMIN
#   include "qcqpcpp/bonminOptProblem.h"
#endif

#include "rapter/util/diskUtil.hpp"                 // saveBAckup
#include "rapter/util/util.hpp"                     // timestamp2Str

#include "rapter/io/io.h"
//#include "rapter/optimization/candidateGenerator.h" // generate()
//#include "rapter/optimization/energyFunctors.h"     // PointLineDistanceFunctor,
#include "rapter/optimization/problemSetup.h"         // everyPatchNeedsDirection()
#include "rapter/processing/diagnostic.hpp"           // Diagnostic
#include "rapter/processing/impl/angleUtil.hpp"

namespace rapter
{

//! \brief      Step 3. Reads a formulated problem from path and runs qcqpcpp::OptProblem::optimize() on it.
//! \param argc Number of command line arguments.
//! \param argv Vector of command line arguments.
//! \return     Exit code. 0 == EXIT_SUCCESS.
template <class _PrimitiveContainerT
         , class _InnerPrimitiveContainerT
         , class _PrimitiveT
         >
int
Solver::solve( int    argc
             , char** argv )
{
    int                                   err           = EXIT_SUCCESS;

    bool                                  verbose       = false;
    enum SOLVER { MOSEK, BONMIN, GUROBI } solver        = MOSEK;
    std::string                           project_path  = "problem", solver_str = "bonmin";
    Scalar                                max_time      = 360;
    int                                   bmode         = 0; // Bonmin solver mode, B_Bb by default
    std::string                           rel_out_path  = ".";
    std::string                           x0_path       = "";
    int                                   attemptCount  = 0;
    std::string                           energy_path        = "energy.csv";

    // parse
    {
        bool valid_input = true;

        // verbose parsing
        if ( pcl::console::find_switch(argc,argv,"-v") || pcl::console::find_switch(argc,argv,"--verbose") )
            verbose = true;

        // solver parsing
        //std::string solver_str = "bonmin";
        valid_input &= (pcl::console::parse_argument( argc, argv, "--solver", solver_str) >= 0) || (pcl::console::parse_argument( argc, argv, "--solver3D", solver_str) >= 0);
        {
                 if ( !solver_str.compare("mosek")  ) solver = MOSEK;
            else if ( !solver_str.compare("bonmin") ) solver = BONMIN;
            else if ( !solver_str.compare("gurobi") ) solver = GUROBI;
            else
            {
                std::cerr << "[" << __func__ << "]: " << "Cannot parse solver " << solver_str << std::endl;
                valid_input = false;
            }
#           ifndef RAPTER_WITH_BONMIN
                 if ( solver == BONMIN )
                     throw new std::runtime_error("You have to specify a different solver, the project was not compiled with Bonmin enabled!");
#           endif // WITH_BONMIN
        }
        // problem parsing
        pcl::console::parse_argument( argc, argv, "--problem", project_path );
        // parse time
        pcl::console::parse_argument( argc, argv, "--time", max_time );
        // parse bonmin solver mode
        pcl::console::parse_argument( argc, argv, "--bmode", bmode );
        pcl::console::parse_argument( argc, argv, "--rod"  , rel_out_path );

        // X0
        if (pcl::console::parse_argument( argc, argv, "--x0", x0_path ) >= 0)
        {
            if ( !boost::filesystem::exists(x0_path) )
            {
                std::cerr << "[" << __func__ << "]: " << "X0 path does not exist! " << x0_path << std::endl;
                valid_input = false;
            }
        }

        // usage print
        std::cerr << "[" << __func__ << "]: " << "Usage:\t gurobi_opt\n"
                  << "\t--solver *" << solver_str << "* (mosek | bonmin | gurobi)\n"
                  << "\t--problem " << project_path << "\n"
                  << "\t[--time] " << max_time << "\n"
                  << "\t[--bmode *" << bmode << "*\n"
                         << "\t\t0 = B_BB, Bonmin\'s Branch-and-bound \n"
                         << "\t\t1 = B_OA, Bonmin\'s Outer Approximation Decomposition\n"
                         << "\t\t2 = B_QG, Bonmin's Quesada & Grossmann branch-and-cut\n"
                         << "\t\t3 = B_Hyb Bonmin's hybrid outer approximation\n"
                         << "\t\t4 = B_Ecp Bonmin's implemantation of ecp cuts based branch-and-cut a la FilMINT\n"
                         << "\t\t5 = B_IFP Bonmin's implemantation of iterated feasibility pump for MINLP]\n"
                  << "\t[--verbose] " << "\n"
                  << "\t[--rod " << rel_out_path << "]\t\t Relative output directory\n"
                  << "\t[--x0 " << x0_path << "]\t Path to starting point sparse matrix\n"
                  << "\t[--help, -h] "
                  << std::endl;

        // valid_input
        if ( !valid_input || pcl::console::find_switch(argc,argv,"--help") || pcl::console::find_switch(argc,argv,"-h") )
        {
            std::cerr << "[" << __func__ << "]: " << "--solver is compulsory" << std::endl;
            err = EXIT_FAILURE;
        } //...if valid_input
    } //...parse

    err = DO_RETRY; // flip to enter
    while ( (err == DO_RETRY) && (attemptCount < 2) )
    {
        err = EXIT_SUCCESS; // reset flag

        // select solver
        typedef double OptScalar; // Mosek, and Bonmin uses double internally, so that's what we have to do...
        typedef qcqpcpp::OptProblem<OptScalar>  OptProblemT;
        typedef OptProblemT::SparseMatrix       SparseMatrix;
        OptProblemT *p_problem = NULL;
        if ( EXIT_SUCCESS == err )
        {
            switch ( solver )
            {
                case BONMIN:
                    p_problem = new qcqpcpp::BonminOpt<OptScalar>();
                    break;

                default:
                    std::cerr << "[" << __func__ << "]: " << "Unrecognized solver type, exiting" << std::endl;
                    err = EXIT_FAILURE;
                    break;
            } //...switch
        } //...select solver

        // problem.read()
        if ( EXIT_SUCCESS == err )
        {
            err += p_problem->read( project_path );
            if ( EXIT_SUCCESS != err )
                std::cerr << "[" << __func__ << "]: " << "Could not read problem, exiting" << std::endl;
        } //...problem.read()PrimitiveT

        // problem.parametrize()
        {
            if ( max_time > 0 )
                p_problem->setTimeLimit( max_time );
            if ( solver == BONMIN )
            {
#           ifdef RAPTER_WITH_BONMIN
                qcqpcpp::BonminOpt<OptScalar>* p_bonminProblem = static_cast<qcqpcpp::BonminOpt<OptScalar>*>(p_problem);
                p_bonminProblem->setAlgorithm( Bonmin::Algorithm(bmode) );
                std::cout << "[" << __func__ << "]: " << "setting attemptCount to " << (1 + attemptCount) * 100 << std::endl;
                p_bonminProblem->setNodeLimit( (1 + attemptCount) * 100 );
//                if ( attemptCount )
//                {
//                    p_bonminProblem->setMaxSolutions( 1 ); // this is evil
//                }
                OptProblemT::SparseMatrix x0;
                if ( !x0_path.empty() )
                {
                    x0 = qcqpcpp::io::readSparseMatrix<OptScalar>( x0_path, 0 );
                    static_cast<qcqpcpp::BonminOpt<OptScalar>*>(p_problem)->setStartingPoint( x0 );
                }

#           endif // WITH_BONMIN
            }
        }

        // problem.update()
        OptProblemT::ReturnType r = 0;
        if ( EXIT_SUCCESS == err )
        {
            // log
            if ( verbose ) { std::cout << "[" << __func__ << "]: " << "calling problem update..."; fflush(stdout); }

            // update
            r = p_problem->update();

            // log
            if ( verbose ) { std::cout << "[" << __func__ << "]: " << "problem update finished\n"; fflush(stdout); }
        } //...problem.update()

        // problem.optimize()
        if ( EXIT_SUCCESS == err )
        {
            // optimize
            std::vector<OptScalar> x_out;
            if ( r == p_problem->getOkCode() )
            {
                // log
                if ( verbose ) { std::cout << "[" << __func__ << "]: " << "calling problem optimize...\n"; fflush(stdout); }

                // work
                r = p_problem->optimize( &x_out, OptProblemT::OBJ_SENSE::MINIMIZE );

                // check output
                if ( r != p_problem->getOkCode() )
                {
                    std::cerr << "[" << __func__ << "]: " << "ooo...optimize didn't work with code " << r << std::endl; fflush(stderr);
                    err = r;
                }
            } //...optimize

            if ( !x_out.size() || std::accumulate(x_out.begin(),x_out.end(),0) == 0 )
            {
                std::cerr << "No output from optimizer, exiting" << std::endl;
                {
                    err = DO_RETRY;
                    ++attemptCount;
                }
            }
            else
                std::cout << "accumulate: " << std::accumulate(x_out.begin(),x_out.end(),0) << std::endl;

            // copy output
            std::vector<Scalar> scalar_x_out( x_out.size() );
            if ( EXIT_SUCCESS == err )
            {
                // copy
                std::copy( x_out.begin(), x_out.end(), scalar_x_out.begin() );
            } //...copy output

            // dump
            if ( EXIT_SUCCESS != err ) // by Aron 27/12/2014
            {
                std::cout << "err is " << err << ", no saving will happen..." << std::endl;
            }
            if ( EXIT_SUCCESS == err)
            {
                Diagnostic<OptScalar> diag( p_problem->getLinObjectivesMatrix(), p_problem->getQuadraticObjectivesMatrix() );
                {
                    std::string x_path = project_path + "/x.csv";
                    OptProblemT::SparseMatrix sp_x( x_out.size(), 1 ); // output colvector
                    for ( size_t i = 0; i != x_out.size(); ++i )
                    {
                        if ( int(round(x_out[i])) > 0 )
                        {
                            sp_x.insert(i,0) = x_out[i];
                        }
                    }
                    qcqpcpp::io::writeSparseMatrix<OptScalar>( sp_x, x_path, 0 );
                    std::cout << "[" << __func__ << "]: " << "wrote output to " << x_path << std::endl;
                }

                std::string candidates_path;
                if ( pcl::console::parse_argument( argc, argv, "--candidates", candidates_path ) >= 0 )
                {
                    // read primitives
                    _PrimitiveContainerT prims;
                    {
                        if ( verbose ) std::cout << "[" << __func__ << "]: " << "reading primitives from " << candidates_path << "...";
                        io::readPrimitives<_PrimitiveT, _InnerPrimitiveContainerT>( prims, candidates_path );
                        if ( verbose ) std::cout << "reading primitives ok\n";
                    } //...read primitives

                    // save selected primitives
                    _PrimitiveContainerT out_prims( 1 );
                    LidT prim_id = 0;
                    for ( size_t l = 0; l != prims.size(); ++l )
                        for ( size_t l1 = 0; l1 != prims[l].size(); ++l1 )
                        {
                            if ( prims[l][l1].getTag( _PrimitiveT::TAGS::STATUS ) == _PrimitiveT::STATUS_VALUES::SMALL )
                            {
                                // copy small, keep for later iterations
                                out_prims.back().push_back( prims[l][l1] );
                            }
                            else
                            {
                                // copy to output, only, if chosen
                                if ( int(round(x_out[prim_id])) > 0 )
                                {
                                    //std::cout << "saving " << prims[l][l1].getTag(_PrimitiveT::TAGS::GID) << ", " << prims[l][l1].getTag(_PrimitiveT::TAGS::DIR_GID) << ", X: " << x_out[prim_id] << "\t, ";
                                    prims[l][l1].setTag( _PrimitiveT::TAGS::STATUS, _PrimitiveT::STATUS_VALUES::ACTIVE );
                                    out_prims.back().push_back( prims[l][l1] );

                                    // diagnostic // 5/1/2015
                                    char name[256];
                                    sprintf( name,"p%ld,%ld", prims[l][l1].getTag(_PrimitiveT::TAGS::GID),prims[l][l1].getTag(_PrimitiveT::TAGS::DIR_GID) );
                                    diag.setNodeName( prim_id, name );
                                    diag.setNodePos( prim_id, prims[l][l1].template pos() );
                                }

                                // increment non-small primitive ids
                                ++prim_id;
                            }
                        } // ... for l1
                    std::cout << std::endl;

                    const LidT clusterVarsStart = prim_id;
                    std::cout << "clusterVarsStart: " << clusterVarsStart << std::endl;
                    // add rest of nodes (cluster_nodes)
                    {
                        for ( ; prim_id < static_cast<LidT>(x_out.size()); ++prim_id )
                        {
                            char name[256];
                            if ( int(round(x_out[prim_id])) > 0 )
                            {
                                sprintf( name,"%ld_on", prim_id );
                                diag.setNodeName( prim_id, name );
                            }
                            else
                            {
                                sprintf( name,"%ld_off", prim_id );
                            }
                        }
                    }

                    // go over constraints
                    {
                        typedef typename OptProblemT::SparseMatrix SparseMatrix;
                        for ( LidT j = 0; j != static_cast<LidT>(p_problem->getConstraintCount()); ++j )
                        {
                            SparseMatrix Qk = p_problem->getQuadraticConstraintsMatrix( j );
                            if ( !Qk.nonZeros() ) continue;

                            for ( LidT row = 0; row != Qk.outerSize(); ++row )
                            {
                                for ( typename SparseMatrix::InnerIterator it(Qk,row); it; ++it )
                                {
                                    if ( it.value() != 0. )
                                        diag.addEdge( it.row(), it.col() );
                                }
                            }
                        }
                    }

                    std::string parent_path = boost::filesystem::path(candidates_path).parent_path().string();
                    if ( parent_path.empty() )  parent_path = "./";
                    else                        parent_path += "/";

                    std::string out_prim_path = parent_path + rel_out_path + "/primitives." + solver_str + ".csv";
                    {
                        int iteration = 0;
                        iteration = std::max(0,util::parseIteration(candidates_path) );
                        {
                            std::stringstream ss;
                            ss << parent_path + rel_out_path << "/primitives_it" << iteration << "." << solver_str << ".csv";
                            out_prim_path = ss.str();
                        }

                        {
                            std::stringstream ss;
                            ss << parent_path + rel_out_path << "/diag_it" << iteration << ".gv";
                            diag.draw( ss.str(), false );
                        }
                    }

                    util::saveBackup    ( out_prim_path );
                    io::savePrimitives<_PrimitiveT, typename _InnerPrimitiveContainerT::const_iterator>( out_prims, out_prim_path, /* verbose: */ true );

                } // if --candidates
                else
                {
                    std::cout << "[" << __func__ << "]: " << "You didn't provide candidates, could not save primitives" << std::endl;
                } // it no --candidates

                // calc Energy
                {
                    std::string parent_path = boost::filesystem::path(project_path).parent_path().string();
                    SparseMatrix xOut( x_out.size(), 1 );
                    size_t varId = 0;
                    for ( auto it = x_out.begin(); it != x_out.end(); ++it, ++varId )
                        xOut.insert( varId, 0 ) = *it;

                    OptScalar dataC     = (xOut.transpose() * p_problem->getLinObjectivesMatrix()).eval().coeff(0,0);
                    OptScalar pairwiseC = (xOut.transpose() * p_problem->getQuadraticObjectivesMatrix() * xOut ).eval().coeff(0,0);
                    std::cout << "E = " << dataC + pairwiseC << " = "
                              << dataC << " (data) + " << pairwiseC << "(pw)"
                              << std::endl;
                    std::ofstream fenergy( parent_path + "/" + energy_path, std::ofstream::out | std::ofstream::app );
                    fenergy << dataC + pairwiseC << "," << dataC << "," << pairwiseC << "," << 0 << std::endl;
                    fenergy.close();
                } //...calcEnergy
            } //...if exit_success (solved)

        } //...problem.optimize()

        if ( p_problem ) { delete p_problem; p_problem = NULL; }
    } //...err == doRetry || exit_SUCCESS

    return err;
}

//! \brief Unfinished function. Supposed to do GlobFit.
template < class _PrimitiveContainerT
         , class _InnerPrimitiveContainerT
         , class _PrimitiveT
>
int
Solver::datafit( int    argc
               , char** argv )
{
    int                     err             = EXIT_SUCCESS;
    Scalar                  scale           = 0.05f;
    std::string             cloud_path      = "cloud.ply",
                            primitives_path = "candidates.csv",
                            associations_path = "";
    AnglesT                 angle_gens( {AnglesT::Scalar(90.)} );
    bool                    verbose         = false;

    // parse params
    {
        bool valid_input = true;
        valid_input &= pcl::console::parse_argument( argc, argv, "--scale"     , scale          ) >= 0;
        valid_input &= pcl::console::parse_argument( argc, argv, "--cloud"     , cloud_path     ) >= 0;
        valid_input &= pcl::console::parse_argument( argc, argv, "--primitives", primitives_path) >= 0;
        pcl::console::parse_x_arguments( argc, argv, "--angle-gens", angle_gens );
        if ( pcl::console::find_switch(argc,argv,"-v") || pcl::console::find_switch(argc,argv,"--verbose") )
            verbose = true;

        if (    (pcl::console::parse_argument( argc, argv, "-a", associations_path) < 0)
             && (pcl::console::parse_argument( argc, argv, "--assoc", associations_path) < 0)
             && (!boost::filesystem::exists(associations_path)) )
        {
            std::cerr << "[" << __func__ << "]: " << "-a or --assoc is compulsory" << std::endl;
            valid_input = false;
        }

        std::cerr << "[" << __func__ << "]: " << "Usage:\t gurobi_opt --gfit\n"
                  << "\t--scale " << scale << "\n"
                  << "\t--cloud " << cloud_path << "\n"
                  << "\t--primitives " << primitives_path << "\n"
                  << "\t--a,--assoc " << associations_path << "\n"
                  << "\t--no-paral\n"
                  << "\t[--angle-gens "; for(size_t i=0;i!=angle_gens.size();++i)std::cerr<<angle_gens[i]<<",";std::cerr<< "]\n";
        std::cerr << std::endl;

        if ( !valid_input || pcl::console::find_switch(argc,argv,"--help") || pcl::console::find_switch(argc,argv,"-h") )
        {
            std::cerr << "[" << __func__ << "]: " << "--scale, --cloud and --primitives are compulsory" << std::endl;
            return EXIT_FAILURE;
        }
    } // ... parse params
    // read points
    PointContainerT     points;
    {
        if ( verbose ) std::cout << "[" << __func__ << "]: " << "reading cloud from " << cloud_path << "...";
        io::readPoints<PointPrimitiveT>( points, cloud_path );
        std::vector<std::pair<PidT,LidT> > points_primitives;
        io::readAssociations( points_primitives, associations_path, NULL );
        for ( size_t i = 0; i != points.size(); ++i )
        {
            // store association in point
            points[i].setTag( PointPrimitiveT::TAGS::GID, points_primitives[i].first );
        }
        if ( verbose ) std::cout << "reading cloud ok\n";
    } //...read points

    // read primitives
    typedef std::map<GidT, _InnerPrimitiveContainerT> PrimitiveMapT;
    _PrimitiveContainerT prims;
    PrimitiveMapT       patches;
    {
        if ( verbose ) std::cout << "[" << __func__ << "]: " << "reading primitives from " << primitives_path << "...";
        io::readPrimitives<_PrimitiveT, _InnerPrimitiveContainerT>( prims, primitives_path, &patches );
        if ( verbose ) std::cout << "reading primitives ok\n";
    } //...read primitives

    // Read desired angles
    bool no_paral = pcl::console::find_switch( argc, argv, "--no-paral");
    AnglesT angles;
    {
        angles::appendAnglesFromGenerators( angles, angle_gens, no_paral, true );
    } // ... read angles

    return err;
} // ...Solver::datafit()

//! \brief              Prints energy of solution in \p x using \p weights. Unused for now.
//! \param[in] x        A solution to calculate the energy of.
//! \param[in] weights  Problem weights used earlier. \todo Dump to disk together with solution.
Eigen::Matrix<rapter::Scalar,3,1>
Solver::checkSolution( std::vector<Scalar> const& x
                     , Solver::SparseMatrix const& linObj
                     , Solver::SparseMatrix const& Qo
                     , Solver::SparseMatrix const& /* A */
                     , Eigen::Matrix<Scalar,3,1> const& weights )
{
    Eigen::Matrix<Scalar,3,1> energy; energy.setZero();

    SparseMatrix complexity( x.size(), 1 );
    for ( size_t row = 0; row != x.size(); ++row )
        complexity.insert( row, 0 ) = weights(2);

    // X
    SparseMatrix mx( x.size(), 1 );
    for ( size_t i = 0; i != x.size(); ++i )
        mx.insert( i, 0 ) = x[i];

    SparseMatrix data   = linObj - complexity;

    // qo
    SparseMatrix e02 = mx.transpose() * linObj;
    std::cout << "[" << __func__ << "]: " << "qo * x = " << e02.coeffRef(0,0) << std::endl; fflush(stdout);

    // datacost
    SparseMatrix e0 = (mx.transpose() * data);
    energy(0) = e0.coeffRef(0,0);
    //std::cout << "[" << __func__ << "]: " << "data: " << energy(0) << std::endl; fflush(stdout);

    // Qo
    SparseMatrix e1 = mx.transpose() * Qo * mx;
    energy(1) = e1.coeffRef(0,0);
    //std::cout << "[" << __func__ << "]: " << "x' * Qo * x = pw = " << energy(1) << std::endl; fflush(stdout);

    // complexity
    SparseMatrix e2 = mx.transpose() * complexity;
    energy(2) = e2.coeffRef(0,0);
    //std::cout << "[" << __func__ << "]: " << "complx = " << energy(2) << std::endl; fflush(stdout);
    std::cout << "[" << __func__ << "]: " << std::setprecision(9) << energy(0) << " + " << energy(1) << " + " << energy(2) << " = " << energy.sum();
    std::cout                             << std::setprecision(9) << weights(0) << " * " << energy(0)/weights(0)
                                                                  << " + " << weights(1) << " * " << energy(1) / weights(1)
                                                                  << " + " << weights(2) << " * " << energy(2) / weights(2)
                                                                  << std::endl;

    return energy;
} //...checkSolution

} // ... ns rapter

#endif // RAPTER_SOLVER_HPP
