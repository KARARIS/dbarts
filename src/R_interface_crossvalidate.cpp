#include "R_interface_crossvalidate.hpp"

#include <cstddef> // size_t
#include <cstring> // strcmp

#include <external/alloca.h>
#include <external/linearAlgebra.h>
#include <external/stats.h>
#include <external/string.h>

#define R_NO_REMAP 1
#include <R.h>
#include <Rinternals.h>

#include <rc/bounds.h>
#include <rc/util.h>

#include <dbarts/control.hpp>
#include <dbarts/data.hpp>
#include <dbarts/model.hpp>

#include "crossvalidate.hpp"

#include "R_interface_common.hpp"

#define asRXLen(_X_) static_cast<R_xlen_t>(_X_)

using std::size_t;

namespace {
  using namespace dbarts;
  using namespace dbarts::xval;
  
  SEXP allocateResult(size_t numNTrees, size_t numKs, size_t numPowers, size_t numBases, size_t numReps, bool dropUnusedDims);
  LossFunctorDefinition* createLossFunctorDefinition(SEXP lossTypeExpr, size_t numTestObservations, size_t numSamples);
}

extern "C" {
  using namespace dbarts;
  using namespace dbarts::xval;


  SEXP xbart(SEXP controlExpr, SEXP modelExpr, SEXP dataExpr,
             SEXP KExpr, SEXP numRepsExpr, SEXP numBurnInExpr, SEXP lossTypeExpr, SEXP numThreadsExpr,
             SEXP numTreesExpr, SEXP kExpr, SEXP powerExpr, SEXP baseExpr,
             SEXP dropExpr)
  {
    Control control;
    Model model;
    Data data;
    
    SEXP classExpr = Rf_getAttrib(controlExpr, R_ClassSymbol);
    if (std::strcmp(CHAR(STRING_ELT(classExpr, 0)), "dbartsControl") != 0) Rf_error("internal error: 'control' argument to dbarts_xbart not of class 'dbartsControl'");
    
    classExpr = Rf_getAttrib(modelExpr, R_ClassSymbol);
    if (std::strcmp(CHAR(STRING_ELT(classExpr, 0)), "dbartsModel") != 0) Rf_error("internal error: 'model' argument to dbarts_xbart not of class 'dbartsModel'");
    
    classExpr = Rf_getAttrib(dataExpr, R_ClassSymbol);
    if (std::strcmp(CHAR(STRING_ELT(classExpr, 0)), "dbartsData") != 0) Rf_error("internal error: 'data' argument to dbarts_xbart not of class 'dbartsData'");
    
    
    initializeControlFromExpression(control, controlExpr);
    initializeModelFromExpression(model, modelExpr, control);
    initializeDataFromExpression(data, dataExpr);
    
    
    if (data.numObservations == 0) Rf_error("xbart called on empty data set");
    
    rc_checkInts(numTreesExpr, "num trees", RC_LENGTH | RC_GEQ, asRXLen(1), RC_VALUE | RC_GT, 0, RC_END);
    rc_checkDoubles(kExpr, "k", RC_LENGTH | RC_GEQ, asRXLen(1), RC_VALUE | RC_GT, 0.0, RC_END);
    rc_checkDoubles(powerExpr, "power", RC_LENGTH | RC_GEQ, asRXLen(1), RC_VALUE | RC_GT, 0.0, RC_END);
    rc_checkDoubles(baseExpr, "power", RC_LENGTH | RC_GEQ, asRXLen(1), RC_VALUE | RC_GT, 0.0, RC_VALUE | RC_LT, 1.0, RC_END);
    rc_checkInts(numBurnInExpr, "num burn",  RC_LENGTH | RC_GEQ, asRXLen(1), RC_LENGTH | RC_LEQ, asRXLen(2), RC_VALUE | RC_GEQ, 0, RC_END);
    
    size_t numFolds = static_cast<size_t>(
      rc_getInt(KExpr, "num folds", RC_LENGTH | RC_EQ, asRXLen(1), RC_VALUE | RC_GT, 0, RC_VALUE | RC_LEQ, static_cast<int>(data.numObservations) - 1, RC_END));
    
    size_t numReps = static_cast<size_t>(
      rc_getInt(numRepsExpr, "num reps", RC_LENGTH | RC_GEQ, asRXLen(1), RC_VALUE | RC_GT, 0, RC_END));
    
    int numThreadsInt = rc_getInt(numThreadsExpr, "num threads", RC_LENGTH | RC_EQ, asRXLen(1), RC_VALUE | RC_GT, 0, RC_NA | RC_YES, RC_END);
    size_t numThreads = numThreadsInt != NA_INTEGER ? static_cast<size_t>(numThreadsInt) : 1;
    
    size_t numInitialBurnIn    = static_cast<size_t>(INTEGER(numBurnInExpr)[0]);
    size_t numSubsequentBurnIn = rc_getLength(numBurnInExpr) == 2 ? static_cast<size_t>(INTEGER(numBurnInExpr)[1]) : numInitialBurnIn / 5;
    
    bool dropUnusedDims = rc_getBool(dropExpr, "drop", RC_LENGTH | RC_EQ, asRXLen(1), RC_END);
    
    
    size_t numTestObservations     = data.numObservations / numFolds;
    // size_t numTrainingObservations = data.numObservations - numTestObservations;
    
    size_t numNTrees = rc_getLength(numTreesExpr);
    size_t numKs     = rc_getLength(kExpr);
    size_t numPowers = rc_getLength(powerExpr);
    size_t numBases  = rc_getLength(baseExpr);
    
    int* nTreesInt  = INTEGER(numTreesExpr);
    size_t* nTrees = ext_stackAllocate(numNTrees, size_t);
    for (size_t i = 0; i < numNTrees; ++i) nTrees[i] = static_cast<size_t>(nTreesInt[i]);
    
    double* k     = REAL(kExpr);
    double* power = REAL(powerExpr);
    double* base  = REAL(baseExpr);
    
    LossFunctorDefinition* lossFunctionDef = createLossFunctorDefinition(lossTypeExpr, numTestObservations, control.numSamples);

    
    SEXP result = PROTECT(allocateResult(numNTrees, numKs, numPowers, numBases, numReps, dropUnusedDims));
    
    GetRNGstate();
    
    crossvalidate(control, model, data,
                  numFolds, numReps, numInitialBurnIn, numSubsequentBurnIn,
                  *lossFunctionDef, numThreads,
                  nTrees, numNTrees, k, numKs, power, numPowers, base, numBases,
                  REAL(result));
    
    PutRNGstate();
    
    delete lossFunctionDef;
    
    ext_stackFree(nTrees);
    
    invalidateData(data);
    invalidateModel(model);
    invalidateControl(control);

    UNPROTECT(1);
    
    return result;
  }
}

