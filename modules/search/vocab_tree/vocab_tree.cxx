#include "vocab_tree.hpp"
#include <utils/filesystem.hpp>
#include <utils/vision.hpp>
#include <iostream>
#include <fstream>
#include <memory>
#include <math.h> // for pow
#include <utility> // std::pair

VocabTree::VocabTree() : SearchBase() {


}

// struct used for writing and reading cv::mat's
struct cvmat_header {
  uint64_t elem_size;
  int32_t elem_type;
  uint32_t rows, cols;
};

bool VocabTree::load (const std::string &file_path) {
	std::cout << "Reading vocab tree from " << file_path << "..." << std::endl;

  std::ifstream ifs(file_path, std::ios::binary);
  ifs.read((char *)&split, sizeof(uint32_t));
  ifs.read((char *)&maxLevel, sizeof(uint32_t));
  ifs.read((char *)&numberOfNodes, sizeof(uint32_t));

  weights.resize(numberOfNodes);
  ifs.read((char *)&weights[0], sizeof(float)*numberOfNodes);

  // load image data
  uint32_t imageCount;
  ifs.read((char *)&imageCount, sizeof(uint32_t));
  for (uint32_t i = 0; i < imageCount; i++) {
    uint64_t imageId;
    std::vector<float> vec(numberOfNodes);
    ifs.read((char *)&imageId, sizeof(uint64_t));
    ifs.read((char *)&vec[0], sizeof(float)*numberOfNodes);
    databaseVectors[imageId] = vec;
  }

  // load inveted files
  uint32_t invertedFileCount;
  ifs.read((char *)&invertedFileCount, sizeof(uint32_t));
  invertedFiles.resize(invertedFileCount);

  for (uint32_t i = 0; i < invertedFileCount; i++) {
    uint32_t size;
    ifs.read((char *)&size, sizeof(uint32_t));
    for (uint32_t j = 0; j < size; j++) {
      uint64_t imageId;
      uint32_t imageCount;
      ifs.read((char *)&imageId, sizeof(uint64_t));
      ifs.read((char *)&imageCount, sizeof(uint32_t));
      invertedFiles[i][imageId] = imageCount;
    }
  }

  // read in tree
  tree.resize(numberOfNodes);
  for (uint32_t i = 0; i < numberOfNodes; i++) {
    TreeNode t = tree[i];
    ifs.read((char *)&t.firstChildIndex, sizeof(uint32_t));
    ifs.read((char *)&t.index, sizeof(uint32_t));
    ifs.read((char *)&t.invertedFileLength, sizeof(uint32_t));
    ifs.read((char *)&t.level, sizeof(uint32_t));
    ifs.read((char *)&t.levelIndex, sizeof(uint32_t));

    // read cv::mat, copied from filesystem.cxx
    cvmat_header h;
    ifs.read((char *)&h, sizeof(cvmat_header));
    t.mean.create(h.rows, h.cols, h.elem_type);
    ifs.read((char *)t.mean.ptr(), h.rows * h.cols * h.elem_size);
  }

	std::cout << "Done reading vocab tree." << std::endl;
	
  return (ifs.rdstate() & std::ifstream::failbit) == 0;
}

