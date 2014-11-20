#include "config.hpp"
#include <dbarts/endNodeModel.hpp>

#include <cmath>
#include "dbarts/cstdint.hpp"
#include <cstring> // strncpy

#include <external/alloca.h>
#include <external/binaryIO.h>
#include <external/io.h>
#include <external/linearAlgebra.h>
#include <external/random.h>
#include <external/stats_mt.h>

#include <dbarts/bartFit.hpp>
#include <dbarts/control.hpp>
#include "node.hpp"

using std::size_t;
using std::uint32_t;

namespace {
  namespace MeanNormal {
    using namespace dbarts;
  
    struct NodeScratch {
      double mu; // this plays two roles: average of ys when calculating integrated log-like and posterior draw of node-mean
      double numEffectiveObservations;
    };
    
    void print(const BARTFit& fit);
    
    double logPrior(const BARTFit& fit, const Node& node);
    double logIntegratedLikelihood(const BARTFit& fit, const Node& node, const double* y, double residualVariance);
    void drawFromPrior(const BARTFit& fit, const Node& node);
    void drawFromPosterior(const BARTFit& fit, const Node& node, const double* y, double residualVariance);
    
    double getPrediction(const BARTFit& fit, const Node& node, const double* Xt);
    void getPredictions(const BARTFit& fit, const Node& node, double* y_hat);
    // void getPredictionsForIndices(const BARTFit& fit, const Node& node, const double* y, const size_t* indices, double* y_hat);
    
    void storeScratch(const BARTFit& fit, const Node& source, void* target);
    void restoreScratch(const BARTFit& fit, void* source, Node& target);
    
    void printScratch(const BARTFit& fit, const Node& node);
    
    void updateScratchWithMemberships(const BARTFit& fit, const Node& node, double residualVariance);
    void prepareScratchForLikelihoodAndPosteriorCalculations(const BARTFit& fit, const Node& node, const double* y, double residualVariance);
    void prepareScratchFromChildren(const BARTFit& fit, Node& parent, const double* y, double residualVariance, const Node& leftChild, const Node& rightChild);
    
    int writeScratch(const Node& node, ext_binaryIO* bio);
    int readScratch(Node& node, ext_binaryIO* bio);
  }
}


namespace dbarts {  
  namespace EndNode {
    void initializeMeanNormalModel(MeanNormalModel& model)
    {
      model.perNodeScratchSize = sizeof(::MeanNormal::NodeScratch);
      model.info = CONDITIONALLY_INTEGRABLE | PREDICTION_IS_CONSTANT;
      std::strncpy(model.name, meanNormalName, sizeof(model.name));
      model.numParameters = 1;
      model.precision = 1.0;

      model.print = &::MeanNormal::print;
      model.getParameters = NULL;
      model.setParameters = NULL;
      
      model.computeLogPrior = &::MeanNormal::logPrior;
      model.computeLogIntegratedLikelihood = &::MeanNormal::logIntegratedLikelihood;
      model.drawFromPrior = &::MeanNormal::drawFromPrior;
      model.drawFromPosterior = &::MeanNormal::drawFromPosterior;
      
      model.getPrediction = &::MeanNormal::getPrediction;
      model.getPredictions = &::MeanNormal::getPredictions;
      
      model.createScratch = NULL;
      model.destroyScratch = NULL;
      model.storeScratch = &::MeanNormal::storeScratch;
      model.restoreScratch = &::MeanNormal::restoreScratch;
      
      model.printScratch = &::MeanNormal::printScratch;
      
      model.updateScratchWithMemberships = &::MeanNormal::updateScratchWithMemberships;
      model.prepareScratchForLikelihoodAndPosteriorCalculations = &::MeanNormal::prepareScratchForLikelihoodAndPosteriorCalculations;
      model.updateMembershipsAndPrepareScratch = &::MeanNormal::prepareScratchForLikelihoodAndPosteriorCalculations;
      model.prepareScratchFromChildren = &::MeanNormal::prepareScratchFromChildren;
      
      model.writeScratch = &::MeanNormal::writeScratch;
      model.readScratch = &::MeanNormal::readScratch;
    }
    
    MeanNormalModel* createMeanNormalModel()
    {
      MeanNormalModel* result = new MeanNormalModel;
      initializeMeanNormalModel(*result);
      
      return result;
    }
    
    void initializeMeanNormalModel(MeanNormalModel& model, const Control& control, double k)
    {
      initializeMeanNormalModel(model);
      
      double sigma = (control.responseIsBinary ? 3.0 : 0.5) /  (k * std::sqrt(static_cast<double>(control.numTrees)));
      model.precision = 1.0 / (sigma * sigma);
    }
    
