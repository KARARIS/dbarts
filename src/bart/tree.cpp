#include "config.hpp"
#include "tree.hpp"

#include <bart/cstdint>

#include <external/alloca.h>
#include <external/stats.h>

#include <bart/bartFit.hpp>
#include <bart/data.hpp>
#include <bart/model.hpp>

using std::uint32_t;

namespace {
  using namespace bart;
  
  // multithread me!
  size_t* createObservationToNodeIndexMap(const BARTFit& fit, const Node& top,
                                          const double* Xt, size_t numObservations)
  {
    if (numObservations == 0) return NULL;
    
    // bart::NodeIndexMap nodeToIndexMap(top.getBottomIndexMap());
    
    size_t* map = new size_t[numObservations];
        
    for (size_t i = 0; i < numObservations; ++i) {
      const Node* bottomNode = top.findBottomNode(fit, Xt + i * fit.data.numPredictors);
      
      map[i] = bottomNode->enumerationIndex;
    }
    
    return map;
  }
}

namespace bart {
  void Tree::setNodeAverages(const BARTFit& fit, const double* y) {
    NodeVector bottomNodes(getBottomNodes());
    
    size_t numBottomNodes = bottomNodes.size();
    
    for (size_t i = 0; i < numBottomNodes; ++i) {
      bottomNodes[i]->setAverage(fit, y);
    }
  }
  
  void Tree::getCurrentFits(const BARTFit& fit, double* trainingFits, double* testFits)
  {
    NodeVector bottomNodes(top.getAndEnumerateBottomVector());
    size_t numBottomNodes = bottomNodes.size();
    
    double* nodePosteriorPredictions = NULL;
    
    if (testFits != NULL) nodePosteriorPredictions = ext_stackAllocate(numBottomNodes, double);
    
    for (size_t i = 0; i < numBottomNodes; ++i) {
      const Node& bottomNode(*bottomNodes[i]);
      
      double posteriorPrediction = bottomNode.drawFromPosterior(*fit.model.muPrior, fit.sigma * fit.sigma);
      bottomNode.setPredictions(trainingFits, posteriorPrediction);
      
      if (testFits != NULL) nodePosteriorPredictions[i] = posteriorPrediction;
    }
    
    if (testFits != NULL) {
      size_t* observationNodeMap = createObservationToNodeIndexMap(fit, top, fit.Xt_test, fit.data.numTestObservations);
      for (size_t i = 0; i < fit.data.numTestObservations; ++i) testFits[i] = nodePosteriorPredictions[observationNodeMap[i]];
      delete [] observationNodeMap;
      
       ext_stackFree(nodePosteriorPredictions);
    }
  }
  
  void Tree::countVariableUses(uint32_t* variableCounts) {
    top.countVariableUses(variableCounts);
  }
  

  // static Node* Tree::duplicateBranch(const BARTFit& fit, const Node* node);
}