namespace {
  using namespace dbarts;
  using namespace dbarts::xval;
  
  SEXP allocateResult(size_t numNTrees, size_t numKs, size_t numPowers, size_t numBases, size_t numReps, bool dropUnusedDims)
  {
    SEXP result = Rf_allocVector(REALSXP, numNTrees * numKs * numPowers * numBases * numReps);
    if (dropUnusedDims) {
      size_t numDims = 1 + (numNTrees > 1 ? 1 : 0) + (numKs > 1 ? 1 : 0) + (numPowers > 1 ? 1 : 0) + (numBases > 1 ? 1 : 0);
      if (numDims > 1) {
        SEXP dimsExpr = Rf_allocVector(INTSXP, numDims);
        int* dims = INTEGER(dimsExpr);
        numDims = 0;
        if (numNTrees > 1) dims[numDims++] = static_cast<int>(numNTrees);
        if (numKs > 1)     dims[numDims++] = static_cast<int>(numKs);
        if (numPowers > 1) dims[numDims++] = static_cast<int>(numPowers);
        if (numBases > 1)  dims[numDims++] = static_cast<int>(numBases);
        dims[numDims] = static_cast<int>(numReps);
        
        R_do_slot_assign(result, R_DimSymbol, dimsExpr);
      }
    } else {
      rc_setDims(result, static_cast<int>(numNTrees), static_cast<int>(numKs), static_cast<int>(numPowers),
                 static_cast<int>(numBases), static_cast<int>(numReps), -1);
    }
    return result;
  }
  
  struct MSELossFunctor : LossFunctor {
    double* scratch;
  };
  