    MeanNormalModel* createMeanNormalModel(const Control& control, double k)
    {
      MeanNormalModel* result = createMeanNormalModel();
      initializeMeanNormalModel(*result, control, k);
      
      return result;
    }
  }
}

// normal prior on single mean parameter
namespace {
  namespace MeanNormal {
  
#define DEFINE_MODEL(_FIT_) const MeanNormalModel& model(*static_cast<const MeanNormalModel*>(_FIT_.model.endNodeModel))
#define DEFINE_SCRATCH(_NODE_) NodeScratch& scratch(*static_cast<NodeScratch*>(_NODE_.getScratch()))
  
    using dbarts::BARTFit;
    using dbarts::Node;
     
    using dbarts::EndNode::MeanNormalModel;
    
    void print(const BARTFit& fit)
    {
      DEFINE_MODEL(fit);
      
      double sigma = std::sqrt(1.0 / model.precision);
      double k = (fit.control.responseIsBinary ? 3.0 : 0.5) / (sigma * std::sqrt(static_cast<double>(fit.control.numTrees)));
      ext_printf("\tend node - mean w/normal; k = %f\n", k);
    }
    
    double computeVarianceForNode(const BARTFit& fit, const Node& node, const double* y, double average)
    {
      size_t numObservations = node.getNumObservations();
      uint32_t updateType = (node.isTop() == false ? 1 : 0) + (fit.data.weights != NULL ? 2 : 0);
      switch (updateType) {
        case 0: // isTop && weights == NULL
        return ext_mt_computeVarianceForKnownMean(fit.threadManager, y, numObservations, average);
        
        case 1: // !isTop && weights == NULL
        return ext_mt_computeIndexedVarianceForKnownMean(fit.threadManager, y, node.getObservationIndices(), numObservations, average);
        
        case 2: // isTop && weights != NULL
        return ext_mt_computeWeightedVarianceForKnownMean(fit.threadManager, y, numObservations, fit.data.weights, average);
        
        case 3: // !isTop && weights != NULL
        return ext_mt_computeIndexedWeightedVarianceForKnownMean(fit.threadManager, y, node.getObservationIndices(), numObservations, fit.data.weights, average);
        
        default:
        break;
      }
      
      return NAN;
    }
    
    double logPrior(const BARTFit& fit, const Node& node)
    {
      DEFINE_MODEL(fit);
      DEFINE_SCRATCH(node);
      
      return -0.5 * scratch.mu * scratch.mu * model.precision;
    }
    
    double logIntegratedLikelihood(const BARTFit& fit, const Node& node, const double* y, double residualVariance)
    {
      size_t numObservationsInNode = node.getNumObservations();
      if (numObservationsInNode == 0) return 0.0;
      
      DEFINE_MODEL(fit);
      DEFINE_SCRATCH(node);
      
      double y_bar = scratch.mu;
      double var_y = computeVarianceForNode(fit, node, y, y_bar);
        
      double dataPrecision = scratch.numEffectiveObservations / residualVariance;
      
      double result;
      result  = 0.5 * std::log(model.precision / (model.precision + dataPrecision));
      result -= 0.5 * (var_y / residualVariance) * static_cast<double>(numObservationsInNode - 1);
      result -= 0.5 * ((model.precision * y_bar) * (dataPrecision * y_bar)) / (model.precision + dataPrecision);
      
      return result;
    }
    
    void drawFromPrior(const BARTFit& fit, const Node& node)
    {
      DEFINE_MODEL(fit);
      DEFINE_SCRATCH(node);
      
      scratch.mu = ext_rng_simulateStandardNormal(fit.control.rng) / std::sqrt(model.precision);
    }
    
    void drawFromPosterior(const BARTFit& fit, const Node& node, const double*, double residualVariance)
    {
      DEFINE_MODEL(fit);
      DEFINE_SCRATCH(node);
      
      double posteriorPrecision = scratch.numEffectiveObservations / residualVariance;
    
      double posteriorMean = posteriorPrecision * scratch.mu / (model.precision + posteriorPrecision);
      double posteriorSd   = 1.0 / std::sqrt(model.precision + posteriorPrecision);
    
      scratch.mu = posteriorMean + posteriorSd * ext_rng_simulateStandardNormal(fit.control.rng);
    }
    
    double getPrediction(const BARTFit&, const Node& node, const double*)
    {
      DEFINE_SCRATCH(node);
      
      return scratch.mu;
    }
    
