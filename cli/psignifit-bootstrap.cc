#include "../src/psipp.h"
#include "cli.h"

#include "cli_utilities.h"

#include <cstdio>

int main ( int argc, char ** argv ) {
	// Parse command line
	cli_parser parser ( "psignifit-mapestimate [options] <file> [ <file> ... ]" );
	parser.add_option ( "-c", "psignifit core object to be used", "mw0.1" );
	parser.add_option ( "-s", "psignifit sigmoid object to be used", "logistic" );
	parser.add_option ( "-prior1", "prior for the first parameter (alpha,a,m,...)", "None" );
	parser.add_option ( "-prior2", "prior for the second parameter (beta,b,w,...)", "None" );
	parser.add_option ( "-prior3", "prior for the third parameter (lambda)", "Uniform(0,.1)" );
	parser.add_option ( "-prior4", "prior for the fourth parameter (gamma)", "Uniform(0,.1)" );
	parser.add_option ( "-nafc",   "number of response alternatives in forced choice designs (set this to 1 for yes-no tasks)", "2" );
	parser.add_option ( "-nsamples","number of bootstrap samples to be generated","2000" );
	parser.add_option ( "-o",      "write output to this file", "stdout" );
	parser.add_option ( "-cuts",   "cuts to be determined", "0.25,0.50,0.75" );
	parser.add_switch ( "-v", "display status messages", false );
	parser.add_switch ( "--summary", "write a short summary to stdout" );
	parser.add_switch ( "-e", "In yes-no tasks: set gamma==lambda", false );
	parser.add_switch ( "-nonparametric", "Use nonparametric bootstrap instead of the default parametric bootstrap", false );
	parser.add_switch ( "--matlab", "format output to be parsable by matlab", false );

	parser.parse_args ( argc, argv );

	// Set up the most important data
	bool verbose ( parser.getOptSet ( "-v" ) ), pmfshown ( false ), summary ( parser.getOptSet( "--summary" ) );;
	PsiData *data;
	PsiPsychometric *pmf;
	PsiOptimizer * opt;
	std::vector<double> theta;
	std::vector<double> cuts (getCuts ( parser.getOptArg("-cuts") ) );
	unsigned int i,j, ncuts(cuts.size()), nparams, nblocks;
	BootstrapList *bs_list;
	JackKnifeList *jk_list;
	unsigned int nsamples ( atoi ( parser.getOptArg("-nsamples").c_str() ) );
	double th;

	// Contents of the bootstrap lists
	std::vector< std::vector<double> > mcthres ( nsamples, cuts );
	std::vector< std::vector<double> > mcslopes ( nsamples, cuts );
	std::vector< std::vector<double> > *mcestimates;
	std::vector< std::vector<int> >    *mcdata;
	std::vector<double>   mcdeviance ( nsamples );
	std::vector<double>   mcRpd      ( nsamples );
	std::vector<double>   mcRkd      ( nsamples );
	std::vector<double>   bias_thres ( ncuts );
	std::vector<double>   acc_thres  ( ncuts );
	std::vector<double>   bias_slope ( ncuts );
	std::vector<double>   acc_slope  ( ncuts );
	std::vector<double> *influential;
	std::vector<int>    *outliers;
	std::vector<double> *ci_lower;
	std::vector<double> *ci_upper;
	std::vector<double> *devianceresiduals;

	// Use matlabformat?
	bool matlabformat ( parser.getOptSet ( "--matlab" ) );

	// Get the output file
	FILE * ofile;
	if ( !(parser.getOptArg ( "-o" ).compare( "stdout" )) ) {
		if ( verbose ) std::cout << "Writing results to stdout\n";
		ofile = stdout;
	} else 
		ofile = fopen ( parser.getOptArg ( "-o" ).c_str(), "w" );

	// Write some status messages
	if (verbose) {
		std::cout << "core:    " << parser.getOptArg ( "-c" ) << "\n";
		std::cout << "sigmoid: " << parser.getOptArg ( "-s" ) << "\n";
		std::cout << "cuts:    ";
		for (i=0; i<cuts.size(); i++) std::cout << cuts[i] << " ";
		std::cout << "\n";
		std::cout << "priors:\n";
		std::cout << "   prm1: " << parser.getOptArg ( "-prior1" ) << "\n";
		std::cout << "   prm2: " << parser.getOptArg ( "-prior2" ) << "\n";
		std::cout << "   prm3: " << parser.getOptArg ( "-prior3" ) << "\n";
		if ( atoi (parser.getOptArg("-nafc").c_str()) < 2 ) std::cout << "   prm4: " << parser.getOptArg ( "-prior4" ) << "\n";
		std::cout << "parametric bootstrap: " << (parser.getOptSet("-nonparametric")?"no":"yes");
		std::cout << "number of bootstrap samples: " << nsamples << "\n";
		if ( parser.getOptSet ( "-e" ) )
			std::cout << "gamma==lambda\n";
	}

	std::string fname;
	fname = parser.popArg ();
	if ( fname == "" ) {
		std::cerr << "No input file given --- aborting!\n";
		exit ( -1 );
	}

	while ( fname != "" ) {
		if ( verbose ) std::cerr << "Analyzing input file '" << fname << "'\n   ";

		// Get the data
		data = allocateDataFromFile ( fname, atoi ( parser.getOptArg ( "-nafc" ).c_str() ) );
		nblocks = data->getNblocks();

		if ( verbose ) std::cerr << "Read " << nblocks << " blocks ";

		if ( verbose ) {
			std::cerr << "Data:\n";
			for (i=0; i<nblocks; i++)
				std::cerr << i << " " << data->getIntensity ( i ) << " " << data->getNcorrect ( i ) << " " << data->getNtrials ( i ) << "\n";
		}

		// Get the psychometric function model
		pmf  = allocatePsychometric ( parser.getOptArg ( "-c" ),
			parser.getOptArg ( "-s" ),
			atoi ( parser.getOptArg ( "-nafc" ).c_str() ),
			data,
			verbose && !pmfshown);
		pmfshown = true;
		if ( parser.getOptSet ( "-e" ) ) pmf->setgammatolambda();
		setPriors ( pmf,
				parser.getOptArg ( "-prior1" ),
				parser.getOptArg ( "-prior2" ),
				parser.getOptArg ( "-prior3" ),
				parser.getOptArg ( "-prior4" ) );
		nparams = pmf->getNparams();

		// Determine starting value
		opt = new PsiOptimizer ( pmf, data );
		theta = opt->optimize ( pmf, data );

		// Sample
		if ( verbose ) {
			std::cout << "Starting sampling ...";
			std::cout << "bs...";
			std::cout.flush();
		}
		bs_list = new BootstrapList ( bootstrap ( atoi(parser.getOptArg("-nsamples").c_str()),
				data, pmf, cuts, &theta,true,!(parser.getOptSet("-nonparametric")) ) );
		if ( verbose ) { std::cout << "jk..."; std::cout.flush(); }
		jk_list = new JackKnifeList ( jackknifedata ( data, pmf ) );
		if ( verbose ) { std::cout << " Done"; std::cout.flush(); }

		if ( verbose ) std::cerr << "\n";

		// These might change during analysis and have to be allocated for each file
		mcestimates = new std::vector< std::vector<double> > (nsamples);
		mcdata      = new std::vector< std::vector<int> >    (nsamples);
		influential = new std::vector<double>                (nblocks);
		outliers    = new std::vector<int>                   (nblocks);
		ci_lower    = new std::vector<double>                (nparams);
		ci_upper    = new std::vector<double>                (nparams);

		// Now store everything that is related to inteval estimation of parameters
		for ( i=0; i<nsamples; i++ ) {
			(*mcestimates)[i] = bs_list->getEst ( i );
			(*mcdata)[i]      = bs_list->getData ( i );
			for ( j=0; j<ncuts; j++ ) {
				mcthres[i][j] = bs_list->getThres_byPos ( i, j );
				mcslopes[i][j] = bs_list->getSlope_byPos ( i, j );
			}
		}
		for ( j = 0; j<ncuts; j++ ) {
			bias_thres[j] = bs_list->getBias_t(j);
			acc_thres[j]  = bs_list->getAcc_t(j);
			bias_slope[j] = bs_list->getBias_s(j);
			acc_slope[j]  = bs_list->getAcc_s(j);
		}
		for ( i=0; i<nparams; i++ ) {
			(*ci_lower)[i] = bs_list->getPercentile ( .025, i );
			(*ci_upper)[i] = bs_list->getPercentile ( .975, i );
		}
		for ( i=0; i<nblocks; i++ ) {
			(*influential)[i] = jk_list->influential ( i, *ci_lower, *ci_upper );
			(*outliers)[i]     = jk_list->outlier ( i );
		}

		// Write a summary of the parameter estimation if requested.
		if ( summary ) {
			std::cout << "Parameter estimates:\n";
			std::cout << "--------------------\n";
			for ( i=0; i<nparams; i++ )
				std::cout << "parameter" << i+1 << " = " << theta[i] << " CI_95 = (" << (*ci_lower)[i] << "," << (*ci_upper)[i] << ")\n";
			std::cout << "\n";
			std::cout << "Threshold estimates:\n";
			std::cout << "--------------------\n";
			for ( i=0; i<ncuts; i++ ) {
				th = pmf->getThres ( theta, cuts[i] );
				std::cout << "Threshold(" << cuts[i] << ") = " << th << " CI_95 = ("
					<< bs_list->getThres(.025,cuts[i]) << ","
					<< bs_list->getThres(.975,cuts[i]) << ") ";
				std::cout << "Slope(" << cuts[i] << ") = " << pmf->getSlope ( theta, th ) << " CI_95 = ("
					<< bs_list->getSlope(.025,cuts[i]) << ","
					<< bs_list->getSlope(.975,cuts[i]) << ")\n";
			}
		}

		// If we performed nonparametric bootstrap, we can't use the bootstrap samples for goodness of fit
		// Here we perform a second, parametric bootstrap that gives the data for the goodness of fit assessment
		if ( parser.getOptSet("-nonparametric") ) {
			// redo bootstrap to obtain goodness of fit form parametric simulations
			delete bs_list;
			bs_list = new BootstrapList ( bootstrap ( atoi(parser.getOptArg("-nsamples").c_str()),
					data, pmf, cuts, &theta, true, true ) );
		}

		// Now store everything related to goodness of fit
		for ( i=0; i<nsamples; i++ ) {
			mcdeviance[i] = bs_list->getdeviance(i);
			mcRpd[i]      = bs_list->getRpd(i);
			mcRkd[i]      = bs_list->getRkd(i);
		}

		// Write a summary of the goodness of fit statistics if requested
		if ( summary ) {
			devianceresiduals = new std::vector<double> ( pmf->getDevianceResiduals ( theta, data ) );
			std::cout << "\n";
			std::cout << "Goodness of fit statistics:\n";
			std::cout << "---------------------------\n";
			std::cout << "Deviance: " << pmf->deviance ( theta, data ) <<             "   crit: " << bs_list->getDeviancePercentile ( .95 ) << "\n";
			std::cout << "Rpd: " << pmf->getRpd (
					*devianceresiduals, theta, data ) << "   crit: (" << bs_list->percRpd(.025) << "," << bs_list->percRpd(.975) << ")\n";
			std::cout << "Rkd: " << pmf->getRkd (
					*devianceresiduals, data ) <<        "   crit: (" << bs_list->percRkd(.025) << "," << bs_list->percRkd(.975) << ")\n";
			delete devianceresiduals;
		}

		// Now store everything in the output file.
		print ( *mcdata,      matlabformat, "mcdata",      ofile );
		print ( *mcestimates, matlabformat, "mcestimates", ofile );
		print ( mcdeviance,   matlabformat, "mcdeviance",  ofile );
		print ( mcthres,      matlabformat, "mcthres",     ofile );
		print ( mcslopes,     matlabformat, "mcslopes",    ofile );
		print ( mcRpd,        matlabformat, "mcRpd",       ofile );
		print ( mcRkd,        matlabformat, "mcRkd",       ofile );
		print ( *influential, matlabformat, "influential", ofile );
		print ( *outliers,    matlabformat, "outliers",    ofile );
		print ( bias_thres,   matlabformat, "bias_thres",  ofile );
		print ( bias_slope,   matlabformat, "bias_slope",  ofile );
		print ( acc_thres,    matlabformat, "acc_thres",   ofile );
		print ( acc_slope,    matlabformat, "acc_slope",   ofile );


		// Get the next input file (if there is one)
		fname = parser.popArg();

		// Clean up
		delete mcestimates;
		delete mcdata;
		delete influential;
		delete outliers;
		delete data;
		delete pmf;
		delete opt;
	}

	return 0;
}
