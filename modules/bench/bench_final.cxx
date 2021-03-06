#include <config.hpp>

#include "bench_config.hpp"

#include <fstream>
#include <utils/filesystem.hpp>
#include <utils/numerics.hpp>
#include <utils/dataset.hpp>
#include <utils/misc.hpp>
#include <utils/vision.hpp>
#include <utils/logger.hpp>
#include <utils/image.hpp>
#include <utils/cycletimer.hpp>
#include <search/bag_of_words/bag_of_words.hpp>
#include <search/inverted_index/inverted_index.hpp>
#include <search/vocab_tree/vocab_tree.hpp>
#include <search/search_base/search_base.hpp>
#include <vis/matches_page.hpp>

#if ENABLE_MULTITHREADING && ENABLE_OPENMP
#include <omp.h>
#endif
#if ENABLE_MULTITHREADING && ENABLE_MPI
#include <mpi.h>
#endif

_INITIALIZE_EASYLOGGINGPP


void validate_results(Dataset &dataset, PTR_LIB::shared_ptr<const SimpleDataset::SimpleImage> &query_image, 
PTR_LIB::shared_ptr<MatchResultsBase> &matches, MatchesPage &html_output, std::ofstream &valid_out) {

#if ENABLE_MULTITHREADING && ENABLE_MPI
    int rank, procs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &procs);
    if (rank != 0)
      return;
#endif
	cv::Mat keypoints_0, descriptors_0;
	const std::string &query_keypoints_location = dataset.location(query_image->feature_path("keypoints"));
	const std::string &query_descriptors_location = dataset.location(query_image->feature_path("descriptors"));
	filesystem::load_cvmat(query_keypoints_location, keypoints_0);
	filesystem::load_cvmat(query_descriptors_location, descriptors_0);
 	uint32_t num_validate = std::min(64, (int)(matches->matches.size()));
  std::vector<int> validated(num_validate, 0); 
  uint32_t number_valid = 0;
#if ENABLE_MULTITHREADING && ENABLE_OPENMP
	#pragma omp parallel for schedule(dynamic)
#endif
	for(int32_t j=0; j<(int32_t)num_validate; j++) {
		cv::Mat keypoints_1, descriptors_1;
		PTR_LIB::shared_ptr<SimpleDataset::SimpleImage> match_image = std::static_pointer_cast<SimpleDataset::SimpleImage>(dataset.image(matches->matches[j]));
		const std::string &match_keypoints_location = dataset.location(match_image->feature_path("keypoints"));
		const std::string &match_descriptors_location = dataset.location(match_image->feature_path("descriptors"));
		filesystem::load_cvmat(match_keypoints_location, keypoints_1);
		filesystem::load_cvmat(match_descriptors_location, descriptors_1);

		cv::detail::MatchesInfo match_info;
		vision::geo_verify_f(descriptors_0, keypoints_0, descriptors_1, keypoints_1, match_info);

		validated[j] = vision::is_good_match(match_info) ? 1 : -1;
    if(validated[j] == 1)
      number_valid++;
	}
  for(int i=0;i<validated.size(); i++) {
    valid_out << validated[i] << "\t";
  }
  valid_out << std::endl;
  

	html_output.add_match(query_image->id, matches->matches, dataset, PTR_LIB::make_shared< std::vector<int> >(validated));
	html_output.write(dataset.location() + "/results/matches/");
}

void bench_dataset(SearchBase &searcher, SimpleDataset &dataset, const std::shared_ptr<SearchParamsBase> &params, std::ofstream &valid_out) {
	uint32_t num_searches = 256;


#if ENABLE_MULTITHREADING && ENABLE_MPI
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#endif

	MatchesPage html_output;
	std::cout<< "Current list: " << std::endl;	

	const std::vector<PTR_LIB::shared_ptr<const Image> > &rand_images = dataset.random_images(num_searches);
	for(uint32_t i=0; i<num_searches; i++) {
    std::cout << i << std::endl;
    // if(i==16||i==101||i==136||i==140||i==198) continue;
    try{
      // ...

      PTR_LIB::shared_ptr<const SimpleDataset::SimpleImage> query_image =  std::static_pointer_cast<const SimpleDataset::SimpleImage>(rand_images[i]);

      PTR_LIB::shared_ptr<MatchResultsBase> matches = searcher.search(dataset, params, query_image);

      if(matches == PTR_LIB::shared_ptr<MatchResultsBase>()) {
        //LERROR << "Error while running search.";
        std::cout << " ERROR ";
        continue;
         }
      // validate matches
      validate_results(dataset, query_image, matches, html_output, valid_out);
      std::stringstream ss;
      ss << "/home/psastras/vocabtree/data/oxford/perf/perf." <<  std::setw(5) << std::setfill('0') << i;
      std::cout << ss.str() << std::endl;
      PerfTracker::instance().save(ss.str());
    } catch (const cv::Exception &e) {
      std::cout << "oops" << std::endl;

    }  catch (const std::exception& ex) {
      std::cout << "oops" << std::endl;
    }
	  
		
	}
	std::cout << std::endl;

}