    void getPredictions(const BARTFit&, const Node& node, double* y_hat)
    {
      DEFINE_SCRATCH(node);
      
      if (node.isTop()) 
        ext_setVectorToConstant(y_hat, node.getNumObservations(), scratch.mu);
      else
        ext_setIndexedVectorToConstant(y_hat, node.getObservationIndices(), node.getNumObservations(), scratch.mu);
    }
   
    void storeScratch(const BARTFit&, const Node& node, void* targetPtr) {
      const NodeScratch& source(*static_cast<const NodeScratch*>(node.getScratch()));
      NodeScratch& target(*static_cast<NodeScratch*>(targetPtr));
      
      target.mu = source.mu;
      target.numEffectiveObservations = source.numEffectiveObservations;
    }
    
    void restoreScratch(const BARTFit&, void* sourcePtr, Node& node) {
      const NodeScratch& source(*static_cast<const NodeScratch*>(sourcePtr));
      NodeScratch& target(*static_cast<NodeScratch*>(node.getScratch()));
      
      target.mu = source.mu;
      target.numEffectiveObservations = source.numEffectiveObservations;
    }

    
    void printScratch(const BARTFit&, const Node& node)
    {
      DEFINE_SCRATCH(node);
      ext_printf(" ave: %f", scratch.mu);
    }
    
    void prepareScratchForLikelihoodAndPosteriorCalculations(const BARTFit& fit, const Node& node, const double* y, double)
    {
      DEFINE_SCRATCH(node);
      
      size_t numObservations = node.getNumObservations();
      uint32_t updateType = (node.isTop() == false ? 1 : 0) + (fit.data.weights != NULL ? 2 : 0);
      switch (updateType) {
        case 0: // isTop && weights == NULL
        scratch.mu = ext_mt_computeMean(fit.threadManager, y, numObservations);
        scratch.numEffectiveObservations = static_cast<double>(numObservations);
        break;
        
        case 1: // !isTop && weights == NULL
        scratch.mu = ext_mt_computeIndexedMean(fit.threadManager, y, node.getObservationIndices(), numObservations);
        scratch.numEffectiveObservations = static_cast<double>(numObservations);
        break;
        
        case 2: // isTop && weights != NULL
        scratch.mu = ext_mt_computeWeightedMean(fit.threadManager, y, numObservations, fit.data.weights, &scratch.numEffectiveObservations);
        break;
        
        case 3: // !isTop && weights != NULL
        scratch.mu = ext_mt_computeIndexedWeightedMean(fit.threadManager, y, node.getObservationIndices(), numObservations, fit.data.weights, &scratch.numEffectiveObservations);
        break;
        
        default:
        break;
      }
    }
    
    void updateScratchWithMemberships(const BARTFit& fit, const Node& node, double)
    {
      DEFINE_SCRATCH(node);
      
      if (fit.data.weights == NULL) {
        scratch.numEffectiveObservations = static_cast<double>(node.getNumObservations());
      } else {
        if (node.isTop()) {
          scratch.numEffectiveObservations = ext_sumVectorElements(fit.data.weights, fit.data.numObservations);
        } else {
          scratch.numEffectiveObservations = 0.0;
          size_t numObservations = node.getNumObservations();
          const size_t* observationIndices = node.getObservationIndices();
          for (size_t i = 0; i < numObservations; ++i) scratch.numEffectiveObservations += fit.data.weights[observationIndices[i]];
        }
      }
    }
    
    void prepareScratchFromChildren(const BARTFit&, Node& parentNode, const double*, double, const Node& leftChildNode, const Node& rightChildNode)
    {
      NodeScratch& parent(*static_cast<NodeScratch*>(parentNode.getScratch()));
      const NodeScratch& leftChild(*static_cast<const NodeScratch*>(leftChildNode.getScratch()));
      const NodeScratch& rightChild(*static_cast<const NodeScratch*>(rightChildNode.getScratch()));

      parent.numEffectiveObservations = leftChild.numEffectiveObservations + rightChild.numEffectiveObservations;    
      parent.mu = leftChild.mu * (leftChild.numEffectiveObservations / parent.numEffectiveObservations) +
                  rightChild.mu * (rightChild.numEffectiveObservations / parent.numEffectiveObservations);
    }
    
    int writeScratch(const Node& node, ext_binaryIO* bio) {
      DEFINE_SCRATCH(node);
      
      int errorCode = ext_bio_writeDouble(bio, scratch.mu);
      if (errorCode != 0) return errorCode;
      
      return ext_bio_writeDouble(bio, scratch.numEffectiveObservations);
    }
    