bool VocabTree::save (const std::string &file_path) const {
	std::cout << "Writing vocab tree to " << file_path << "..." << std::endl;

  std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);

  //uint32_t num_clusters = inverted_index.size();
  ofs.write((const char *)&split, sizeof(uint32_t));
  ofs.write((const char *)&maxLevel, sizeof(uint32_t));
  ofs.write((const char *)&numberOfNodes, sizeof(uint32_t));
  ofs.write((const char *)&weights[0], sizeof(float)*numberOfNodes); // weights

  // write out databaseVectors
  uint32_t imageCount = databaseVectors.size();
  ofs.write((const char *)&imageCount, sizeof(uint32_t));
  for (auto& pair : databaseVectors) {
    ofs.write((const char *)&pair.first, sizeof(uint64_t));
    ofs.write((const char *)&(pair.second)[0], sizeof(float)*numberOfNodes); 
  }

  // write out inverted files
  uint32_t numInvertedFiles = invertedFiles.size();
  ofs.write((const char *)&numInvertedFiles, sizeof(uint32_t));
  for (std::unordered_map<uint64_t, uint32_t> invFile : invertedFiles) {
    uint32_t size = invFile.size();
    ofs.write((const char *)&size, sizeof(uint32_t));
    for (std::pair<uint64_t, uint32_t> pair : invFile) {
      ofs.write((const char *)&pair.first, sizeof(uint64_t));
      ofs.write((const char *)&pair.second, sizeof(uint32_t));
    }
  }

  // write out tree
  for (uint32_t i = 0; i < numberOfNodes; i++) {
    TreeNode t = tree[i];
    ofs.write((const char *)&t.firstChildIndex, sizeof(uint32_t));
    ofs.write((const char *)&t.index, sizeof(uint32_t));
    ofs.write((const char *)&t.invertedFileLength, sizeof(uint32_t));
    ofs.write((const char *)&t.level, sizeof(uint32_t));
    ofs.write((const char *)&t.levelIndex, sizeof(uint32_t));

    // write cv::mat, copied from filesystem.cxx
    cvmat_header h;
    h.elem_size = t.mean.elemSize();
    h.elem_type = t.mean.type();
    h.rows = t.mean.rows;
    h.cols = t.mean.cols;
    ofs.write((char *)&h, sizeof(cvmat_header));
    ofs.write((char *)t.mean.ptr(), h.rows * h.cols * h.elem_size);
  }

	std::cout << "Done writing vocab tree." << std::endl;

  return (ofs.rdstate() & std::ofstream::failbit) == 0;;
}

bool VocabTree::train(Dataset &dataset, const std::shared_ptr<const TrainParamsBase> &params, 
  const std::vector< std::shared_ptr<const Image > > &examples) {

	const std::shared_ptr<const TrainParams> &vt_params = std::static_pointer_cast<const TrainParams>(params);
	split = vt_params->split;
	uint32_t depth = vt_params->depth;
  numberOfNodes = (uint32_t)pow(split, maxLevel) / (split - 1);
  weights.resize(numberOfNodes);
  tree.resize(numberOfNodes);
  invertedFiles.resize((uint32_t)pow(split, maxLevel-1));

  // took the following from bag_of_words
  std::vector<uint64_t> all_ids(examples.size());
  for (uint32_t i = 0; i < examples.size(); i++) {
    all_ids[i] = examples[i]->id;
  }
  std::random_shuffle(all_ids.begin(), all_ids.end());

  std::vector<cv::Mat> all_descriptors;
  uint64_t num_features = 0;
  for (size_t i = 0; i < all_ids.size(); i++) {
    std::shared_ptr<Image> image = std::static_pointer_cast<Image>(dataset.image(all_ids[i]));
    if (image == nullptr) continue;

    const std::string &descriptors_location = dataset.location(image->feature_path("descriptors"));
    if (!filesystem::file_exists(descriptors_location)) continue;

    cv::Mat descriptors;
    if (filesystem::load_cvmat(descriptors_location, descriptors)) {
      num_features += descriptors.rows;

      all_descriptors.push_back(descriptors);
    }
  }

  const cv::Mat merged_descriptor = vision::merge_descriptors(all_descriptors, true);
  cv::Mat labels;
  uint32_t attempts = 1;
  cv::TermCriteria tc(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 16, 0.0001);
  // end of stuff from bag of words

  tree[0].levelIndex = 0;
  tree[0].index = 0;
  buildTreeRecursive(0, merged_descriptor, tc, attempts, cv::KMEANS_PP_CENTERS, 0);

  databaseVectors.reserve(all_ids.size());

  // now generate data on the reference images - descriptors go down tree, add images to inverted lists at leaves, 
  //   and generate di vector for image
  // Also stores counts for how many images pass through each node to calculate weights
  std::vector<uint32_t> counts(numberOfNodes);
  for (size_t i = 0; i < numberOfNodes; i++)
    counts[i] = 0;

  for (size_t i = 0; i < all_ids.size(); i++) {
    std::shared_ptr<Image> image = std::static_pointer_cast<Image>(dataset.image(all_ids[i]));
    if (image == nullptr) continue;

    const std::string &descriptors_location = dataset.location(image->feature_path("descriptors"));
    if (!filesystem::file_exists(descriptors_location)) continue;

    cv::Mat descriptors;
    if (filesystem::load_cvmat(descriptors_location, descriptors)) {
      std::vector<float> result = generateVector(descriptors, false, all_ids[i]);
      // accumulate counts
      for (size_t j = 0; j < numberOfNodes; j++)
      if (result[j] > 0)
        counts[j]++;

      //databaseVectors.insert(std::make_pair<uint64_t, std::vector<float>>(all_ids[i], result));
      databaseVectors[all_ids[i]] = result;
    }
  }
  for (size_t i = 0; i < numberOfNodes; i++)
    weights[i] = log(((float)counts[i]) / ((float)all_ids.size()));

  // now that we have the weights we iterate over all images and adjust the vector by weights, 
  //  then normalizes the vector
  typedef std::unordered_map<uint64_t, std::vector<float>>::iterator it_type;
  for (it_type iterator = databaseVectors.begin(); iterator != databaseVectors.end(); iterator++) {
    float length = 0; // hopefully shouldn't overflow
    for (size_t i = 0; i < numberOfNodes; i++) {
      (iterator->second)[i] *= weights[i];
      length += (float)pow((iterator->second)[i], 2.0);
    }
    length = sqrt(length);
    // normalizing
    for (size_t i = 0; i < numberOfNodes; i++) 
      (iterator->second)[i] /= length;
  }

	return true;
}

