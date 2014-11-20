#include "config.hpp"
#include <dbarts/bartFit.hpp>

#include <cmath>     // sqrt
#include <cstring>   // memcpy
#include <cstddef>   // size_t, offsetof

#if !defined(HAVE_SYS_TIME_H) && defined(HAVE_GETTIMEOFDAY)
#undef HAVE_GETTIMEOFDAY
#endif
#ifdef HAVE_SYS_TIME_H
#  include <sys/time.h> // gettimeofday
#else
#  include <time.h>
#endif


#include <set>       // used to sort and find 
#include <vector>    //   split points
#include <algorithm> // integer min

#include <external/alloca.h>
#include <external/io.h>
#include <external/linearAlgebra.h>
#include <external/random.h>
#include <external/stats.h>
#include <external/thread.h>

#include <dbarts/responseModel.hpp>
#include <dbarts/results.hpp>
#include "tree.hpp"

using std::size_t;
using std::uint32_t;

namespace {
  using namespace dbarts;

  void allocateMemory(BARTFit& fit);
  void setPrior(BARTFit& fit);
  void setInitialCutPoints(BARTFit& fit);
  void setCutPoints(BARTFit& fit, const size_t* columns, size_t numColumns);
  void setCutPointsFromQuantiles(BARTFit& fit, const double* x, uint32_t maxNumCuts,
                                 uint32_t& numCutsPerVariable, double*& cutPoints,
                                 std::set<double>& uniqueElements, std::vector<double>& sortedElements);
  void setCutPointsUniformly(BARTFit& fit, const double* x, uint32_t maxNumCuts,
                             uint32_t& numCutsPerVariable, double*& cutPoints);
  void setInitialFit(BARTFit& fit);
  
  void printInitialSummary(const BARTFit& fit);
  void printTerminalSummary(const BARTFit& fit);
  
  void initializeLatents(BARTFit& fit);
  void rescaleResponse(BARTFit& fit);
  
  void calculateResidualsFromOtherTreeFits(const double* y, const double* totalFits, const double* oldTreeFits, size_t numObservations, double* r);
  void updateTotalFits(const double* oldTreeFits, const double* currTreeFits, size_t numObservations, double* totalFits);
  
  void sampleProbitLatentVariables(BARTFit& fit, const double* fits, double* yRescaled);
  void storeSamples(const BARTFit& fit, Results& results, const double* trainingSample, const double* testSample,
                    double sigma, const uint32_t* variableCounts, size_t simNum);
  void countVariableUses(const BARTFit& fit, uint32_t* variableCounts);
  
  void updateTrainingFits(const BARTFit& fit, double* currTreeFits, double* totalFits);
  void updateTestFits(const BARTFit& fit, double* totalTestFits);
  
#ifdef HAVE_SYS_TIME_H
  double subtractTimes(struct timeval end, struct timeval start);
#else
  double subtractTimes(time_t end, time_t start);
#endif
}

#define CHEAT_SIGMA(_M_) static_cast<Response::NormalChiSquaredModel*>(_M_)->sigma

namespace dbarts {
  
  void BARTFit::setResponse(const double* newY) {
    if (!control.responseIsBinary) {
      double sigmaUnscaled = CHEAT_SIGMA(model.responseModel) * scratch.dataScale.range;
      double priorUnscaled;
      if (model.responseModel->info & Response::HAS_SCALE_PARAMETER)
        priorUnscaled = model.responseModel->getScale(*this) * scratch.dataScale.range * scratch.dataScale.range;
      
      data.y = newY;
      
      rescaleResponse(*this);
      
      CHEAT_SIGMA(model.responseModel) = sigmaUnscaled / scratch.dataScale.range;
      if (model.responseModel->info & Response::HAS_SCALE_PARAMETER)
        model.responseModel->setScale(*this, priorUnscaled / (scratch.dataScale.range * scratch.dataScale.range));
    } else {
      data.y = newY;
      
      sampleProbitLatentVariables(*this, const_cast<const double*>(state.totalFits), const_cast<double*>(scratch.yRescaled));
    }
  }
  
  void BARTFit::setOffset(const double* newOffset) {
    if (!control.responseIsBinary) {
      double sigmaUnscaled = CHEAT_SIGMA(model.responseModel) * scratch.dataScale.range;
      double priorUnscaled;
      if (model.responseModel->info & Response::HAS_SCALE_PARAMETER)
        priorUnscaled = model.responseModel->getScale(*this) * scratch.dataScale.range * scratch.dataScale.range;
      
      data.offset = newOffset;
      
      rescaleResponse(*this);
      
      CHEAT_SIGMA(model.responseModel) = sigmaUnscaled / scratch.dataScale.range;
      if (model.responseModel->info & Response::HAS_SCALE_PARAMETER)
        model.responseModel->setScale(*this, priorUnscaled / (scratch.dataScale.range * scratch.dataScale.range));
    } else {
      data.offset = newOffset;
      
      sampleProbitLatentVariables(*this, const_cast<const double*>(state.totalFits), const_cast<double*>(scratch.yRescaled));
    }
  }
  