    int readScratch(Node& node, ext_binaryIO* bio) {
      DEFINE_SCRATCH(node);
      
      int errorCode = ext_bio_readDouble(bio, &scratch.mu);
      if (errorCode != 0) return errorCode;
      
      return ext_bio_readDouble(bio, &scratch.numEffectiveObservations);
    }
  } // namespace MeanNormal
  
#undef DEFINE_SCRATCH
#undef DEFINE_MODEL

} // anonymous namespace

// ordinary linear regression with gaussian prior
namespace {
  namespace LinearRegressionNormal {
    using namespace dbarts;
    
    struct NodeScratch {
      double* posteriorCovarianceRightFactor;
      double* coefficients;
      double* y;
      double* Xt;
    };
    
    void print(const BARTFit& fit);
    
    double logIntegratedLikelihood(const BARTFit& fit, const Node& node, const double* y, double residualVariance);
    void drawFromPosterior(const BARTFit& fit, const Node& node, const double* y, double residualVariance);

    
    double getPrediction(const BARTFit& fit, const Node& node, const double* Xt);
    void getPredictions(const BARTFit& fit, const Node& node, double* y_hat);
      
    void createScratch(const BARTFit& fit, Node& node);
    void destroyScratch(const BARTFit& fit, void* scratch);

    void storeScratch(const BARTFit& fit, const Node& node, void* target);
    void restoreScratch(const BARTFit& fit, void* source, Node& node); 

    void printScratch(const BARTFit& fit, const Node& node);
    
    void updateScratchWithMemberships(const BARTFit& fit, const Node& node, double residualVariance);
    void prepareScratchForLikelihoodAndPosteriorCalculations(const BARTFit& fit, const Node& node, const double* y, double residualVariance);
    void updateMembershipsAndPrepareScratch(const BARTFit& fit, const Node& node, const double* y, double residualVariance);
    void prepareScratchFromChildren(const BARTFit& fit, Node& parent, const double* y, double residualVariance, const Node& leftChild, const Node& rightChild);
    
  /* 
    int writeScratch(const Node& node, ext_binaryIO* bio);
    int readScratch(Node& node, ext_binaryIO* bio); */
  }
}

namespace dbarts {
  namespace EndNode {
    LinearRegressionNormalModel* createLinearRegressionNormalModel(const Data& data, const double* precisions)
    {
      LinearRegressionNormalModel* result = new LinearRegressionNormalModel;
    
      initializeLinearRegressionNormalModel(*result, data, precisions);
    
      return result;
    }
  
    void initializeLinearRegressionNormalModel(LinearRegressionNormalModel& model, const Data& data, const double* precisions)
    {
      model.perNodeScratchSize = sizeof(::LinearRegressionNormal::NodeScratch);
      model.info = CONDITIONALLY_INTEGRABLE;
      strncpy(model.name, linearRegressionNormalName, sizeof(model.name));
      model.numParameters = data.numPredictors + 1;
      model.precisions = precisions;
     
      model.print = &::LinearRegressionNormal::print;
      model.getParameters = NULL;
      model.setParameters = NULL;
      
      model.computeLogPrior = NULL;
      model.computeLogIntegratedLikelihood = &::LinearRegressionNormal::logIntegratedLikelihood;
      model.drawFromPrior = NULL;
      model.drawFromPosterior = &::LinearRegressionNormal::drawFromPosterior;
      
      model.getPrediction = &::LinearRegressionNormal::getPrediction;
      model.getPredictions = &::LinearRegressionNormal::getPredictions;
      
      model.createScratch = &::LinearRegressionNormal::createScratch;
      model.destroyScratch = &::LinearRegressionNormal::destroyScratch;
      model.storeScratch = &::LinearRegressionNormal::storeScratch;
      model.restoreScratch = &::LinearRegressionNormal::restoreScratch;
      
      model.printScratch = &::LinearRegressionNormal::printScratch;
      
      model.updateScratchWithMemberships = &::LinearRegressionNormal::updateScratchWithMemberships;
      model.prepareScratchForLikelihoodAndPosteriorCalculations = &::LinearRegressionNormal::prepareScratchForLikelihoodAndPosteriorCalculations;
      model.updateMembershipsAndPrepareScratch = &::LinearRegressionNormal::updateMembershipsAndPrepareScratch;
      model.prepareScratchFromChildren = &::LinearRegressionNormal::prepareScratchFromChildren;
      
      model.writeScratch = NULL; // &::LinearRegressionNormal::writeScratch;
      model.readScratch = NULL; // &::LinearRegressionNormal::readScratch;
      
      size_t numPredictors = data.numPredictors + 1;
      model.Xt = new double[numPredictors * data.numObservations];
      double* Xt = const_cast<double*>(model.Xt);
      
      for (size_t col = 0; col < data.numObservations; ++col) {
        Xt[col * numPredictors] = 1.0;
       
        for (size_t row = 0; row < data.numPredictors; ++row) {
          Xt[row + col * numPredictors + 1] = data.X[col + row * data.numObservations];
        } 
      }

      for (size_t row = 0; row < numPredictors; ++row) {
        for (size_t col = 1; col < 5; ++col) {
        }
      }
    }
    