void VocabTree::buildTreeRecursive(uint32_t t, cv::Mat descriptors, cv::TermCriteria tc, 
  int attempts, int flags, int currLevel) {

  tree[t].level = currLevel;

  // handles the leaves
  if (currLevel == maxLevel - 1) {
    tree[t].firstChildIndex = -1;
    return;
  }

  cv::Mat labels;
  cv::Mat centers;

  cv::kmeans(descriptors, split, labels, tc, attempts, flags, centers);

  std::vector<cv::Mat> groups(split);
  for (uint32_t i = 0; i < split; i++)
    groups[i] = cv::Mat();

  for (int i = 0; i < labels.rows; i++) {
    int index = labels.at<int>(i);
    groups[index].push_back(descriptors.row(i));
  }

  for (uint32_t i = 0; i < split; i++) {
    uint32_t childLevelIndex = tree[t].levelIndex*split + i;
    uint32_t childIndex = (uint32_t)(pow(split, tree[t].level) / (split - 1)) + childLevelIndex;
    tree[childIndex].mean = centers.row(i);
    tree[childIndex].levelIndex = childLevelIndex;
    tree[childIndex].index = childIndex;
    if (i == 0)
      tree[t].firstChildIndex = childIndex;

    buildTreeRecursive(childIndex, groups[i], tc, attempts, flags, currLevel + 1);
  }
}

std::vector<float> VocabTree::generateVector(cv::Mat descriptors, bool shouldWeight, uint64_t id) {
  std::unordered_set<uint64_t> dummy;
  return generateVector(descriptors, shouldWeight, dummy, id);
}