  bool BARTFit::setPredictor(const double* newPredictor)
  {
    size_t* columns = ext_stackAllocate(data.numPredictors, size_t);
    for (size_t i = 0; i < data.numPredictors; ++i) columns[i] = i;
    
    setCutPoints(*this, columns, data.numPredictors);
    
    ext_stackFree(columns);
    
    data.X = newPredictor;
    
    double* Xt = const_cast<double*>(scratch.Xt);
    for (size_t col = 0; col < data.numPredictors; ++col) {
      for (size_t row = 0; row < data.numObservations; ++row) {
        Xt[row * data.numPredictors + col] = data.X[row + col * data.numObservations];
      }
    }
    
    double sigma_sq = CHEAT_SIGMA(model.responseModel) * CHEAT_SIGMA(model.responseModel);
    // double** nodePosteriorPredictions = new double*[control.numTrees];
    // for (size_t i = 0; i < control.numTrees; ++i) nodePosteriorPredictions[i] = NULL;
    
    bool allTreesAreValid = true;
    size_t treeNum = 0;
    for ( ; treeNum < control.numTrees && allTreesAreValid == true; ++treeNum) {
      Tree& tree_i(*TREE_AT(state.trees, treeNum, scratch.nodeSize));
      // const double* treeFits = state.treeFits + treeNum * data.numObservations;
      
      // next allocates memory
      // nodePosteriorPredictions[treeNum] = tree_i.recoverAveragesFromFits(*this, treeFits);
      
      tree_i.updateWithNewCovariates(*this, sigma_sq);
      // tree_i.updateState(*this, NULL, BART_NODE_UPDATE_COVARIATES_CHANGED);
      
      allTreesAreValid &= tree_i.isValid();
    }
    
    
    if (allTreesAreValid) updateTrainingFits(*this, state.treeFits, state.totalFits);
    
    
    // for (size_t i = control.numTrees; i > 0; --i) delete [] nodePosteriorPredictions[i - 1];
    // delete [] nodePosteriorPredictions;
    
    return allTreesAreValid;
  }
  
  bool BARTFit::updatePredictor(const double* newPredictor, size_t column)
  {
    return updatePredictors(newPredictor, &column, 1);
  }
  
  bool BARTFit::updatePredictors(const double* newPredictor, const size_t* columns, size_t numColumns)
  {
    // store current
    double* oldPredictor = new double[data.numObservations * numColumns];
    double** oldCutPoints = new double*[numColumns];
    
    for (size_t i = 0; i < numColumns; ++i) {
      std::memcpy(oldPredictor + i * data.numObservations, data.X + columns[i] * data.numObservations, data.numObservations * sizeof(double));
      oldCutPoints[i] = new double[scratch.numCutsPerVariable[columns[i]]];
      std::memcpy(oldCutPoints[i], scratch.cutPoints[columns[i]], scratch.numCutsPerVariable[columns[i]] * sizeof(double));
    }
    
    
    // install new
    setCutPoints(*this, columns, numColumns);
    
    double* X  = const_cast<double*>(data.X);
    double* Xt = const_cast<double*>(scratch.Xt);
    for (size_t i = 0; i < numColumns; ++i) {
      std::memcpy(X + columns[i] * data.numObservations, newPredictor + i * data.numObservations, data.numObservations * sizeof(double));
      for (size_t row = 0; row < data.numObservations; ++row) {
        Xt[row * data.numPredictors + columns[i]] = newPredictor[row + i * data.numObservations];
      }
    }
    
    
    // check validity of new columns and recover node posterior samples
    bool allTreesAreValid = true;
    
    // double** nodePosteriorPredictions = new double*[control.numTrees];
    // for (size_t i = 0; i < control.numTrees; ++i) nodePosteriorPredictions[i] = NULL;
    double sigma_sq = CHEAT_SIGMA(model.responseModel) * CHEAT_SIGMA(model.responseModel);
    
    size_t treeNum;
    for (treeNum = 0; treeNum < control.numTrees && allTreesAreValid == true; ++treeNum) {
      Tree& tree_i(*TREE_AT(state.trees, treeNum, scratch.nodeSize));
      // const double* treeFits = state.treeFits + treeNum * data.numObservations;
      
      // nodePosteriorPredictions[treeNum] = tree_i.recoverAveragesFromFits(*this, treeFits);
      
      tree_i.updateWithNewCovariates(*this, sigma_sq);
      // tree_i.updateState(*this, NULL, BART_NODE_UPDATE_COVARIATES_CHANGED);
      
      allTreesAreValid &= tree_i.isValid();
    }
    
    
    if (!allTreesAreValid) {
      for (size_t i = 0; i < numColumns; ++i) {
        std::memcpy(X + columns[i] * data.numObservations, oldPredictor + i * data.numObservations, data.numObservations * sizeof(double));
        
        std::memcpy(const_cast<double**>(scratch.cutPoints)[columns[i]], oldCutPoints[i], scratch.numCutsPerVariable[columns[i]] * sizeof(double));
        
        for (size_t row = 0; row < data.numObservations; ++row) {
          Xt[row * data.numPredictors + columns[i]] = oldPredictor[row + i * data.numObservations];
        }
      }
      
      // for (size_t i = 0; i < treeNum; ++i) TREE_AT(state.trees, i, scratch.nodeSize)->updateMemberships(*this);
      for (size_t i = 0; i < treeNum; ++i) TREE_AT(state.trees, i, scratch.nodeSize)->updateWithNewCovariates(*this, sigma_sq);
    } else {
      updateTrainingFits(*this, state.treeFits, state.totalFits);
    }
    
    // for (size_t i = control.numTrees; i > 0; --i) delete [] nodePosteriorPredictions[i - 1];
    // delete [] nodePosteriorPredictions;
    
    for (size_t i = 0; i < numColumns; ++i) delete [] oldCutPoints[i];
    delete [] oldCutPoints;
    delete [] oldPredictor;
    
    return allTreesAreValid;
  }
  
#define INVALID_ADDRESS reinterpret_cast<const double*>(this)
  void BARTFit::setTestPredictor(const double* newTestPredictor, size_t numTestObservations) {
    setTestPredictorAndOffset(newTestPredictor, INVALID_ADDRESS, numTestObservations);
  }
  