    void destroyLinearRegressionNormalModel(LinearRegressionNormalModel* model)
    {
      invalidateLinearRegressionNormalModel(*model);
      
      delete model;
    }
    
    void invalidateLinearRegressionNormalModel(LinearRegressionNormalModel& model)
    {
      delete [] model.Xt; model.Xt = NULL;
    }
  }
}

namespace {
  namespace LinearRegressionNormal {

  #define DEFINE_MODEL(_FIT_) const LinearRegressionNormalModel& model(*static_cast<const LinearRegressionNormalModel*>(_FIT_.model.endNodeModel))
  #define DEFINE_SCRATCH(_NODE_) NodeScratch& scratch(*static_cast<NodeScratch*>(_NODE_.getScratch()))
    
    using dbarts::BARTFit;
    using dbarts::Node;
    
    using dbarts::EndNode::LinearRegressionNormalModel;
    
    double* createXtForNode(const BARTFit& fit, const Node& node);
    void calculateCovarianceRightFactor(const BARTFit& fit, const Node& node, const double* Xt, double* R, double residualVariance);
    
    void print(const BARTFit& fit) {
      DEFINE_MODEL(fit);
      
      ext_printf("\tend node - lin reg w/normal prior; sds = %.4f", 1.0 / std::sqrt(model.precisions[0]));
      size_t numToPrint = fit.data.numPredictors < 4 ? fit.data.numPredictors + 1 : 5;
      for (size_t i = 1; i < numToPrint; ++i) ext_printf(" %.4f", 1.0 / std::sqrt(model.precisions[i]));
      if (fit.data.numPredictors > 4) ext_printf("...");
      ext_printf("\n");
    }
    
    double logIntegratedLikelihood(const BARTFit& fit, const Node& node, const double*, double residualVariance)
    {
      DEFINE_SCRATCH(node);
      
      size_t numPredictors = fit.data.numPredictors + 1;
      size_t numObservations = node.getNumObservations();
      
      double determinantTerm = 0.0;
      for (size_t i = 0; i < numPredictors; ++i) determinantTerm -= log(scratch.posteriorCovarianceRightFactor[i * (1 + numPredictors)]);
      
      // at this point, coefficients should contain R^-T X' y
      double* y_hat = ext_stackAllocate(numObservations, double);
      double* beta_tilde = ext_stackAllocate(numPredictors, double);
      std::memcpy(beta_tilde, const_cast<const double*>(scratch.coefficients), numPredictors * sizeof(double));
      
      // R^-1 R^-T X' y
      ext_solveTriangularSystemInPlace(const_cast<const double*>(scratch.posteriorCovarianceRightFactor), numPredictors,
                                       false, EXT_TRIANGLE_TYPE_UPPER, beta_tilde, 1);
      
      // X R^-1 R^-T X' y
      ext_multiplyMatrixIntoVector(scratch.Xt, numPredictors, numObservations, true, beta_tilde, y_hat);
      
      // y_hat := y_hat - y
      ext_addVectorsInPlace(scratch.y, numObservations, -1.0, y_hat);
      
      // exp(-0.5 y'(y - y_hat) / sigma^2)
      double exponentialTerm = 0.5 * ext_dotProduct(scratch.y, numObservations, y_hat) / residualVariance;
      
      ext_stackFree(beta_tilde);
      ext_stackFree(y_hat);
      
      return determinantTerm + exponentialTerm;
    }
    
    void drawFromPosterior(const BARTFit& fit, const Node& node, const double*, double residualVariance)
    {
      DEFINE_SCRATCH(node);
      
      double sigma = std::sqrt(residualVariance);
      
      size_t numPredictors = fit.data.numPredictors + 1;
      
      for (size_t i = 0; i < numPredictors; ++i) scratch.coefficients[i] += ext_rng_simulateStandardNormal(fit.control.rng) * sigma;
      
      // coefficients are now parameters
      ext_solveTriangularSystemInPlace(const_cast<const double*>(scratch.posteriorCovarianceRightFactor), numPredictors,
                                       false, EXT_TRIANGLE_TYPE_UPPER, scratch.coefficients, 1);
    }
    