std::vector<float> VocabTree::generateVector(cv::Mat descriptors, bool shouldWeight, 
  std::unordered_set<uint64_t> & possibleMatches,  uint64_t id) {

  std::vector<float> vec(numberOfNodes);
  for (uint32_t i = 0; i < numberOfNodes; i++)
    vec[i] = 0;

  for (int r = 0; r < descriptors.rows; r++) {
    generateVectorHelper(0, descriptors.row(r), vec, possibleMatches, id);
  }

  // if shouldWeight is true then weight all values in the vector and normalize
  if (shouldWeight) {
    float length = 0; // for normalizing
    for (uint32_t i = 0; i < numberOfNodes; i++) {
      vec[i] *= weights[i];
      length += vec[i] * vec[i];
    }
    length = sqrt(length);
    for (uint32_t i = 0; i < numberOfNodes; i++)
      vec[i] /= length;
  }

  return vec;
}

void VocabTree::generateVectorHelper(uint32_t nodeIndex, cv::Mat descriptor, std::vector<float> & counts,
  std::unordered_set<uint64_t> & possibleMatches, uint64_t id) {

  counts[nodeIndex]++;
  // if leaf
  if (tree[nodeIndex].firstChildIndex < 0) {
    std::unordered_map<uint64_t, uint32_t> invFile = invertedFiles[tree[nodeIndex].levelIndex];
    // inserting image id into the inverted file
    if (id >= 0) {
      if (invFile.find(id) == invFile.end())
        invFile[id] = 1;
      else
        invFile[id]++;
    }
    // accumulating image id's into possibleMatches
    else {
      // i don't like doing this serial, should find a better method
      typedef std::unordered_map<uint64_t, uint32_t>::iterator it_type;
      for (it_type iterator = invFile.begin(); iterator != invFile.end(); iterator++)
        possibleMatches.insert(iterator->first);
    }
  }
  // if inner node
  else {
    uint32_t maxChild = tree[nodeIndex].firstChildIndex;
    double max = descriptor.dot(tree[maxChild].mean);
    
    for (uint32_t i = 1; i < split; i++) {
      uint32_t childIndex = tree[nodeIndex].firstChildIndex + i;
      double dot = descriptor.dot(tree[childIndex].mean);
      if (dot>max) {
        max = dot;
        maxChild = childIndex;
      }
    }
    generateVectorHelper(maxChild, descriptor, counts, possibleMatches, id);
  }
}


std::shared_ptr<MatchResultsBase> VocabTree::search(Dataset &dataset, const std::shared_ptr<const SearchParamsBase> &params, 
  const std::shared_ptr<const Image > &example) {

	std::cout << "Searching for matching images..." << std::endl;
	const std::shared_ptr<const SearchParams> &ii_params = std::static_pointer_cast<const SearchParams>(params);
	
	std::shared_ptr<MatchResults> match_result = std::make_shared<MatchResults>();

  // get descriptors for example
  if (example == nullptr) return nullptr;
  const std::string &descriptors_location = dataset.location(example->feature_path("descriptors"));
  if (!filesystem::file_exists(descriptors_location)) return nullptr;

  cv::Mat descriptors;
  if (!filesystem::load_cvmat(descriptors_location, descriptors)) return nullptr;

  std::unordered_set<uint64_t> possibleMatches;

  std::vector<float> vec = generateVector(descriptors, true, possibleMatches);

  typedef std::pair<uint64_t, float> matchPair;
  std::vector<matchPair> values(possibleMatches.size());
  //for (int i = 0; i < vec.size(); i++) {
  for (uint64_t elem : possibleMatches) {
    // compute L1 norm (based on paper eq 5)
    float l1norm = 0;
    for (uint32_t i = 0; i < numberOfNodes; i++)
      l1norm += abs(vec[i] * (databaseVectors[elem])[i]);
    values.push_back(std::make_pair(elem, l1norm));
  }

  struct myComparer {
    bool operator() (matchPair a, matchPair b) { return a.second < b.second; };
  } comparer;

  std::sort(values.begin(), values.end(), comparer);

	// add in matches, right now just return the top 10%
  for (int i = 0; i < possibleMatches.size() / 10.0; i++)
    match_result->matches.push_back(values[i].first);
	//match_result->matches.push_back(0);

	return (std::shared_ptr<MatchResultsBase>)match_result;
}