  void BARTFit::setTestOffset(const double* newTestOffset) {
     data.testOffset = newTestOffset;
  }
  // setting testOffset to NULL is valid
  // an invalid pointer address for testOffset is the object itself; when invalid, it is not updated
  void BARTFit::setTestPredictorAndOffset(const double* X_test, const double* testOffset, size_t numTestObservations) {
    if (numTestObservations == 0 || X_test == NULL) {
      if (scratch.Xt_test != NULL) { delete [] scratch.Xt_test; scratch.Xt_test = NULL; }
      if (state.totalTestFits != NULL) { delete [] state.totalTestFits; state.totalTestFits = NULL; }
      
      data.X_test = NULL;
      data.numTestObservations = 0;
      data.testOffset = NULL;
    } else {
      data.X_test = X_test;
      
      if (numTestObservations != data.numTestObservations) {
        if (scratch.Xt_test != NULL) { delete [] scratch.Xt_test; scratch.Xt_test = NULL; }
        if (state.totalTestFits != NULL) { delete [] state.totalTestFits; state.totalTestFits = NULL; }
        data.numTestObservations = numTestObservations;
        
        scratch.Xt_test = new double[data.numTestObservations * data.numPredictors];
        state.totalTestFits = new double[data.numTestObservations];
      }
      
      double* Xt_test = const_cast<double*>(scratch.Xt_test);
      for (size_t col = 0; col < data.numPredictors; ++col) {
        for (size_t row = 0; row < data.numTestObservations; ++row) {
          Xt_test[row * data.numPredictors + col] = data.X_test[col * data.numTestObservations + row];
        }
      }
      
      if (testOffset != INVALID_ADDRESS) data.testOffset = testOffset;
      
      updateTestFits(*this, state.totalTestFits);
    }
  }
#undef INVALID_ADDRESS
  
  void BARTFit::updateTestPredictor(const double* newTestPredictor, size_t column) {
    updateTestPredictors(newTestPredictor, &column, 1);
  }
  
  void BARTFit::updateTestPredictors(const double* newTestPredictor, const size_t* columns, size_t numColumns) {
    double* X_test = const_cast<double*>(data.X_test);
    double* Xt_test = const_cast<double*>(scratch.Xt_test);
    
    for (size_t i = 0; i < numColumns; ++i) {
      size_t col = columns[i];
      std::memcpy(X_test + col * data.numTestObservations, newTestPredictor + i * data.numTestObservations, data.numTestObservations * sizeof(double));
      
      for (size_t row = 0; row < data.numTestObservations; ++row) {
        Xt_test[row * data.numPredictors + col] = newTestPredictor[row + i * data.numTestObservations];
      }
    }
    
    updateTestFits(*this, state.totalTestFits);
  }
  
  BARTFit::BARTFit(Control control, Model model, Data data) :
    control(control), model(model), data(data), scratch(), state(), threadManager(NULL)
  {
    // Node contains union(struct { Node*, Rule }, struct { size_t, void* })
    // instead of void*, we stick the scratch there (is basically a "void")
    // however, if size_t + nodeScratch <= size of Node* + Rule, we would end
    // up allocating too little space
    if (offsetof(NodeMembers::EndNode, scratch) + model.endNodeModel->perNodeScratchSize <= sizeof(NodeMembers::Parent)) {
      scratch.nodeSize = sizeof(Node);
    } else {
      scratch.nodeSize = offsetof(Node, e) + offsetof(NodeMembers::EndNode, scratch) + model.endNodeModel->perNodeScratchSize;;
    }
    
    allocateMemory(*this);

    setPrior(*this);
    setInitialCutPoints(*this);
    setInitialFit(*this);

    state.runningTime = 0.0;

    if (this->control.verbose) printInitialSummary(*this);
  }
  
  BARTFit::~BARTFit()
  {
    delete [] scratch.yRescaled; scratch.yRescaled = NULL;
    delete [] scratch.Xt; scratch.Xt = NULL;
    delete [] scratch.Xt_test; scratch.Xt_test = NULL;
    delete [] scratch.treeY; scratch.treeY = NULL;
    
    delete [] scratch.numCutsPerVariable; scratch.numCutsPerVariable = NULL;
    if (scratch.cutPoints != NULL) {
      for (size_t i = 0; i < data.numPredictors; ++i) delete [] scratch.cutPoints[i];
    }
    delete [] scratch.cutPoints; scratch.cutPoints = NULL;
    
    if (state.trees != NULL) for (size_t i = control.numTrees; i > 0; --i) Node::invalidate(*this, *NODE_AT(state.trees, i - 1, scratch.nodeSize));
    ::operator delete (state.trees); state.trees = NULL;
    delete [] state.treeIndices; state.treeIndices = NULL;
    
    delete [] state.treeFits; state.treeFits = NULL;
    delete [] state.totalFits; state.totalFits = NULL;
    if (data.numTestObservations > 0) delete [] state.totalTestFits;
    state.totalTestFits = NULL;
    
    ext_mt_destroy(threadManager);
  }
  
  Results* BARTFit::runSampler()
  {
    return runSampler(control.numBurnIn, control.numSamples);
  }
  