  LossFunctor* createMSELoss(const LossFunctorDefinition& def, std::size_t numTestObservations, std::size_t numSamples)
  {
    (void) def; (void) numSamples;
    
    MSELossFunctor* result = new MSELossFunctor;
    result->scratch = new double[numTestObservations];
    return result;
  }
  
  void deleteMSELoss(LossFunctor* instance)
  {
    delete [] static_cast<MSELossFunctor*>(instance)->scratch;
    delete instance;
  }
  
  void calculateMSELoss(LossFunctor& restrict v_instance,
                        const double* restrict y_test, size_t numTestObservations, const double* restrict testSamples, size_t numSamples,
                        double* restrict results)
  {
    MSELossFunctor& restrict instance(*static_cast<MSELossFunctor* restrict>(&v_instance));
    
    double* restrict y_test_hat = instance.scratch;
    for (size_t i = 0; i < numTestObservations; ++i) {
      y_test_hat[i] = 0.0;
      for (size_t j = 0; j < numSamples; ++j) y_test_hat[i] += testSamples[i + j * numTestObservations];
      y_test_hat[i] /= static_cast<double>(numSamples);
    }
    
    results[0] = ext_computeSumOfSquaredResiduals(y_test, numTestObservations, y_test_hat) / static_cast<double>(numTestObservations);
  }
  
  struct MCRLossFunctor : LossFunctor {
    double* scratch;
  };
  
  LossFunctor* createMCRLoss(const LossFunctorDefinition& def, std::size_t numTestObservations, std::size_t numSamples)
  {
    (void) def; (void) numTestObservations;
    
    MCRLossFunctor* result = new MCRLossFunctor;
    result->scratch = new double[numSamples];
    return result;
  }
  
  void deleteMCRLoss(LossFunctor* instance)
  {
    delete [] static_cast<MCRLossFunctor*>(instance)->scratch;
    delete instance;
  }

  void calculateMCRLoss(LossFunctor& restrict v_instance,
                        const double* restrict y_test, size_t numTestObservations, const double* restrict testSamples, size_t numSamples,
                        double* restrict results)
  {
    MCRLossFunctor& restrict instance(*static_cast<MCRLossFunctor* restrict>(&v_instance));
    
    double* restrict probabilities = instance.scratch;
    size_t fp = 0, fn = 0;
    for (size_t i = 0; i < numTestObservations; ++i) {
      for (size_t j = 0; j < numSamples; ++j) {
        probabilities[j] = ext_cumulativeProbabilityOfNormal(testSamples[i + j * numTestObservations], 0.0, 1.0);
      }
      double y_test_hat = ext_computeMean(probabilities, numSamples) > 0.5 ? 1.0 : 0.0;
      
      if (y_test[i] != y_test_hat) {
        if (y_test[i] == 1.0) ++fn; else ++fp;
      }
    }
    results[0] = static_cast<double>(fp + fn) / static_cast<double>(numTestObservations);
  }
  
  struct CustomLossFunctorDefinition : LossFunctorDefinition {
    SEXP function;
    SEXP environment;
  };
  
  struct CustomLossFunctor : LossFunctor {
    double* y_test;
    double* testSamples;
    SEXP closure;
    SEXP environment;
  };
  
  LossFunctor* createCustomLoss(const LossFunctorDefinition& v_def, std::size_t numTestObservations, std::size_t numSamples)
  {
    const CustomLossFunctorDefinition& def(*static_cast<const CustomLossFunctorDefinition*>(&v_def));

    CustomLossFunctor* result = new CustomLossFunctor;
    
    SEXP y_testExpr      = PROTECT(Rf_allocVector(REALSXP, numTestObservations));
    SEXP testSamplesExpr = PROTECT(Rf_allocVector(REALSXP, numTestObservations * numSamples));
    rc_setDims(testSamplesExpr, static_cast<int>(numTestObservations), static_cast<int>(numSamples), -1);
    
    result->y_test      = REAL(y_testExpr);
    result->testSamples = REAL(testSamplesExpr);
    
    result->closure     = PROTECT(Rf_lang3(def.function, y_testExpr, testSamplesExpr));
    result->environment = def.environment;
    
    return result;
  }
    
  void deleteCustomLoss(LossFunctor* instance)
  {
    (void) instance;
    
    UNPROTECT(3);
  }
  
