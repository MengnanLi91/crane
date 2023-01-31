//* This file is part of Crane, an open-source
//* application for plasma chemistry and thermochemistry
//* https://github.com/lcpp-org/crane
//*
//* Crane is powered by the MOOSE Framework
//* https://www.mooseframework.org
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

//#ifndef ADDGENERALREACTIONS_H
//#define ADDGENERALREACTIONS_H
#pragma once

#include "AddVariableAction.h"
#include "Action.h"
#include "ChemicalReactionsBase.h"

class AddGeneralReactions : public ChemicalReactionsBase
{
public:
  AddGeneralReactions(const InputParameters & params);

  static InputParameters validParams();

  virtual void act();

protected:
  // virtual void addEEDFReactionKernel(const std::string & var_name);
  virtual void addEEDFReactionKernel(std::vector<VariableName> potential,
                                     std::string n_e,
                                     unsigned int var_num,
                                     unsigned int rxn_num,
                                     std::string kernel_name,
                                     std::vector<SubdomainName> block);
  //virtual void addReactionCoefficient(const std::string & var_name);
  std::string _coefficient_format;
};

//#endif // ADDZAPDOSREACTIONS_H