  Results* BARTFit::runSampler(size_t numBurnIn, size_t numSamples)
  {
    bool /* stepTaken, */ isThinningIteration;
    // StepType ignored;
    
    Results* resultsPointer = new Results(data.numObservations, data.numPredictors,
                                          data.numTestObservations, numSamples == 0 ? 1 : numSamples); // ensure at least one sample for state's sake
    Results& results(*resultsPointer);
    
    double* currFits = new double[data.numObservations];
    double* currTestFits = NULL;
    if (data.numTestObservations > 0) currTestFits = new double[data.numTestObservations];
    
    uint32_t* variableCounts = ext_stackAllocate(data.numPredictors, uint32_t);
    
    
    size_t totalNumIterations = (numBurnIn + numSamples) * control.treeThinningRate;
    uint32_t majorIterationNum = 0;
    
    if (control.verbose) ext_printf("Running mcmc loop:\n");
    
#ifdef HAVE_SYS_TIME_H
    struct timeval startTime;
    struct timeval endTime;
    gettimeofday(&startTime, NULL);
#else
    time_t startTime;
    time_t endTime;
    startTime = time(NULL);
#endif

    for (uint32_t k = 0; k < totalNumIterations; ++k) {
      isThinningIteration = ((k + 1) % control.treeThinningRate != 0);
            
      majorIterationNum = k / control.treeThinningRate;
      
      if (control.verbose && !isThinningIteration && (majorIterationNum + 1) % control.printEvery == 0)
        ext_printf("iteration: %u (of %u)\n", majorIterationNum + 1, totalNumIterations / control.treeThinningRate);
      
      if (!isThinningIteration && data.numTestObservations > 0) ext_setVectorToConstant(state.totalTestFits, data.numTestObservations, 0.0);
      
      double sigma_sq = CHEAT_SIGMA(model.responseModel) * CHEAT_SIGMA(model.responseModel);

      for (size_t i = 0; i < control.numTrees; ++i) {
        Tree& tree_i(*TREE_AT(state.trees, i, scratch.nodeSize));
        double* oldTreeFits = state.treeFits + i * data.numObservations;
        
        // put into scratch.treeY what we should fit tree against
        calculateResidualsFromOtherTreeFits(scratch.yRescaled, state.totalFits, oldTreeFits, data.numObservations, scratch.treeY);
        
        // this should cache in the bottom nodes values necessary to a) calculate log likelihood/log integrated
        // likelihood and b) sample from the posterior of the parameters for the model
        // tree_i.updateBottomNodesWithValues(*this, scratch.treeY);
        tree_i.prepareForMetropolisStep(*this, scratch.treeY, sigma_sq);
        
        tree_i.drawFromTreeStructurePosterior(*this, scratch.treeY, sigma_sq);
        tree_i.drawFromEndNodePosteriors(*this, scratch.treeY, sigma_sq);
        tree_i.getFits(*this, currFits, isThinningIteration ? NULL : currTestFits);
        
        // remove old tree fits from total and add in current
        updateTotalFits(oldTreeFits, currFits, data.numObservations, state.totalFits);
        
        if (!isThinningIteration && data.numTestObservations > 0) {
          ext_addVectorsInPlace(const_cast<const double*>(currTestFits), data.numTestObservations, 1.0, state.totalTestFits);
        }
        
        std::memcpy(oldTreeFits, const_cast<const double*>(currFits), data.numObservations * sizeof(double));
      }
      
      if (control.responseIsBinary) {
        sampleProbitLatentVariables(*this, state.totalFits, const_cast<double*>(scratch.yRescaled));
      } else {
        model.responseModel->drawFromPosterior(*this, scratch.yRescaled, state.totalFits);
      }
      
      if (!isThinningIteration) {
        // if not out of burn-in, store result in first result; start
        // overwriting after that
        bool isBurningIn = majorIterationNum < numBurnIn;
        size_t simNum = (!isBurningIn ? majorIterationNum - numBurnIn : 0);
        
        countVariableUses(*this, variableCounts);
        
        storeSamples(*this, results, state.totalFits, state.totalTestFits, CHEAT_SIGMA(model.responseModel), variableCounts, simNum);
        
        if (control.callback != NULL) {
          control.callback(control.callbackData, *this, isBurningIn,
                           results.trainingSamples + simNum * data.numObservations,
                           results.testSamples + simNum * data.numTestObservations,
                           results.sigmaSamples[simNum]);
        }
      }
    }
    
#ifdef HAVE_SYS_TIME_H
    gettimeofday(&endTime, NULL);
#else
    endTime = time(NULL);
#endif
    
    state.runningTime += subtractTimes(endTime, startTime);
    
    if (control.verbose) printTerminalSummary(*this);
    
    delete [] currFits;
    if (data.numTestObservations > 0) delete [] currTestFits;
    ext_stackFree(variableCounts);
    
    if (numSamples == 0) {
      delete resultsPointer;
      return NULL;
    }
    
    return resultsPointer;
  }
} // namespace dbarts


namespace {
  using namespace dbarts;
  
  void printInitialSummary(const BARTFit& fit) {
    const Control& control(fit.control);
    const Data& data(fit.data);
    const Model& model(fit.model);
    const Scratch& scratch(fit.scratch);
    
    if (control.responseIsBinary)
      ext_printf("\nRunning BART with binary y\n\n");
    else
      ext_printf("\nRunning BART with numeric y\n\n");
    
    ext_printf("number of trees: %u\n", control.numTrees);
    
    ext_printf("Prior:\n");
    // dirty hack... should have priors print themselves
    model.endNodeModel->print(fit);
    if (!control.responseIsBinary) {
      model.responseModel->print(fit);
    }
    CGMPrior* treePrior = static_cast<CGMPrior*>(model.treePrior);
    ext_printf("\tpower and base for tree prior: %f %f\n", treePrior->power, treePrior->base);
    ext_printf("\tuse quantiles for rule cut points: %s\n", control.useQuantiles ? "true" : "false");
    ext_printf("data:\n");
    ext_printf("\tnumber of training observations: %u\n", data.numObservations);
    ext_printf("\tnumber of test observations: %u\n", data.numTestObservations);
    ext_printf("\tnumber of explanatory variables: %u\n\n", data.numPredictors);
    if (data.weights != NULL) ext_printf("\tusing observation weights\n");
    
    
    ext_printf("\nCutoff rules c in x<=c vs x>c\n");
    ext_printf("Number of cutoffs: (var: number of possible c):\n");
    for (size_t i = 0; i < data.numPredictors; ++i ) {
      ext_printf("(%u: %u) ", i + 1, scratch.numCutsPerVariable[i]);
      if ((i + 1) % 5 == 0) ext_printf("\n");
    }
    ext_printf("\n");
    if (control.printCutoffs > 0) {
      ext_printf("cutoffs:\n");
      for (size_t i = 0; i < data.numPredictors; ++i) {
        ext_printf("x(%u) cutoffs: ", i + 1);
        
        size_t j;
        for (j = 0; j < scratch.numCutsPerVariable[i] - 1 && j < control.printCutoffs - 1; ++j) {
          ext_printf("%f", scratch.cutPoints[i][j]);
          if ((j + 1) % 5 == 0) ext_printf("\n\t");
        }
        if (j > 2 && j == control.printCutoffs && j < scratch.numCutsPerVariable[i] - 1)
          ext_printf("...");
        
        ext_printf("%f", scratch.cutPoints[i][scratch.numCutsPerVariable[i] - 1]);
        ext_printf("\n");
      }
    }
    
    if (data.offset != NULL || (data.numTestObservations > 0 && data.testOffset != NULL)) {
      ext_printf("\noffsets:\n");
      
      if (data.offset != NULL) {
        ext_printf("\treg : %.2f", data.offset[0]);
        for (size_t i = 1; i < (5 < data.numObservations ? 5 : data.numObservations); ++i) ext_printf(" %.2f", data.offset[i]);
        ext_printf("\n");
      }
      if (data.numTestObservations > 0 && data.testOffset != NULL) {
        ext_printf("\ttest: %.2f", data.testOffset[0]);
        for (size_t i = 1; i < (5 < data.numTestObservations ? 5 : data.numTestObservations); ++i) ext_printf(" %.2f", data.testOffset[i]);
      }
    }
  }
  