    double getPrediction(const BARTFit& fit, const Node& node, const double* Xt)
    {
      DEFINE_SCRATCH(node);
      
      // be sneaky about intercept
      return scratch.coefficients[0] + ext_dotProduct(Xt, fit.data.numPredictors, scratch.coefficients + 1);
    }
    
    void getPredictions(const BARTFit& fit, const Node& node, double* y_hat)
    {
      DEFINE_MODEL(fit);
      DEFINE_SCRATCH(node);
      
      if (node.isTop()) {
        ext_multiplyMatrixIntoVector(model.Xt, fit.data.numPredictors + 1, fit.data.numObservations, true,
                                     scratch.coefficients, y_hat);
      } else {
        size_t numObservations = node.getNumObservations();
        double* predictions = ext_stackAllocate(numObservations, double);
        ext_multiplyMatrixIntoVector(scratch.Xt, fit.data.numPredictors + 1, numObservations, true,
                                     scratch.coefficients, predictions);

        const size_t* observationIndices = node.getObservationIndices();
        for (size_t i = 0; i < numObservations; ++i) y_hat[observationIndices[i]] = predictions[i];
      }
    }
    
    void createScratch(const BARTFit& fit, Node& node)
    {
      // ext_printf("creating at %p\n", node.getScratch());
      DEFINE_MODEL(fit);
      DEFINE_SCRATCH(node);
      
      size_t numPredictors = fit.data.numPredictors + 1;
      
      scratch.y = NULL;
      scratch.Xt = const_cast<double*>(model.Xt);
      scratch.coefficients = new double[numPredictors];
      // ext_setVectorToConstant(scratch.coefficients, numPredictors, 0.0);
      scratch.posteriorCovarianceRightFactor = new double[numPredictors * numPredictors];
    }
    
    void destroyScratch(const BARTFit& fit, void* scratchPtr)
    {
      // ext_printf("destroying %p\n", scratchPtr);
      DEFINE_MODEL(fit);
      NodeScratch& scratch(*static_cast<NodeScratch*>(scratchPtr));
      
      delete [] scratch.coefficients;
      scratch.coefficients = NULL;
      
      delete [] scratch.posteriorCovarianceRightFactor;
      scratch.posteriorCovarianceRightFactor = NULL;
    
      if (scratch.Xt != model.Xt) {
        delete [] scratch.Xt;
        scratch.Xt = NULL;
        // if (scratch.y != NULL) ext_printf("    dealloc: %p\n", scratch.y);
        delete [] scratch.y;
        scratch.y  = NULL;
      }
    }
    
    void storeScratch(const BARTFit& fit, const Node& sourceNode, void* targetPtr)
    {
      // ext_printf("storing from %p to %p\n", sourceNode.getScratch(), targetPtr);
      DEFINE_MODEL(fit);
      NodeScratch& target(*static_cast<NodeScratch*>(targetPtr));
      const NodeScratch& source(*static_cast<const NodeScratch*>(sourceNode.getScratch()));
      
      size_t numPredictors = fit.data.numPredictors + 1;
      
      target.coefficients = new double[numPredictors];
      target.posteriorCovarianceRightFactor = new double[numPredictors * numPredictors];
      
      std::memcpy(target.coefficients, source.coefficients, numPredictors * sizeof(double));
      std::memcpy(target.posteriorCovarianceRightFactor, source.posteriorCovarianceRightFactor, numPredictors * numPredictors * sizeof(double));
      
      if (source.Xt != model.Xt) {
        size_t numObservations = sourceNode.getNumObservations();
        
        target.Xt = new double[numPredictors * numObservations];
        std::memcpy(target.Xt, source.Xt, numPredictors * numObservations * sizeof(double));
        target.y  = new double[numObservations];
        // ext_printf("    copy to %p from %p\n", target.y);
        std::memcpy(target.y, source.y, numObservations * sizeof(double));
      } else {
        target.Xt = const_cast<double*>(model.Xt);
        target.y  = const_cast<double*>(source.y);
      }
    }
    