  void calculateCustomLoss(LossFunctor& restrict v_instance,
                           const double* restrict, size_t, const double* restrict, size_t,
                           double* restrict results)
  {
    CustomLossFunctor& restrict instance(*static_cast<CustomLossFunctor* restrict>(&v_instance));
    
    SEXP customResult = Rf_eval(instance.closure, instance.environment);
    std::memcpy(results, const_cast<const double*>(REAL(customResult)), rc_getLength(customResult) * sizeof(double));
  }

  
  const char* const lossTypeNames[] = { "mse", "mcr" };
  typedef enum { MSE, MCR, CUSTOM } LossFunctorType;
  
  LossFunctorDefinition* createLossFunctorDefinition(SEXP lossTypeExpr, size_t numTestObservations, size_t numSamples)
  {
    LossFunctorDefinition* result = NULL;
    
    if (Rf_isString(lossTypeExpr)) {
      if (rc_getLength(lossTypeExpr) != 1) Rf_error("length of lossType for strings must be 1");
      const char* lossTypeName = CHAR(STRING_ELT(lossTypeExpr, 0));
      
      size_t lossTypeNumber;
      int errorCode = ext_str_matchInArray(lossTypeName, lossTypeNames, static_cast<size_t>(CUSTOM - MSE), &lossTypeNumber);
      if (errorCode != 0) Rf_error("error matching string: %s", std::strerror(errorCode));
      if (lossTypeNumber == EXT_STR_NO_MATCH) Rf_error("unsupported result type: '%s'", lossTypeName);
      
      LossFunctorType type = static_cast<LossFunctorType>(lossTypeNumber);
      
      switch (type) {
        case MSE:
        result = new LossFunctorDefinition;
        result->y_testOffset = -1;
        result->testSamplesOffset = -1;
        result->numResults = 1;
        result->displayString = lossTypeNames[0];
        result->calculateLoss = &calculateMSELoss;
        result->createFunctor = &createMSELoss;
        result->deleteFunctor = &deleteMSELoss;
        break;
        
        case MCR:
        result = new LossFunctorDefinition;
        result->y_testOffset = -1;
        result->testSamplesOffset = -1;
        result->numResults = 1;
        result->displayString = lossTypeNames[1];
        result->calculateLoss = &calculateMCRLoss;
        result->createFunctor = &createMCRLoss;
        result->deleteFunctor = &deleteMCRLoss;
        break;
        case CUSTOM:
        Rf_error("internal error: invalid type enumeration");
      }
    } else if (Rf_isList(lossTypeExpr)) {
      if (rc_getLength(lossTypeExpr) != 2) Rf_error("length of lossType for functions must be 2");
     
      SEXP function    = VECTOR_ELT(lossTypeExpr, 0);
      SEXP environment = VECTOR_ELT(lossTypeExpr, 1);
      
      if (!Rf_isFunction(function)) Rf_error("first element of list for function lossType must be a closure");
      if (!Rf_isEnvironment(environment)) Rf_error("second element of list for function lossType must be a closure");
      
      CustomLossFunctorDefinition* c_result = new CustomLossFunctorDefinition;
      result = c_result;
      result->y_testOffset = 0;
      result->testSamplesOffset = sizeof(double*);
      
      SEXP tempY_test      = PROTECT(Rf_allocVector(REALSXP, numTestObservations));
      SEXP tempTestSamples = PROTECT(Rf_allocVector(REALSXP, numTestObservations * numSamples));
      rc_setDims(tempTestSamples, static_cast<int>(numTestObservations), static_cast<int>(numSamples), -1);
      ext_setVectorToConstant(REAL(tempY_test), numTestObservations, 0.0);
      ext_setVectorToConstant(REAL(tempTestSamples), numTestObservations * numSamples, 0.0);
      
      SEXP tempClosure = PROTECT(Rf_lang3(function, tempY_test, tempTestSamples));
      SEXP tempResult = Rf_eval(tempClosure, environment);
      result->numResults = rc_getLength(tempResult);
      
      result->displayString = "custom";
      result->calculateLoss = &calculateCustomLoss;
      result->createFunctor = &createCustomLoss;
      result->deleteFunctor = &deleteCustomLoss;
      
      c_result->function = function;
      c_result->environment = environment;
      
      UNPROTECT(3);
    } else {
      Rf_error("lossType must be a character string or list(closure, env)");
    }
    
    return result;
  }
}