  void printTerminalSummary(const BARTFit& fit) {
    ext_printf("total seconds in loop: %f\n", fit.state.runningTime);
    
    ext_printf("\nTree sizes, last iteration:\n");
    for (size_t i = 0; i < fit.control.numTrees; ++i) {
      ext_printf("%u ", TREE_AT(fit.state.trees, i, fit.scratch.nodeSize)->getNumBottomNodes());
      if ((i + 1) % 20 == 0) ext_printf("\n");
    }
    ext_printf("\n");
    
    uint32_t* variableCounts = ext_stackAllocate(fit.data.numPredictors, uint32_t);
    
    ext_printf("Variable Usage, last iteration (var:count):\n");
    countVariableUses(fit, variableCounts);
    for (size_t i = 0; i < fit.data.numPredictors; ++i) {
      ext_printf("(%lu: %u) ", static_cast<unsigned long int>(i + 1), variableCounts[i]);
      if ((i + 1) % 5 == 0) ext_printf("\n");
    }
    
    ext_stackFree(variableCounts);
    
    
    ext_printf("\nDONE BART\n\n");
  }
  
  void allocateMemory(BARTFit& fit) {
    Control& control(fit.control);
    Data& data(fit.data);
    Scratch& scratch(fit.scratch);
    State& state(fit.state);
        
    scratch.yRescaled = new double[data.numObservations];
    
    if (control.responseIsBinary) initializeLatents(fit);
    else rescaleResponse(fit);
    
    scratch.Xt = new double[data.numObservations * data.numPredictors];
    double* Xt = const_cast<double*>(scratch.Xt);
    for (size_t col = 0; col < data.numPredictors; ++col) {
      for (size_t row = 0; row < data.numObservations; ++row) {
        Xt[row * data.numPredictors + col] = data.X[col * data.numObservations + row];
      }
    }
    
    if (data.numTestObservations > 0) {
      scratch.Xt_test = new double[data.numTestObservations * data.numPredictors];
      double* Xt_test = const_cast<double*>(scratch.Xt_test);
      for (size_t col = 0; col < data.numPredictors; ++col) {
        for (size_t row = 0; row < data.numTestObservations; ++row) {
          Xt_test[row * data.numPredictors + col] = data.X_test[col * data.numTestObservations + row];
        }
      }
    }

    scratch.treeY = new double[data.numObservations];
    for (size_t i = 0; i < data.numObservations; ++i) scratch.treeY[i] = scratch.yRescaled[i];
    
    scratch.numCutsPerVariable = new uint32_t[data.numPredictors];

    scratch.cutPoints = new double*[data.numPredictors];
    const double** cutPoints = const_cast<const double**>(scratch.cutPoints);
    for (size_t i = 0; i < data.numPredictors; ++i) cutPoints[i] = NULL;
    
    state.trees = ::operator new (control.numTrees * scratch.nodeSize);
    state.treeIndices = new size_t[data.numObservations * control.numTrees];
    
    for (size_t i = 0; i < control.numTrees; ++i) {
      Node::initialize(fit, *NODE_AT(state.trees, i, fit.scratch.nodeSize), state.treeIndices + i * data.numObservations, data.numObservations);
    }
    
    if (control.numThreads > 1 && ext_mt_create(&fit.threadManager, control.numThreads) != 0) {
      ext_printMessage("Unable to multi-thread, defaulting to single.");
    }
  }
  
  void setPrior(BARTFit& fit) {
    Control& control(fit.control);
    Data& data(fit.data);
    Model& model(fit.model);
    Scratch& scratch(fit.scratch);
    // State& state(fit.state);
    
    CHEAT_SIGMA(fit.model.responseModel) = control.responseIsBinary ? 1.0 : (data.sigmaEstimate / scratch.dataScale.range);
    fit.model.responseModel->setScale(fit, CHEAT_SIGMA(fit.model.responseModel) * CHEAT_SIGMA(fit.model.responseModel) * model.responseModel->getScale(fit));
  }
  
  void setInitialCutPoints(BARTFit& fit) {
    Data& data(fit.data);
    Scratch& scratch(fit.scratch);
    
    uint32_t* numCutsPerVariable = const_cast<uint32_t*>(scratch.numCutsPerVariable);
    double** cutPoints = const_cast<double**>(scratch.cutPoints);
    for (size_t i = 0; i < data.numPredictors; ++i) {
      numCutsPerVariable[i] = static_cast<uint32_t>(-1);
      cutPoints[i] = NULL;
    }
    
    size_t* columns = ext_stackAllocate(data.numPredictors, size_t);
    for (size_t i = 0; i < data.numPredictors; ++i) columns[i] = i;
    
    setCutPoints(fit, columns, data.numPredictors);
    
    ext_stackFree(columns);
  }
  
  void setCutPoints(BARTFit& fit, const size_t* columns, size_t numColumns)
  {
    Control& control(fit.control);
    Data& data(fit.data);
    Scratch& scratch(fit.scratch);
    
    uint32_t* numCutsPerVariable = const_cast<uint32_t*>(scratch.numCutsPerVariable);
    double** cutPoints = const_cast<double**>(scratch.cutPoints);
        
    if (control.useQuantiles) {
      if (data.maxNumCuts == NULL) ext_throwError("Num cuts cannot be NULL if useQuantiles is true.");
      
       // sets are inherently sorted, should be a binary tree back there somewhere
      std::set<double> uniqueElements;
      std::vector<double> sortedElements(data.numObservations);
      
      for (size_t i = 0; i < numColumns; ++i) {
        size_t col = columns[i];
        
        setCutPointsFromQuantiles(fit, data.X + col * data.numObservations, data.maxNumCuts[col],
                                  numCutsPerVariable[col], cutPoints[col],
                                  uniqueElements, sortedElements);
      }
    } else {
      for (size_t i = 0; i < numColumns; ++i) {
        size_t col = columns[i];
        
        setCutPointsUniformly(fit, data.X + col * data.numObservations, data.maxNumCuts[col],
                              numCutsPerVariable[col], cutPoints[col]);
      }
    }
  }
  