    void restoreScratch(const BARTFit& fit, void* sourcePtr, Node& targetNode)
    {
      // ext_printf("restore from %p to %p\n", sourcePtr, targetNode.getScratch());
      DEFINE_MODEL(fit);
      NodeScratch& target(*static_cast<NodeScratch*>(targetNode.getScratch()));
      NodeScratch& source(*static_cast<NodeScratch*>(sourcePtr)); 
      
      // size_t numPredictors = fit.data.numPredictors + 1;
      
      delete [] target.coefficients;
      target.coefficients = source.coefficients;
      delete [] target.posteriorCovarianceRightFactor;
      target.posteriorCovarianceRightFactor = source.posteriorCovarianceRightFactor;
      
      if (target.Xt != model.Xt) {
        delete [] target.Xt;
        // if (target.y != NULL) ext_printf("    move from %p and destroy %p\n", source.y, target.y);
        delete [] target.y;
      }
      target.Xt = source.Xt;
      target.y = source.y;
      /* std::memcpy(target.coefficients, source.coefficients, numPredictors * sizeof(double));
      delete [] source.coefficients;
      std::memcpy(target.posteriorCovarianceRightFactor, source.posteriorCovarianceRightFactor, numPredictors * numPredictors * sizeof(double));
      delete [] source.posteriorCovarianceRightFactor;    

      if (target.Xt != model.Xt) {
        delete [] target.Xt;
        delete [] target.y;
      }
      if (source.Xt != model.Xt) {
        size_t numObservations = targetNode.getNumObservations();
        target.Xt = new double[numPredictors * numObservations];
        std::memcpy(target.Xt, source.Xt, numPredictors * numObservations * sizeof(double));
        target.y  = new double[numObservations];
        std::memcpy(target.y, source.y, numObservations * sizeof(double));
      } else {
        target.Xt = const_cast<double*>(model.Xt);
        target.y  = const_cast<double*>(source.y);
      } */
    } 
    
    
    void printScratch(const BARTFit& fit, const Node& node)
    {
      const NodeScratch& scratch(*static_cast<const NodeScratch*>(node.getScratch()));
      
      size_t numToPrint = fit.data.numPredictors < 4 ? fit.data.numPredictors + 1 : 5;
      for (size_t i = 0; i < numToPrint; ++i) ext_printf(" %f", scratch.coefficients[i]);
      
      /* ext_printf("\nXt (%p):\n", scratch.Xt);
      if (scratch.Xt != NULL) {
        for (size_t i = 0; i < numToPrint; ++i) {
          ext_printf("  %0.6f", scratch.Xt[i]);
          for (size_t j = 1; j < (node.getNumObservations() < 5 ? node.getNumObservations() : 5); ++j)
            ext_printf(" %0.6f", scratch.Xt[i + j * (fit.data.numPredictors + 1)]);
          ext_printf("\n");
        }
      }
      ext_printf("\n");

      ext_printf("y (%p):\n", scratch.y);
      if (scratch.y != NULL) {
        ext_printf("  %0.6f", scratch.y[0]);
        for (size_t i = 0; i < (node.getNumObservations() < 5 ? node.getNumObservations() : 5); ++i)
          ext_printf(" %0.6f", scratch.y[i]);
        ext_printf("\n");
      }
      ext_printf("\n");

      ext_printf("R: %p (%f)\n", scratch.posteriorCovarianceRightFactor, scratch.posteriorCovarianceRightFactor[0]); */ 
    }
    
    void prepareScratchForLikelihoodAndPosteriorCalculations(const BARTFit& fit, const Node& node, const double* y, double)
    {
      // ext_printf("preparing for likelihood at %p\n", node.getScratch());
      DEFINE_SCRATCH(node);
      
      size_t numObservations = node.getNumObservations();
      size_t numPredictors = fit.data.numPredictors + 1;

      if (node.isTop()) {
        scratch.y = const_cast<double*>(y);
      } else {
        // ext_printf("    deleting at prepare %p\n", scratch.y);
        delete [] scratch.y;
        scratch.y = node.subsetVector(y);
        // ext_printf("    subset to %p\n", scratch.y);
      }
      
      // X' y
      ext_multiplyMatrixIntoVector(scratch.Xt, numPredictors, numObservations, false, scratch.y, scratch.coefficients);  
      // R^-T X' y
      ext_solveTriangularSystemInPlace(const_cast<const double*>(scratch.posteriorCovarianceRightFactor), numPredictors,
                                       true, EXT_TRIANGLE_TYPE_UPPER, scratch.coefficients, 1);
    }
    
    void updateScratchWithMemberships(const BARTFit& fit, const Node& node, double residualVariance) {
      // ext_printf("updating with memberships %p\n", node.getScratch());
      DEFINE_MODEL(fit);
      DEFINE_SCRATCH(node);
      
      if (scratch.Xt != model.Xt) {
        delete [] scratch.Xt;
        // if (scratch.y != NULL) ext_printf("    deleting during memberships from %p\n", scratch.y);
        delete [] scratch.y;
      }
      
      scratch.Xt = node.isTop() ? const_cast<double*>(model.Xt) : createXtForNode(fit, node);
      scratch.y = NULL;
      
      calculateCovarianceRightFactor(fit, node, scratch.Xt, scratch.posteriorCovarianceRightFactor, residualVariance);
    }
    
