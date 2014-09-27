// Copyright 2013-2014 [Author: Po-Wei Chou]
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <string>
#include <dnn.h>
#include <dnn-utility.h>
#include <cmdparser.h>
#include <rbm.h>
#include <batch.h>
using namespace std;

size_t dnn_predict(const DNN& dnn, DataSet& data, ERROR_MEASURE errorMeasure);
void dnn_train(DNN& dnn, DataSet& train, DataSet& valid, size_t batchSize, ERROR_MEASURE errorMeasure);
bool isEoutStopDecrease(const std::vector<size_t> Eout, size_t epoch, size_t nNonIncEpoch);

int main (int argc, char* argv[]) {

  CmdParser cmd(argc, argv);

  cmd.add("training_set_file")
     .add("model_in")
     .add("model_out", false);

  cmd.addGroup("Feature options:")
     .add("--input-dim", "specify the input dimension (dimension of feature).\n"
	 "0 for auto detection.")
     .add("--normalize", "Feature normalization: \n"
	"0 -- Do not normalize.\n"
	"1 -- Rescale each dimension to [0, 1] respectively.\n"
	"2 -- Normalize to standard score. z = (x-u)/sigma .", "0")
     .add("--nf", "Load pre-computed statistics from file", "")
     .add("--base", "Label id starts from 0 or 1 ?", "0");

  cmd.addGroup("Training options: ")
     .add("-v", "ratio of training set to validation set (split automatically)", "5")
     .add("--max-epoch", "number of maximum epochs", "100000")
     .add("--min-acc", "Specify the minimum cross-validation accuracy", "0.5")
     .add("--learning-rate", "learning rate in back-propagation", "0.1")
     .add("--variance", "the variance of normal distribution when initializing the weights", "0.01")
     .add("--batch-size", "number of data per mini-batch", "32")
     .add("--type", "choose one of the following:\n"
	"0 -- classfication\n"
	"1 -- regression", "0");

  cmd.addGroup("Hardward options:")
     .add("--cache", "specify cache size (in MB) in GPU used by cuda matrix.", "16");

  cmd.addGroup("Example usage: dnn-train data/train3.dat --nodes=16-8");

  if (!cmd.isOptionLegal())
    cmd.showUsageAndExit();

  string train_fn     = cmd[1];
  string model_in     = cmd[2];
  string model_out    = cmd[3];

  size_t input_dim    = cmd["--input-dim"];
  NormType n_type     = (NormType) (int) cmd["--normalize"];
  string n_filename   = cmd["--nf"];
  int base	      = cmd["--base"];

  int ratio	      = cmd["-v"];
  size_t batchSize    = cmd["--batch-size"];
  float learningRate  = cmd["--learning-rate"];
  float variance      = cmd["--variance"];
  float minValidAcc   = cmd["--min-acc"];
  size_t maxEpoch     = cmd["--max-epoch"];

  size_t cache_size   = cmd["--cache"];
  CudaMemManager<float>::setCacheSize(cache_size);

  // Set configurations
  Config config;
  config.variance = variance;
  config.learningRate = learningRate;
  config.minValidAccuracy = minValidAcc;
  config.maxEpoch = maxEpoch;

  // Load model
  DNN dnn(model_in);
  dnn.setConfig(config);

  // Load data
  DataSet data(train_fn, input_dim, base);
  // data.loadPrecomputedStatistics(n_filename);
  data.setNormType(n_type);
  data.showSummary();

  DataSet train, valid;
  DataSet::split(data, train, valid, ratio);
  config.print();

  // Start Training
  ERROR_MEASURE err = CROSS_ENTROPY;
  dnn_train(dnn, train, valid, batchSize, err);

  // Save the model
  if (model_out.empty())
    model_out = train_fn.substr(train_fn.find_last_of('/') + 1) + ".model";

  dnn.save(model_out);

  return 0;
}

void dnn_train(DNN& dnn, DataSet& train, DataSet& valid, size_t batchSize, ERROR_MEASURE errorMeasure) {

  printf("Training...\n");
  perf::Timer timer;
  timer.start();

  vector<mat> O(dnn.getNLayer());

  size_t Ein = 1;
  size_t MAX_EPOCH = dnn.getConfig().maxEpoch, epoch;
  std::vector<size_t> Eout;

  float lr = dnn.getConfig().learningRate / batchSize;

  size_t nTrain = train.size(),
	 nValid = valid.size();

  mat fout;

  printf("._______._________________________._________________________.\n"
         "|       |                         |                         |\n"
         "|       |        In-Sample        |      Out-of-Sample      |\n"
         "| Epoch |__________.______________|__________.______________|\n"
         "|       |          |              |          |              |\n"
         "|       | Accuracy | # of correct | Accuracy | # of correct |\n"
         "|_______|__________|______________|__________|______________|\n");

  for (epoch=0; epoch<MAX_EPOCH; ++epoch) {

    Batches batches(batchSize, nTrain);
    for (Batches::iterator itr = batches.begin(); itr != batches.end(); ++itr) {

      // Copy a batch of data from host to device
      auto data = train[itr];

      dnn.feedForward(fout, data.x);

      mat error = getError( data.y, fout, errorMeasure);

      dnn.backPropagate(error, data.x, fout, lr);
    }

    Ein = dnn_predict(dnn, train, errorMeasure);
    Eout.push_back(dnn_predict(dnn, valid, errorMeasure));

    float trainAcc = 1.0f - (float) Ein / nTrain;

    if (trainAcc < 0) {
      cout << "."; cout.flush();
      continue;
    }

    float validAcc = 1.0f - (float) Eout[epoch] / nValid;

    printf("|%4lu   |  %.2f %% |  %7lu     |  %.2f %% |  %7lu     |\n",
      epoch, trainAcc * 100, nTrain - Ein, validAcc * 100, nValid - Eout[epoch]);

    if (validAcc > dnn.getConfig().minValidAccuracy && isEoutStopDecrease(Eout, epoch, dnn.getConfig().nNonIncEpoch))
      break;

    dnn.adjustLearningRate(trainAcc);
  }

  // Show Summary
  printf("\n%ld epochs in total\n", epoch);
  timer.elapsed();

  printf("[   In-Sample   ] ");
  showAccuracy(Ein, train.size());
  printf("[ Out-of-Sample ] ");
  showAccuracy(Eout.back(), valid.size());
}

size_t dnn_predict(const DNN& dnn, DataSet& data, ERROR_MEASURE errorMeasure) {
  size_t nError = 0;

  Batches batches(2048, data.size());
  for (Batches::iterator itr = batches.begin(); itr != batches.end(); ++itr) {
    auto d = data[itr];
    mat prob = dnn.feedForward(d.x);
    nError += zeroOneError(prob, d.y, errorMeasure);
  }

  return nError;
}

bool isEoutStopDecrease(const std::vector<size_t> Eout, size_t epoch, size_t nNonIncEpoch) {

  for (size_t i=0; i<nNonIncEpoch; ++i) {
    if (epoch - i > 0 && Eout[epoch] > Eout[epoch - i])
      return false;
  }

  return true;
}