  void setCutPointsFromQuantiles(BARTFit& fit, const double* x, uint32_t maxNumCuts,
                                 uint32_t& numCutsPerVariable, double*& cutPoints,
                                 std::set<double>& uniqueElements, std::vector<double>& sortedElements)
  {
    Data& data(fit.data);
    
    // sets are inherently sorted, should be a binary tree back there somewhere
    uniqueElements.clear();
    for (size_t i = 0; i < data.numObservations; ++i) uniqueElements.insert(x[i]);
    
    size_t numUniqueElements = uniqueElements.size();
      
    size_t step, numCuts, offset;
    if (numUniqueElements <= maxNumCuts + 1) {
      numCuts = numUniqueElements - 1;
      step = 1;
      offset = 0;
    } else {
      numCuts = maxNumCuts;
      step = numCuts / numUniqueElements;
      offset = step / 2;
    }
    
    if (numCutsPerVariable != static_cast<uint32_t>(-1)) {
      if (numCuts < numCutsPerVariable) ext_throwError("Number of induced cut points in new predictor less than previous: old splits would be invalid.");
      if (numCuts > numCutsPerVariable) ext_issueWarning("Number of induced cut points in new predictor greater than previous: ignoring extra quantiles.");
    } else {
      numCutsPerVariable = static_cast<uint32_t>(numCuts);
      cutPoints = new double[numCuts];
    }
    
    sortedElements.clear();
    sortedElements.assign(uniqueElements.begin(), uniqueElements.end());
      
    for (size_t i = 0; i < numCutsPerVariable; ++i) {
      size_t index = std::min(i * step + offset, numUniqueElements - 2);
      cutPoints[i] = 0.5 * (sortedElements[index] + sortedElements[index + 1]);
    }
  }
  
  void setCutPointsUniformly(BARTFit& fit, const double* x, uint32_t maxNumCuts,
                             uint32_t& numCutsPerVariable, double*& cutPoints)
  {
    Data& data(fit.data);
    
    double xMax, xMin, xIncrement;
    
    xMax = x[0]; xMin = x[0];
    for (size_t i = 1; i < data.numObservations; ++i) {
      double x_i = x[i];
      if (x_i < xMin) xMin = x_i;
      if (x_i > xMax) xMax = x_i;
    }
    
    if (numCutsPerVariable == static_cast<uint32_t>(-1)) {
      numCutsPerVariable = maxNumCuts;
      cutPoints = new double[numCutsPerVariable];
    }
      
    xIncrement = (xMax - xMin) / static_cast<double>(numCutsPerVariable + 1);
      
    for (size_t i = 0; i < numCutsPerVariable; ++i) cutPoints[i] = xMin + (static_cast<double>(i + 1)) * xIncrement;
  }
  
  void setInitialFit(BARTFit& fit) {
    Control& control(fit.control);
    Data& data(fit.data);
    State& state(fit.state);
    
    size_t length = data.numObservations * control.numTrees;
    state.treeFits = new double[length];
    for (size_t offset = 0; offset < length; ++offset) state.treeFits[offset] = 0.0;
    
    state.totalFits = new double[data.numObservations];
    for(size_t i = 0; i < data.numObservations; ++i) state.totalFits[i] = 0.0;
    
    if (data.numTestObservations > 0) {
      state.totalTestFits = new double[data.numTestObservations];
      for (size_t i = 0; i < data.numTestObservations; ++i) state.totalTestFits[i] = 0.0;
    }
    
    double sigma_sq = CHEAT_SIGMA(fit.model.responseModel);
    for (size_t i = 0; i < control.numTrees; ++i) {
      TREE_AT(state.trees, i, fit.scratch.nodeSize)->updateWithNewCovariates(fit, sigma_sq);
    }
  }
  
  void initializeLatents(BARTFit& fit) {
    const Data& data(fit.data);
    Scratch& scratch(fit.scratch);
    
    double* z = const_cast<double*>(fit.scratch.yRescaled);
    
    // z = 2.0 * y - 1.0 - offset; so -1 if y == 0 and 1 if y == 1 when offset == 0
#ifndef MATCH_BAYES_TREE
    ext_setVectorToConstant(z, data.numObservations, -1.0);
    if (data.offset != NULL) ext_addVectorsInPlace(data.offset, data.numObservations, -1.0, z);
    ext_addVectorsInPlace(data.y, data.numObservations, 2.0, z);
    
    // shouldn't be used, but will leave at reasonable values; if anyone cares, should
    // look at offset var for min/max/range
    scratch.dataScale.min = -1.0;
    scratch.dataScale.max =  1.0;
    scratch.dataScale.range = 2.0;
#else
    // BayesTree initialized the latents to be -2 and 0; was probably a bug
    ext_setVectorToConstant(z, data.numObservations, -2.0);
    if (data.offset != NULL) ext_addVectorsInPlace(data.offset, data.numObservations, -1.0, z);
    ext_addVectorsInPlace(data.y, data.numObservations, 2.0, z);
    
    scratch.dataScale.min = -2.0;
    scratch.dataScale.max =  0.0;
    scratch.dataScale.range = 2.0;
#endif
  }
  
