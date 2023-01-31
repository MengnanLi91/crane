//* This file is part of Crane, an open-source
//* application for plasma chemistry and thermochemistry
//* https://github.com/lcpp-org/crane
//*
//* Crane is powered by the MOOSE Framework
//* https://www.mooseframework.org
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#pragma once

#include "AuxScalarKernel.h"

class ReactionRateThreeBodyScalar : public AuxScalarKernel
{
public:
  ReactionRateThreeBodyScalar(const InputParameters & parameters);

  static InputParameters validParams();

protected:
  virtual Real computeValue();

  const VariableValue & _v;
  const VariableValue & _w;
  const VariableValue & _z;

  const VariableValue & _rate_coefficient;
  Real _stoichiometric_coeff;
};