    void updateMembershipsAndPrepareScratch(const BARTFit& fit, const Node& node, const double* y, double residualVariance)
    {
      updateScratchWithMemberships(fit, node, residualVariance);
      
      prepareScratchForLikelihoodAndPosteriorCalculations(fit, node, y, residualVariance);
    }
    
    void prepareScratchFromChildren(const BARTFit& fit, Node& parentNode, const double* y, double residualVariance, const Node& leftChildNode, const Node& rightChildNode)
    {
      // ext_printf("preparing from children at %p (%p, %p)\n", parentNode.getScratch(), leftChildNode.getScratch(), rightChildNode.getScratch());
      DEFINE_MODEL(fit);
      NodeScratch& parent(*static_cast<NodeScratch*>(parentNode.getScratch()));
      const NodeScratch& leftChild(*static_cast<const NodeScratch*>(leftChildNode.getScratch()));
      const NodeScratch& rightChild(*static_cast<const NodeScratch*>(rightChildNode.getScratch()));
      

      size_t numPredictors = fit.data.numPredictors + 1;
      parent.coefficients = new double[numPredictors];
      // ext_setVectorToConstant(parent.coefficients, numPredictors, 0.0);
      parent.posteriorCovarianceRightFactor = new double[numPredictors * numPredictors];
      
      if (!parentNode.isTop()) {
        size_t numObservationsOnLeft  = leftChildNode.getNumObservations();
        size_t numObservationsOnRight = rightChildNode.getNumObservations();
        size_t numObservations = numObservationsOnLeft + numObservationsOnRight;
        
        parent.Xt = new double[numPredictors * numObservations];
        std::memcpy(parent.Xt, leftChild.Xt, numPredictors * numObservationsOnLeft * sizeof(double));
        std::memcpy(parent.Xt + numPredictors * numObservationsOnLeft, rightChild.Xt, numPredictors * numObservationsOnRight * sizeof(double));

        parent.y = new double[numObservations];
        // ext_printf("    allocating %p to fill from children\n", parent.y);
        std::memcpy(parent.y, leftChild.y, numObservationsOnLeft * sizeof(double));
        std::memcpy(parent.y + numObservationsOnLeft, rightChild.y, numObservationsOnRight * sizeof(double));
      } else {
        parent.Xt = const_cast<double*>(model.Xt);
        parent.y  = const_cast<double*>(y);
      }
      
      calculateCovarianceRightFactor(fit, parentNode, parent.Xt, parent.posteriorCovarianceRightFactor, residualVariance);
      
      prepareScratchForLikelihoodAndPosteriorCalculations(fit, parentNode, y, residualVariance);
    }
    
    double* createXtForNode(const BARTFit& fit, const Node& node)
    {
      size_t numObservations = node.getNumObservations();
      size_t numPredictors = fit.data.numPredictors + 1;
      
      double* Xt = new double[numPredictors * numObservations];
      
      const size_t* observationIndices = node.getObservationIndices();
    
      for (size_t col = 0; col < numObservations; ++col) {
        Xt[col * numPredictors] = 1.0;
      
        std::memcpy(Xt + col * numPredictors + 1 /* skip first row */,
                   fit.scratch.Xt + observationIndices[col] * fit.data.numPredictors,
                   fit.data.numPredictors * sizeof(double));
      }
      
      return Xt;
    }
    
    void calculateCovarianceRightFactor(const BARTFit& fit, const Node& node, const double* Xt, double* R, double residualVariance)
    {
      DEFINE_MODEL(fit);
      
      size_t numObservations = node.getNumObservations();
      size_t numPredictors = fit.data.numPredictors + 1;
          
      ext_getSingleMatrixCrossproduct(Xt, numPredictors, numObservations, R, true, EXT_TRIANGLE_TYPE_UPPER);
      
      // add in prior contribution
      for (size_t i = 0; i < numPredictors; ++i) R[i * (1 + numPredictors)] += model.precisions[i] * residualVariance;
      
      ext_getSymmetricPositiveDefiniteTriangularFactorizationInPlace(R, numPredictors, EXT_TRIANGLE_TYPE_UPPER);
    }
  }
#undef DEFINE_SCRATCH
#undef DEFINE_MODEL

}