  void rescaleResponse(BARTFit& fit) {
    const Data& data(fit.data);
    Scratch& scratch(fit.scratch);
    
    double* yRescaled = const_cast<double*>(fit.scratch.yRescaled);
    
    if (data.offset != NULL) {
      ext_addVectors(data.offset, data.numObservations, -1.0, data.y, yRescaled);
    } else {
      std::memcpy(yRescaled, data.y, data.numObservations * sizeof(double));
    }
    
    scratch.dataScale.min = yRescaled[0];
    scratch.dataScale.max = yRescaled[0];
    for (size_t i = 1; i < data.numObservations; ++i) {
      if (yRescaled[i] < scratch.dataScale.min) scratch.dataScale.min = yRescaled[i];
      if (yRescaled[i] > scratch.dataScale.max) scratch.dataScale.max = yRescaled[i];
    }
    scratch.dataScale.range = scratch.dataScale.max - scratch.dataScale.min;
    
    // yRescaled = (y - offset - min) / (max - min) - 0.5
    ext_addScalarToVectorInPlace(   yRescaled, data.numObservations, -scratch.dataScale.min);
    ext_scalarMultiplyVectorInPlace(yRescaled, data.numObservations, 1.0 / scratch.dataScale.range);
    ext_addScalarToVectorInPlace(   yRescaled, data.numObservations, -0.5);
  }
  
  inline void calculateResidualsFromOtherTreeFits(const double* y, const double* totalFits, const double* oldTreeFits,
                                                  size_t numObservations, double* r)
  {
    // treeY = y - (totalFits - oldTreeFits)
    std::memcpy(r, y, numObservations * sizeof(double));
    ext_addVectorsInPlace(totalFits, numObservations, -1.0, r);
    ext_addVectorsInPlace(oldTreeFits, numObservations, 1.0, r);
  }
  
  inline void updateTotalFits(const double* oldTreeFits, const double* currTreeFits, size_t numObservations, double* totalFits)
  {
    // totalFits += currFits - oldTreeFits
    ext_addVectorsInPlace(const_cast<const double*>(oldTreeFits), numObservations, -1.0, totalFits);
    ext_addVectorsInPlace(const_cast<const double*>(currTreeFits), numObservations, 1.0, totalFits);
  }
  
  // multithread-this!
  void sampleProbitLatentVariables(BARTFit& fit, const double* fits, double* z) {
    for (size_t i = 0; i < fit.data.numObservations; ++i) {      
#ifndef MATCH_BAYES_TREE
      double mean = fits[i];
      double offset = 0.0;
      if (fit.data.offset != NULL) offset = fit.data.offset[i];
      
      if (fit.data.y[i] > 0.0) {
        z[i] = ext_rng_simulateLowerTruncatedNormalScale1(fit.control.rng, mean, -offset);
      } else {
        z[i] = ext_rng_simulateUpperTruncatedNormalScale1(fit.control.rng, mean, -offset);
      }
#else
      double prob;
      
      double mean = fits[i];
      if (fit.data.offset != NULL) mean += fit.data.offset[i];
      
      double u = ext_rng_simulateContinuousUniform(fit.control.rng);
      if (fit.data.y[i] > 0.0) {
        prob = u + (1.0 - u) * ext_cumulativeProbabilityOfNormal(0.0, mean, 1.0);
        z[i] = ext_quantileOfNormal(prob, mean, 1.0);
      } else {
        prob = u + (1.0 - u) * ext_cumulativeProbabilityOfNormal(0.0, -mean, 1.0);
        z[i] = mean - ext_quantileOfNormal(prob, 0.0, 1.0);
      }
#endif
      
    }
  }
  
  void storeSamples(const BARTFit& fit, Results& results, const double* trainingSample, const double* testSample,
                    double sigma, const uint32_t* variableCounts, size_t simNum)
  {
    const Data& data(fit.data);
    const Control& control(fit.control);
    const Scratch& scratch(fit.scratch);
    
    if (control.responseIsBinary) {
      if (control.keepTrainingFits) {
        double* trainingSamples = results.trainingSamples + simNum * data.numObservations;
        std::memcpy(trainingSamples, trainingSample, data.numObservations * sizeof(double));
        if (data.offset != NULL) ext_addVectorsInPlace(data.offset, data.numObservations, 1.0, trainingSamples);
      }
      
      if (data.numTestObservations > 0) {
        double* testSamples = results.testSamples + simNum * data.numTestObservations;
        std::memcpy(testSamples, testSample, data.numTestObservations * sizeof(double));
        if (data.testOffset != NULL) ext_addVectorsInPlace(data.testOffset, data.numTestObservations, 1.0, testSamples);
      }
      
      results.sigmaSamples[simNum] = 1.0;
      
    } else {
      if (control.keepTrainingFits) {
        double* trainingSamples = results.trainingSamples + simNum * data.numObservations;
        // set training to dataScale.range * (totalFits + 0.5) + dataScale.min + offset
        ext_setVectorToConstant(trainingSamples, data.numObservations, scratch.dataScale.range * 0.5 + scratch.dataScale.min);
        ext_addVectorsInPlace(trainingSample, data.numObservations, scratch.dataScale.range, trainingSamples);
        if (data.offset != NULL) ext_addVectorsInPlace(data.offset, data.numObservations, 1.0, trainingSamples);
      }
      
      if (data.numTestObservations > 0) {
        double* testSamples = results.testSamples + simNum * data.numTestObservations;
        ext_setVectorToConstant(testSamples, data.numTestObservations, scratch.dataScale.range * 0.5 + scratch.dataScale.min);
        ext_addVectorsInPlace(testSample, data.numTestObservations, scratch.dataScale.range, testSamples);
        if (data.testOffset != NULL) ext_addVectorsInPlace(data.testOffset, data.numTestObservations, 1.0, testSamples);
      }
      
      results.sigmaSamples[simNum] = sigma * scratch.dataScale.range;
    }
    
    double* variableCountSamples = results.variableCountSamples + simNum * data.numPredictors;
    for (size_t i = 0; i < data.numPredictors; ++i) variableCountSamples[i] = static_cast<double>(variableCounts[i]);
  }
  