int main(int argc, char *argv[]) {
#if ENABLE_MULTITHREADING && ENABLE_MPI
  MPI_Init(&argc, &argv);
  int rank;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#endif

  // expects arguments of the form: int [int] data_directory database_location output_location
  if (argc != 5 && argc != 7) {
    std::cout << "usage: " << argv[0] << " num_clusters/split [depth numberTrainImages] data_directory database_location output_location\n";
    return 0;
  }
  int vTree = (argc > 5);
  vTree *= 2  ;
  const std::string data_dir = argv[2 + vTree];
  const std::string database_location = argv[3 + vTree];
  const std::string output_loc = argv[4 + vTree]; 
  
  std::stringstream ss;
  ss << "";
#if ENABLE_MULTITHREADING && ENABLE_MPI
  ss << ".N"<<rank;
#endif

  const std::string nodeID = ss.str();

  const std::string machine_out_loc = output_loc + "/info" + nodeID + ".machine";

  const std::string valid_out_loc = output_loc + "/validated" + nodeID + ".txt";
  std::ofstream valid_out(valid_out_loc.c_str(), std::ios::trunc);

  filesystem::create_file_directory(output_loc + "/foo.txt");

  SimpleDataset train_dataset(data_dir, database_location, 0);
  size_t cache_sizes[] = { 1024};
  int numSizes = 1;

  if (vTree) {
    VocabTree vt;
    std::shared_ptr<VocabTree::TrainParams> train_params = std::make_shared<VocabTree::TrainParams>();
    train_params->depth = atoi(argv[2]);
    train_params->split = atoi(argv[1]);
    int numImages = atoi(argv[3]);

    std::shared_ptr<VocabTree::SearchParams> searchParams = std::make_shared<VocabTree::SearchParams>();

    searchParams->amountToReturn = 64;
    //searchParams->cutoff = 10;

    const std::string &tree_root_location = train_dataset.location("tree/");
    filesystem::create_file_directory(tree_root_location);
    std::stringstream ss;
    ss << tree_root_location << "tree." << train_params->split << "." << train_params->depth << ".bin";
    const std::string &tree_location = ss.str();

    if (filesystem::file_exists(tree_location)) {
      vt.load(tree_location);
    }
    else {
      std::cout << "No tree found at " << tree_location << ", building..." << std::endl;
      const std::vector<PTR_LIB::shared_ptr<const Image> > &rand_images = train_dataset.all_images();//(num_searches);
      const std::vector<PTR_LIB::shared_ptr<const Image> > subset(rand_images.begin(), rand_images.begin() + numImages);
      vt.train(train_dataset, train_params, subset);
      vt.save(tree_location);
    }
    // PerfTracker::instance().save(output_loc + "/perf" + nodeID + ".train");
    // PerfTracker::instance().clear();

    for (int i = 0; i < numSizes; i++) {
      size_t cache_size = cache_sizes[i];
      std::cout<< "Testing for cache size "<<cache_size<< std::endl;

      SimpleDataset dataset(data_dir, database_location, cache_size);
      LINFO << dataset;

      bench_dataset(vt, dataset, searchParams, valid_out);

      std::stringstream perfloc;
      perfloc << output_loc << "/perf" << nodeID << "." << cache_size;

      // PerfTracker::instance().save(perfloc.str());
      // PerfTracker::instance().clear();
    }
  }
  else {
    int num_clusters = atoi(argv[1]);
    std::stringstream index_output_file;
    index_output_file << train_dataset.location() << "/index/" << num_clusters << ".index";
    InvertedIndex ii(index_output_file.str());

    for (int i = 0; i < numSizes; i++) {
      size_t cache_size = cache_sizes[i];

      SimpleDataset dataset(data_dir, database_location, cache_size);
      LINFO << dataset;

      bench_dataset(ii, dataset, std::shared_ptr<SearchParamsBase>(), valid_out);


      std::stringstream perfloc;
      perfloc << output_loc << "/perf" << nodeID << "." << cache_size;

      PerfTracker::instance().save(perfloc.str());
      PerfTracker::instance().clear();
    }
  }

  
  // std::cout << "Writing " << machine_out_loc.c_str();
  std::ofstream machine_out(machine_out_loc.c_str(), std::ios::trunc);

  for(int i=0; i<argc; i++) {
    machine_out << argv[i] << "\t";
  }
  machine_out << std::endl;

  machine_out << "Machine " << misc::get_machine_name() << std::endl;
#if ENABLE_MULTITHREADING && ENABLE_OPENMP
  machine_out << "Threads " << omp_get_max_threads() << std::endl;
#endif
  int nodes = 1;
#if ENABLE_MULTITHREADING && ENABLE_MPI
  MPI_Comm_size(MPI_COMM_WORLD, &nodes);
#endif
  machine_out << "Nodes " << nodes << std::endl;
  if ((machine_out.rdstate() & std::ofstream::failbit) != 0)
    std::cout << "Failed to write " << machine_out_loc << std::endl;

  if ((valid_out.rdstate() & std::ofstream::failbit) != 0)
    std::cout << "Failed to write " << valid_out_loc << std::endl;


#if ENABLE_MULTITHREADING && ENABLE_MPI
	MPI_Finalize();
#endif
	return 0;
}
