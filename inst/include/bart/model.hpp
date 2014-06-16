#ifndef BART_MODEL_HPP
#define BART_MODEL_HPP

#include <cstddef>
#include "cstdint"

// can make these kinds of adjustments to trees during MCMC
#define BART_BIRTH_OR_DEATH_PROBABILITY 0.5
#define BART_SWAP_PROBABILITY           0.1
#define BART_CHANGE_PROBABILITY         0.4
// conditional on being inside a birthOrDeath step
#define BART_BIRTH_PROBABILITY          0.5

#define BART_DEFAULT_NORMAL_PRIOR_K       2.0
#define BART_DEFAULT_CHISQ_PRIOR_DF       3.0
#define BART_DEFAULT_CHISQ_PRIOR_QUANTILE 0.9

#define BART_DEFAULT_TREE_PRIOR_POWER 2.0
#define BART_DEFAULT_TREE_PRIOR_BASE  0.95


namespace bart {
  struct TreePrior;
  struct EndNodePrior;
  struct ResidualVariancePrior;
  
  struct Model {
    double birthOrDeathProbability;
    double swapProbability;
    double changeProbability;

    double birthProbability;
    
    TreePrior* treePrior;
    EndNodePrior* muPrior;
    ResidualVariancePrior* sigmaSqPrior;
    
    Model() : 
      birthOrDeathProbability(BART_BIRTH_OR_DEATH_PROBABILITY),
      swapProbability(BART_SWAP_PROBABILITY),
      changeProbability(BART_CHANGE_PROBABILITY),
      birthProbability(BART_BIRTH_PROBABILITY), treePrior(NULL), muPrior(NULL), sigmaSqPrior(NULL)
    {
    }
  };
  
  struct BARTFit;
  struct Control;
  struct Node;
  struct Tree;
  struct Rule;
  
  struct TreePrior {
    virtual double computeGrowthProbability(const BARTFit& fit, const Node& node) const = 0;
    virtual double computeTreeLogProbability(const BARTFit& fit, const Tree& tree) const = 0;
    
    virtual double computeSplitVariableLogProbability(const BARTFit& fit, const Node& node) const = 0;
    virtual double computeRuleForVariableLogProbability(const BARTFit& fit, const Node& node) const = 0;
    

    virtual Rule drawRuleAndVariable(const BARTFit& fit, const Node& node, bool* exhaustedLeftSplits, bool* exhaustedRightSplits) const = 0;
    virtual std::int32_t drawSplitVariable(const BARTFit& fit, const Node& node) const = 0;
    virtual Rule drawRuleForVariable(const BARTFit& fit, const Node& node, std::int32_t variableIndex, bool* exhaustedLeftSplits, bool* exhaustedRightSplits) const = 0;
    
    virtual ~TreePrior() { }
  };
  
  struct EndNodePrior {
    virtual double computeLogIntegratedLikelihood(const BARTFit& fit, const Node& node, const double* y, double residualVariance) const = 0;
    virtual double drawFromPosterior(double ybar, std::size_t numObservations, double residualVariance) const = 0;
    
    virtual ~EndNodePrior() { }
  };
  
  // the virtual scale accessors are for the conditional bart, which can have its data rescaled
  // if your prior doesn't use them, ignore them
  struct ResidualVariancePrior {
    virtual double drawFromPosterior(std::size_t numObservations, double sumOfSquaredResiduals) const = 0;
    
    virtual double getScale() const = 0;
    virtual void setScale(double scale) = 0;
    
    virtual ~ResidualVariancePrior() { }
  };
  
  // for lack of a better name, calling it the Chipman, George, and McCullough prior
  // Pr(node splits) = base / (1 + depth)^power
  
  struct CGMPrior : TreePrior {
    double base;
    double power;
    
    CGMPrior() { }
    CGMPrior(double base, double power) : base(base), power(power) { }
    virtual ~CGMPrior() { }
    
    virtual double computeGrowthProbability(const BARTFit& fit, const Node& node) const;
    virtual double computeTreeLogProbability(const BARTFit& fit, const Tree& tree) const;
    
    virtual double computeSplitVariableLogProbability(const BARTFit& fit, const Node& node) const;
    virtual double computeRuleForVariableLogProbability(const BARTFit& fit, const Node& node) const;
    
    virtual Rule drawRuleAndVariable(const BARTFit& fit, const Node& node, bool* exhaustedLeftSplits, bool* exhaustedRightSplits) const;
    virtual std::int32_t drawSplitVariable(const BARTFit& fit, const Node& node) const;
    virtual Rule drawRuleForVariable(const BARTFit& fit, const Node& node, std::int32_t variableIndex, bool* exhaustedLeftSplits, bool* exhaustedRightSplits) const;
  };
  
  // nodeMu ~ normal(0, 1 / precision)
  struct NormalPrior : EndNodePrior {
    double precision;
    
    NormalPrior() : precision(1.0) { }
    NormalPrior(const Control& control, double k);
    virtual ~NormalPrior() { }
    
    virtual double drawFromPosterior(double ybar, std::size_t numObservations, double residualVariance) const;
    virtual double computeLogIntegratedLikelihood(const BARTFit& fit, const Node& node, const double* y, double residualVariance) const;
  };
  
  // sigmaSq ~ chisq(df, scale)
  struct ChiSquaredPrior : ResidualVariancePrior {
    double degreesOfFreedom;
    double scale;
    
    
    ChiSquaredPrior() :
      degreesOfFreedom(BART_DEFAULT_CHISQ_PRIOR_DF),
      scale(1.0) { }
    ChiSquaredPrior(double degreesOfFreedom, double quantile);
    virtual ~ChiSquaredPrior() { }
    
    virtual double getScale() const { return scale; }
    virtual void setScale(double scale) { this->scale = scale; }
    
    virtual double drawFromPosterior(std::size_t numObservations, double sumOfSquaredResiduals) const;
  };
} // namespace bart

#endif // BART_MODEL_HPP