  void updateTrainingFits(const BARTFit& fit, double* currTreeFits, double* totalFits)
  {
    const Data& data(fit.data);
    const Control& control(fit.control);
    const Scratch& scratch(fit.scratch);
    const State& state(fit.state);
        
    for (size_t i = 0; i < control.numTrees; ++i) {
      Tree& tree_i(*TREE_AT(state.trees, i, scratch.nodeSize));
      double* treeFits = currTreeFits + i * data.numObservations;
      
      ext_addVectorsInPlace(treeFits, data.numObservations, -1.0, totalFits);
      
      tree_i.getFits(fit, treeFits, NULL);
      // tree_i.setCurrentFitsFromAverages(*this, nodePosteriorPredictions[i], treeFits, NULL);
      
      ext_addVectorsInPlace(treeFits, data.numObservations, 1.0, totalFits);
    }
  }
  
  void updateTestFits(const BARTFit& fit, double* totalTestFits)
  {
    const Data& data(fit.data);
    const Control& control(fit.control);
    const Scratch& scratch(fit.scratch);
    const State& state(fit.state);
    
    double* currTestFits = new double[data.numTestObservations];
    
    ext_setVectorToConstant(totalTestFits, data.numTestObservations, 0.0);
    
    for (size_t i = 0; i < control.numTrees; ++i) {
      Tree& tree_i(*TREE_AT(state.trees, i, scratch.nodeSize));
      // const double* treeFits = state.treeFits + i * data.numObservations;
      
      // const double* nodePosteriorPredictions = tree_i.recoverAveragesFromFits(*this, treeFits);
      
      // tree_i.setCurrentFitsFromAverages(*this, nodePosteriorPredictions, NULL, currTestFits);
      tree_i.getFits(fit, NULL, currTestFits);
      
      ext_addVectorsInPlace(currTestFits, data.numTestObservations, 1.0, state.totalTestFits);
      
      // delete [] nodePosteriorPredictions;
    }
    
    delete [] currTestFits; 
  }
  
  
  void countVariableUses(const BARTFit& fit, uint32_t* variableCounts)
  {
    for (size_t i = 0; i < fit.data.numPredictors; ++i) variableCounts[i] = 0;
    
    for (size_t i = 0; i < fit.control.numTrees; ++i) {
      TREE_AT(fit.state.trees, i, fit.scratch.nodeSize)->countVariableUses(variableCounts);
    }
  }

#ifdef HAVE_GETTIMEOFDAY
  double subtractTimes(struct timeval end, struct timeval start) {
    return (1.0e6 * (static_cast<double>(end.tv_sec - start.tv_sec)) + static_cast<double>(end.tv_usec - start.tv_usec)) / 1.0e6;
  }
#else
  double subtractTimes(time_t end, time_t start) { return static_cast<double>(end - start); }
#endif
}

#include <external/binaryIO.h>
#include <sys/stat.h> // permissions
#include <fcntl.h>    // open flags
#include <unistd.h>   // unlink
#include "binaryIO.hpp"

#define VERSION_STRING_LENGTH 8

#ifndef S_IRGRP
#define S_IRGRP 0
#endif
#ifndef S_IROTH
#define S_IROTH 0
#endif

namespace dbarts {
  
  bool BARTFit::saveToFile(const char* fileName) const
  {
    ext_binaryIO bio;
    int errorCode = ext_bio_initialize(&bio, fileName, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    
    if (errorCode != 0) {
      ext_issueWarning("unable to open file: %s", std::strerror(errorCode));
      
      return false;
    }
    
    // because of a peculiarity of how this gets mucked around on creation, this is necessary
    double scaleFactor = control.responseIsBinary ? 1.0 : (data.sigmaEstimate / scratch.dataScale.range);
    double originalScale = model.responseModel->getScale(*this);
    model.responseModel->setScale(*this, originalScale / (scaleFactor * scaleFactor));
    
    if (ext_bio_writeNChars(&bio, "00.08.00", VERSION_STRING_LENGTH) != 0) goto save_failed;
    
    if (writeControl(control, &bio) == false) goto save_failed;
    ext_printf("wrote control\n");
    if (writeModel(model, &bio) == false) goto save_failed;
    ext_printf("wrote model\n");
    if (writeData(data, &bio) == false) goto save_failed;
    ext_printf("wrote model\n");
    
    if (writeState(*this, &bio) == false) goto save_failed;
    ext_printf("wrote state\n");
    
    ext_bio_invalidate(&bio);
    
    model.responseModel->setScale(*this, originalScale);
    
    printTerminalSummary(*this);
    
    return true;
    
save_failed:
    ext_bio_invalidate(&bio);
    model.responseModel->setScale(*this, originalScale);
    unlink(fileName);
    return false; 
  }
  
  
  BARTFit* BARTFit::loadFromFile(const char* fileName) {
    ext_binaryIO bio;
    int errorCode = ext_bio_initialize(&bio, fileName, O_RDONLY, 0);
    if (errorCode != 0) { ext_issueWarning("unable to open file: %s", std::strerror(errorCode)); return NULL; }
    
    char versionString[8];
    if (ext_bio_readNChars(&bio, versionString, VERSION_STRING_LENGTH) != 0) { ext_issueWarning("unable to read version string from file"); return NULL; }
    
    if (strncmp(versionString, "00.08.00", VERSION_STRING_LENGTH) != 0) { ext_issueWarning("unrecognized file formal"); return NULL; }
    
    Control control;
    Model model;
    Data data;
    BARTFit* result = NULL;;
    
    if (readControl(control, &bio) == false) goto load_failed;
    ext_printf("read control\n");
    if (readModel(model, &bio) == false) goto load_failed;
    ext_printf("read model\n");
    if (readData(data, &bio) == false) goto load_failed;
    ext_printf("read data\n");
    
    result = new BARTFit(control, model, data);
    
    if (readState(*result, &bio) == false) goto load_failed;
    ext_printf("read state\n");
    
    ext_bio_invalidate(&bio);
    
    printTerminalSummary(*result);
    
    return result;
    
load_failed:
    ext_bio_invalidate(&bio);
    
    delete result;
    
    delete [] data.maxNumCuts;
    delete [] data.variableTypes;
      
    delete model.responseModel;
    delete model.endNodeModel;
    delete model.treePrior;
    
    return NULL;
  }
}
