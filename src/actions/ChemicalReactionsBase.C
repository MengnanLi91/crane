//* This file is part of Crane, an open-source
//* application for plasma chemistry and thermochemistry
//* https://github.com/lcpp-org/crane
//*
//* Crane is powered by the MOOSE Framework
//* https://www.mooseframework.org
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#include "ChemicalReactionsBase.h"
#include "Parser.h"
#include "FEProblem.h"
#include "Factory.h"
#include "MooseEnum.h"
#include "AddVariableAction.h"
#include "Conversion.h"
#include "DirichletBC.h"
#include "ActionFactory.h"
#include "MooseObjectAction.h"
#include "MooseApp.h"

#include "libmesh/vector_value.h"

#include "pcrecpp.h"

#include <sstream>
#include <stdexcept>

// libmesh includes
#include "libmesh/libmesh.h"
#include "libmesh/exodusII_io.h"
#include "libmesh/equation_systems.h"
#include "libmesh/nonlinear_implicit_system.h"
#include "libmesh/explicit_system.h"
#include "libmesh/string_to_enum.h"
#include "libmesh/fe.h"

registerMooseAction("CraneApp", ChemicalReactionsBase, "add_variable");

InputParameters
ChemicalReactionsBase::validParams()
{
  MooseEnum families(AddVariableAction::getNonlinearVariableFamilies());
  MooseEnum orders(AddVariableAction::getNonlinearVariableOrders());

  InputParameters params = AddVariableAction::validParams();
  params.addParam<std::string>("name",
                               "The name of this reaction list. If multiple reaction blocks are "
                               "written, use this to supply a unique name to each one.");
  params.addParam<bool>(
      "use_bolsig",
      false,
      "Whether or not to use Bolsig+ (or bolos) to compute EEDF rate coefficients.");
  params.addRequiredParam<std::vector<NonlinearVariableName>>(
      "species", "List of (tracked) species included in reactions (both products and reactants)");
  params.addParam<std::vector<std::string>>(
      "aux_species", "Auxiliary species that are not included in nonlinear solve.");
  params.addParam<std::vector<Real>>("reaction_coefficient", "The reaction coefficients.");
  params.addParam<bool>(
      "include_electrons", false, "Whether or not electrons are being considered.");
  params.addParam<bool>(
      "use_log", false, "Whether or not to use logarithmic densities. (N = exp(n))");
  params.addParam<bool>(
      "track_rates", false, "Whether or not to track production rates for each reaction");
  params.addParam<std::string>("electron_density", "The variable used for density of electrons.");
  params.addParam<std::vector<NonlinearVariableName>>(
      "electron_energy", "Electron energy, used for energy-dependent reaction rates.");
  params.addParam<std::vector<NonlinearVariableName>>(
      "gas_energy", "Gas energy, used for energy-dependent reaction rates.");
  params.addParam<std::vector<std::string>>("gas_species",
                                            "All of the background gas species in the system.");
  params.addParam<std::vector<Real>>("gas_fraction", "The initial fraction of each gas species.");
  params.addRequiredParam<std::string>("reactions", "The list of reactions to be added");
  params.addParam<Real>("position_units", 1.0, "The units of position.");
  params.addParam<std::string>(
      "file_location",
      "",
      "The location of the reaction rate files. Default: empty string (current directory).");
  params.addParam<std::string>(
      "sampling_variable",
      "reduced_field",
      "Sample rate constants with E/N (reduced_field) or Te (electron_energy).");
  params.addParam<std::vector<std::string>>("equation_constants",
                                            "The constants included in the reaction equation(s).");
  params.addParam<std::vector<std::string>>(
      "equation_values", "The values of the constants included in the reaction equation(s).");
  params.addParam<std::vector<VariableName>>(
      "equation_variables", "Any nonlinear variables that appear in the equations.");
  params.addParam<std::vector<VariableName>>(
      "rate_provider_var", "The name of the variable used to sample from BOLOS/Bolsig+ files.");
  params.addParam<bool>("lumped_species",
                        false,
                        "If true, the input file parser will look for a parameter denoting lumped "
                        "species (NEUTRAL for now...eventually arbitrary?).");
  params.addParam<std::vector<std::string>>("lumped",
                                            "The neutral species that will be lumped together.");
  params.addParam<std::string>("lumped_name",
                               "The name of the variable that will account for multiple species.");
  params.addParam<bool>(
      "balance_check", false, "Whether or not to check that each reaction is balanced.");
  params.addParam<bool>("charge_balance_check",
                        false,
                        "Whether or not to check that each reaction is balanced by charge. If not, "
                        "equations with electrons are skipped in the balance check. "
                        "(Electron-impact reactions break particle conservation.)");
  params.addParam<std::vector<int>>(
      "num_particles",
      "A vector of values storing the number of particles in each species. Note that this vector "
      "MUST be the same length as 'species'. For any index i, num_particles[i] will be associated "
      "with _species[i].");
  params.addParam<bool>("use_ad",
                        false,
                        "Whether or not to use automatic differentiation. Recommended for systems "
                        "that use equation-based rate coefficients, mixture-averaged diffusion, or "
                        "large simulations in general.");
  params.addParam<bool>("convert_to_moles",
                        false,
                        "Multiplies all constant and parsed function rate coefficients by N_A "
                        "(6.022e23) to convert to a molar rate coefficient. (Note that EEDF rate "
                        "coefficient units are not affected. Those are up to the user to modify.");
  params.addParam<Real>("convert_to_meters",
                        1,
                        "Multiplies constant and parsed function rate coefficients by "
                        "convert_to_meters^(n*(n-1)), where `n` is the number of reactants.");
  params.addParam<std::string>("interpolation_type",
                               "spline",
                               "Type of interpolation to be used for tabulated rate coefficients. "
                               "Options: 'linear' or 'spline'. Default: 'spline'.");
  params.addClassDescription(
      "This Action automatically adds the necessary kernels and materials for a reaction network.");
  return params;
}

// Function that checks for file existence
inline bool
file_exists(const std::string & name)
{
  if (FILE * file = fopen(name.c_str(), "r"))
  {
    fclose(file);
    return true;
  }
  else
  {
    return false;
  }
}

ChemicalReactionsBase::ChemicalReactionsBase(const InputParameters & params)
  : Action(params),
    _species(getParam<std::vector<NonlinearVariableName>>("species")),
    _electron_energy(getParam<std::vector<NonlinearVariableName>>("electron_energy")),
    _gas_energy(getParam<std::vector<NonlinearVariableName>>("gas_energy")),
    _input_reactions(getParam<std::string>("reactions")),
    _r_units(getParam<Real>("position_units")),
    _sampling_variable(getParam<std::string>("sampling_variable")),
    _use_log(getParam<bool>("use_log")),
    _track_rates(getParam<bool>("track_rates")),
    _use_bolsig(getParam<bool>("use_bolsig")),
    _lumped_species(getParam<std::vector<std::string>>("lumped")),
    _use_ad(getParam<bool>("use_ad")),
    //_name(getParam<std::string>("name")),
    _mole_factor(getParam<bool>("convert_to_moles")),
    _rate_factor(getParam<Real>("convert_to_meters")),
    _interpolation_type(getParam<std::string>("interpolation_type"))
{
  // Check interpolation type
  if ((_interpolation_type != "spline") && (_interpolation_type != "linear"))
    mooseError("[Reactions] block: An interpolation_type of " + _interpolation_type +
               " is invalid! Only 'spline' or 'linear' interpolations are possible. 'spline' is "
               "used by default.");

  if (isParamValid("name"))
  {
    _name = getParam<std::string>("name") + "_";
  }
  else
    _name = "";
  // Multiplies rate constants (constant and parsed function based only!) by N_A to convert to mole
  // rates
  if (_mole_factor)
    N_A = 6.022e23;
  else
    N_A = 1.0;

  if (isParamValid("aux_species"))
    _aux_species = getParam<std::vector<std::string>>("aux_species");
  else
    _aux_species.push_back("none");

  if (getParam<bool>("lumped_species") && !isParamValid("lumped"))
    mooseError("The lumped_species parameter is set to true, but vector of neutrals (lumped = "
               "'...') is not set.");

  if (getParam<bool>("balance_check") && !isParamValid("num_particles"))
    mooseError("balance_check = true, but there is no num_particles parameter set! Please indicate "
               "the number of atoms present in each species. For example, molecular oxygen (O_2) "
               "has two particles. Ammonia (NH_3) has four particles (1 N, 3 H).");
  else if (getParam<bool>("balance_check"))
  {
    num_particles = getParam<std::vector<int>>("num_particles");
    if (num_particles.size() != _species.size())
      mooseError("The size of num_particles and species is not equal! Each species must have a "
                 "valid particle number in order to accurate check for particle balances.");
  }
  std::istringstream iss(_input_reactions);
  std::string token;
  std::string token2;
  std::vector<std::string> rate_coefficient_string;
  std::vector<std::string> threshold_energy_string;
  // std::vector<std::string> _rate_equation_string;

  size_t pos;
  size_t pos_start;
  size_t pos_end;
  size_t eq_start;
  size_t eq_end;
  size_t rxn_identifier_start;
  size_t rxn_identifier_end;

  int counter;
  counter = 0;
  //_eedf_reaction_counter = 0;
  //_num_eedf_reactions = 0;
  while (std::getline(iss >> std::ws,
                      token)) // splits by \n character (default) and ignores leading whitespace
  {
    // Skip commented lines
    // (Reactions with comments attached to the end will still be read.)
    if (token.find('#') == 0)
    {
      continue;
    }

    pos = token.find(':'); // Looks for colon, which separates reaction and rate coefficients

    // Brackets enclose the energy gain/loss (if applicable)
    pos_start = token.find('[');
    pos_end = token.find(']');

    // Curly braces enclose function-based constants
    eq_start = token.find('{');
    eq_end = token.find('}');

    // Parentheses enclose the reaction identifier (ionization, excitation, de-excitation, etc.)
    rxn_identifier_start = token.find('(');
    rxn_identifier_end = token.find(')');

    _reaction.push_back(token.substr(0, pos)); // Stores reactions

    /*
    if (rxn_identifier_start != std::string::npos)
    {
      rate_coefficient_string.push_back(token.substr(pos + 1, rxn_identifier_start - (pos + 1)));
    }
    else
    {
      rate_coefficient_string.push_back(token.substr(pos + 1, pos_start - (pos + 1)));
    }
    */
    if (rxn_identifier_start < pos_start)
      rate_coefficient_string.push_back(token.substr(pos + 1, rxn_identifier_start - (pos + 1)));
    else
      rate_coefficient_string.push_back(token.substr(pos + 1, pos_start - (pos + 1)));

    trim(_reaction[counter]);
    trim(rate_coefficient_string[counter]);

    if (pos_start != std::string::npos)
    {
      threshold_energy_string.push_back(token.substr(pos_start + 1, pos_end - pos_start - 1));
      _energy_change.push_back(true);
    }
    else
    {
      threshold_energy_string.push_back("\0");
      _energy_change.push_back(false);
    }

    if (eq_start != std::string::npos)
    {
      _rate_equation_string.push_back(token.substr(eq_start + 1, eq_end - eq_start - 1));
      _rate_equation.push_back(true);
    }
    else
    {
      _rate_equation_string.push_back("NONE");
      _rate_equation.push_back(false);
    }

    if (rxn_identifier_start != std::string::npos && !_rate_equation[counter])
    {
      _is_identified.push_back(true);
      _reaction_identifier.push_back(
          token.substr(rxn_identifier_start + 1, rxn_identifier_end - rxn_identifier_start - 1));
      //_eedf_reaction_number.push_back(_eedf_reaction_counter);
      //_eedf_reaction_counter += 1; // Counts the number of EEDF reactions (this is the only
      // instance
      // in which a reaction identifier is used)
      //_eedf_reaction_number.push_back(counter);
      //_num_eedf_reactions += 1;
    }
    else
    {
      _is_identified.push_back(false);
      _reaction_identifier.push_back("NONE");
      //_eedf_reaction_number.push_back(123456);
    }
    counter += 1;
  }
  _num_reactions = _reaction.size();
  _rate_coefficient.resize(_num_reactions, 0);
  _threshold_energy.resize(_num_reactions, 0);

  _elastic_collision.resize(_num_reactions, false);
  _rate_type.resize(_num_reactions);
  _aux_var_name.resize(_num_reactions);
  _reaction_coefficient_name.resize(_num_reactions);

  _num_eedf_reactions = 0;
  _num_function_reactions = 0;
  _num_constant_reactions = 0;
  for (unsigned int i = 0; i < _num_reactions; ++i)
  {

    if (threshold_energy_string[i] == "\0")
    {
      _threshold_energy[i] = 0.0;
    }
    else if (threshold_energy_string[i] == "elastic")
    {
      _threshold_energy[i] = 0.0;
      _elastic_collision[i] = true;
    }
    else
    {
      _threshold_energy[i] = std::stod(threshold_energy_string[i]);
    }
    _aux_var_name[i] =
        _name + "reaction_rate" + std::to_string(i); // Stores name of rate coefficients
    _reaction_coefficient_name[i] = "rate_constant" + std::to_string(i);
    if (rate_coefficient_string[i] == std::string("EEDF"))
    {
      _rate_coefficient[i] = NAN;
      _rate_type[i] = "EEDF";
      _eedf_reaction_number.push_back(i);
      _num_eedf_reactions += 1;
    }
    else if (_rate_equation[i] == true)
    {
      _rate_coefficient[i] = NAN;
      _rate_type[i] = "Equation";
      _function_reaction_number.push_back(i);
      _num_function_reactions += 1;
      // USE THIS CODE TO SEE IF VARIABLE IS CONTAINED WITHIN EQUATION
      //////////
      // if (_rate_equation_string[i].find("Tgas") != std::string::npos)
      // {
      //   mooseInfo("found!");
      // }
      //////////

      // Here we need to parse the individual reaction equations to find the
      // variables and constants.
      // std::istringstream iss(_rate_equation_string[i]);
      // std::string token;
      // while (std::getline(iss >> std::ws, token,'/'))
      // {
      //   mooseInfo(token);
      // }
    }
    else
    {
      try
      {
        _rate_coefficient[i] = std::stod(rate_coefficient_string[i]);
      }
      catch (const std::invalid_argument &)
      {
        mooseError("Rate coefficient '" + rate_coefficient_string[i] +
                   "' is invalid! "
                   "There are three rate coefficient types that are accepted:\n"
                   "  1. Constant (A + B -> C  : 10)\n"
                   "  2. Equation (A + B -> C  : {1e-4*exp(10)})\n"
                   "  3. EEDF     (A + B -> C  : EEDF)");
        throw;
      }
      catch (const std::out_of_range &)
      {
        mooseError("Argument out of range for a double\n");
        throw;
      }
      // _rate_coefficient[i] = std::stod(rate_coefficient_string[i]);
      _constant_reaction_number.push_back(i);
      _num_constant_reactions += 1;
      _rate_type[i] = "Constant";
    }
  }

  _reactants.resize(_num_reactions);
  _products.resize(_num_reactions);
  _reversible_reaction.resize(_num_reactions);
  _electron_index.resize(_num_reactions, 0);

  // lumped_variable is a vector of booleans that tells the Action whether an individual reaction
  // includes a lumped species. If so, additional initialization steps are taken.
  //_lumped_variable.resize(_num_reactions);
  //_lumped_species_index.resize(_num_reactions);

  // _species_electron.resize(_num_reactions, std::vector<bool>(_species.size()));

  /*
   * Split each reaction equation into reactants and products
   */

  // superelastic_reactions stores number of superelastic reactions, which will be added to
  // _num_reactions
  int superelastic_reactions = 0;
  // unsigned int lumped_count = 0;
  _reaction_lumped.resize(_num_reactions);

  for (unsigned int i = 0; i < _num_reactions; ++i)
  {
    std::istringstream iss2(_reaction[i]);
    std::string token;

    // Isolate individual terms of each reaction
    unsigned int counter = 0;
    bool react_check = true;
    while (std::getline(iss2 >> std::ws, token, ' '))
    {
      // Check for non-variable entries. Skip the plus signs, and if an equals
      // sign shows up we switch from reactants to products.
      // (This is a pretty hacky way to do this...but it works.)
      if (token == "+")
      {
        // If "+", skip to next iteration
        continue;
      }
      else if (token == "=" || token == "->" || token == "=>")
      {
        // If "=", switch from reactants to products, reset counter, and then
        // skip to next iteration.
        _reversible_reaction[i] = false;
        react_check = false;
        counter = 0;
        continue;
      }
      else if (token == "<=>" || token == "<->")
      {
        superelastic_reactions += 1;
        _reversible_reaction[i] = true;

        react_check = false;
        counter = 0;
        continue;
        // mooseError("Cannot handle superelastic reactions yet. Add two separate reactions.");
      }

      // Check if we are on the left side (reactants) or right side (products)
      // of the reaction equation.
      _all_participants.push_back(token);
      if (react_check)
      {
        _reactants[i].push_back(token);
      }
      else
      {
        _products[i].push_back(token);
      }
      counter = counter + 1;
    }
    // Here we check to see if the rate coefficients need to be modified in any way
    // (Options: convert to moles, convert to m^3/s or m^6/s)
    Real exp_factor;
    exp_factor = _reactants[i].size() - 1;
    if (_rate_type[i] == "Equation")
    {
      _rate_equation_string[i] += "*" + Moose::stringify(std::pow(N_A, exp_factor) *
                                                         std::pow(_rate_factor, (3 * exp_factor)));
    }
    else if (_rate_type[i] == "Constant")
    {
      _rate_coefficient[i] *= std::pow(N_A, exp_factor) * std::pow(_rate_factor, (3 * exp_factor));
    }

    // _reaction_lumped is used as a flag in the add_kernel (or add_scalar_kernel) stage. All lumped
    // reactions are flagged as true and the real reactions with the proper reactant terms are
    // appended to the end of the reaction list. Thus for any reaction `i`, if _reaction_lumped[i]
    // == true it will be skipped in the kernel addition stage.
    _reaction_lumped[i] = false;

    //_lumped_species_index[i] = -1;

    for (unsigned int k = 0; k < _reactants[i].size(); ++k)
    {
      if (getParam<bool>("lumped_species"))
      {
        if (_reactants[i][k] == getParam<std::string>("lumped_name"))
        {
          _reaction_lumped[i] = true;
          _lumped_reaction.push_back(i);
          continue;
          //_lumped_species_index[i] = k;
        }
      }
      if (_rate_type[i] == "EEDF" && _use_bolsig)
      {
        if (!isParamValid("electron_density"))
        {
          mooseError("EEDF reaction selected, but electron_density is not set! Please denote the "
                     "electron species.");
        }
        else
        {
          if (_reactants[i][k] != getParam<std::string>("electron_density"))
          {
            _reaction_species.push_back(_reactants[i][k]);
          }
        }
      }
    }

    _num_reactants.push_back(_reactants[i].size());
    _num_products.push_back(_products[i].size());
    _species_count.resize(_num_reactions, std::vector<Real>(_species.size()));
    for (unsigned int j = 0; j < _species.size(); ++j)
    {
      for (unsigned int k = 0; k < _reactants[i].size(); ++k)
      {
        if (_reactants[i][k] == _species[j])
        {
          _species_count[i][j] -= 1;
        }
        if (getParam<bool>("include_electrons") == true)
        {
          if (_reactants[i][k] == getParam<std::string>("electron_density"))
          {
            _electron_index[i] = k;
          }
        }
      }
      for (unsigned int k = 0; k < _products[i].size(); ++k)
      {
        if (_products[i][k] == _species[j])
        {
          _species_count[i][j] += 1;
        }
      }
    }
  }

  // Now we check for lumped species. For each reaction `i` involving a lumped species, this code
  // block will:
  //   1) Delete _reaction[i], _reactants[i], _products[i], while storing
  //   _reactants[i][..], _products[i][..], and _rate_coefficient[i] for later use
  //
  //   2) Add _lumped_species.size() to _reaction, _reactants, _products, and _rate_coefficients
  //
  //   3) Loop through the list of _lumped_species and add each reaction accordingly
  if (getParam<bool>("lumped_species"))
  {
    unsigned int old_num = _num_reactions;
    _num_reactions += (_lumped_reaction.size() * _lumped_species.size());
    //_reaction.resize(_num_reactions * _lumped_species.size());
    _reaction.resize(_num_reactions);
    _reactants.resize(_num_reactions);
    _products.resize(_num_reactions);
    _rate_coefficient.resize(_num_reactions);
    _rate_type.resize(_num_reactions);
    _rate_equation_string.resize(_num_reactions);
    _reaction_coefficient_name.resize(_num_reactions);
    _aux_var_name.resize(_num_reactions);
    _superelastic_reaction.resize(_num_reactions);
    _reversible_reaction.resize(_num_reactions);
    _reaction_lumped.resize(_num_reactions);
    _species_count.resize(_num_reactions);

    unsigned int lumped_counter;
    unsigned int lumped_index;
    // Loop through all of the lumped reactions
    // for (unsigned int i = 0; i < _lumped_reaction.size(); ++i)
    for (unsigned int i = 0; i < _lumped_reaction.size(); ++i)
    {

      lumped_counter = 0;
      while (lumped_counter < _lumped_species.size())
      {

        lumped_index = old_num + (i * _lumped_species.size()) + lumped_counter;
        _reaction[lumped_index] = _reaction[_lumped_reaction[i]];
        _rate_coefficient[lumped_index] = _rate_coefficient[_lumped_reaction[i]];
        _rate_type[lumped_index] = _rate_type[_lumped_reaction[i]];
        _rate_equation_string[lumped_index] = _rate_equation_string[_lumped_reaction[i]];
        _reaction_coefficient_name[lumped_index] = _reaction_coefficient_name[_lumped_reaction[i]];
        _aux_var_name[lumped_index] = "rate_constant" + std::to_string(lumped_index);
        _superelastic_reaction[lumped_index] = _superelastic_reaction[_lumped_reaction[i]];
        _reversible_reaction[lumped_index] = _reversible_reaction[_lumped_reaction[i]];
        _reaction_lumped[lumped_index] = false;
        _species_count[lumped_index] = _species_count[_lumped_reaction[i]];
        for (unsigned int k = 0; k < _reactants[_lumped_reaction[i]].size(); ++k)
        {
          if (_reactants[_lumped_reaction[i]][k] == getParam<std::string>("lumped_name"))
            _reactants[lumped_index].push_back(_lumped_species[lumped_counter]);
          else
            _reactants[lumped_index].push_back(_reactants[_lumped_reaction[i]][k]);
        }
        for (unsigned int k = 0; k < _products[_lumped_reaction[i]].size(); ++k)
        {
          if (_products[_lumped_reaction[i]][k] == getParam<std::string>("lumped_name"))
            _products[lumped_index].push_back(_lumped_species[lumped_counter]);
          else
            _products[lumped_index].push_back(_products[_lumped_reaction[i]][k]);
        }
        lumped_counter += 1;
      }
    }
  }

  std::string new_reaction;
  int new_index = _num_reactions - 1;

  // We also need to resize rate_coefficient and threshold_energy to account
  // for the new reaction(s)
  _superelastic_index.resize(_num_reactions + superelastic_reactions, 0);
  _superelastic_reaction.resize(_num_reactions + superelastic_reactions, false);
  _rate_coefficient.resize(_num_reactions + superelastic_reactions);
  _threshold_energy.resize(_num_reactions + superelastic_reactions);
  _rate_equation.resize(_num_reactions + superelastic_reactions);
  _species_count.resize(_num_reactions + superelastic_reactions,
                        std::vector<Real>(_species.size()));
  _reactants.resize(_reactants.size() + superelastic_reactions);
  _products.resize(_products.size() + superelastic_reactions);
  _aux_var_name.resize(_num_reactions + superelastic_reactions);
  _energy_change.resize(_num_reactions + superelastic_reactions);
  _reaction_coefficient_name.resize(_num_reactions + superelastic_reactions);
  if (superelastic_reactions > 0)
  {
    for (unsigned int i = 0; i < _num_reactions; ++i)
    {
      if (_reversible_reaction[i] == true)
      {
        // _reaction.resize(_num_reactions + 1);
        new_index += 1;
        // This index refers to the ORIGINAL reaction. Example:
        // If reaction #2 out of 5 (index = 1 from [0,4]) is reversible, then a
        // 6th reaction (index = 5) will be added. This is the superelastic reaction.
        // _superelastic_index is intended to refer back to the original reversible
        // reaction later in the code, so we can reverse any energy change and refer
        // to the forward reaction rate if necessary. Thus in this particular case,
        // _superelastic_index[5] = 1.
        _superelastic_index[new_index] = i;
        _superelastic_reaction[new_index] = true;
        _rate_coefficient[new_index] = NAN;
        _threshold_energy[new_index] = -_threshold_energy[i];
        _aux_var_name[new_index] = "rate_constant" + std::to_string(new_index);
        _reaction_coefficient_name[new_index] = "rate_constant" + std::to_string(new_index);
        if (_rate_equation[i] == true)
        {
          _rate_equation[new_index] = true;
        }
        else
        {
          _rate_equation[new_index] = false;
        }

        if (_energy_change[i])
          _energy_change[new_index] = true;
        else
          _energy_change[new_index] = false;

        // Here we reverse the products and reactants to build superelastic reactions.
        for (MooseIndex(_num_products[i]) j = 0; j < _num_products[i]; ++j)
        {
          new_reaction.append(_products[i][j]);
          _reactants[new_index].push_back(_products[i][j]);
          if (j == _num_products[i] - 1)
          {
            break;
          }
          else
          {
            new_reaction.append(" + ");
          }
        }
        new_reaction.append(" -> ");
        for (unsigned int j = 0; j < _reactants[i].size(); ++j)
        {
          new_reaction.append(_reactants[i][j]);
          _products[new_index].push_back(_reactants[i][j]);
          if (j == _reactants[i].size() - 1)
          {
            break;
          }
          else
          {
            new_reaction.append(" + ");
          }
        }
        _reaction.push_back(new_reaction);
      }

      // Calculate coefficients
      for (unsigned int j = 0; j < _species.size(); ++j)
      {
        for (unsigned int k = 0; k < _reactants[new_index].size(); ++k)
        {
          if (_reactants[new_index][k] == _species[j])
          {
            _species_count[new_index][j] -= 1;
          }
          if (getParam<bool>("include_electrons") == true)
          {
            if (_reactants[new_index][k] == getParam<std::string>("electron_density"))
            {
              _electron_index[new_index] = k;
            }
          }
        }
        for (unsigned int k = 0; k < _products[new_index].size(); ++k)
        {
          if (_products[new_index][k] == _species[j])
          {
            _species_count[new_index][j] += 1;
          }
        }
      }
    }
  }

  _num_reactions += superelastic_reactions;
  _reaction_coefficient_name.resize(_num_reactions);
  // Find the unique species across all reaction pathways
  // Note that this also accounts for species that are not tracked in case
  // some of the species are considered to be uniform background gases or
  // arbitrary source/sink terms.
  sort(_all_participants.begin(), _all_participants.end());
  std::vector<std::string>::iterator it;
  it = std::unique(_all_participants.begin(), _all_participants.end());
  _all_participants.resize(std::distance(_all_participants.begin(), it));

  // Find the stoichometric coefficient for each participant.
  _stoichiometric_coeff.resize(_reaction.size());

  for (unsigned int i = 0; i < _reaction.size(); ++i)
  {
    _stoichiometric_coeff[i].resize(_all_participants.size(), 0);

    for (unsigned int j = 0; j < _all_participants.size(); ++j)
    {
      for (unsigned int k = 0; k < _reactants[i].size(); ++k)
      {
        if (_reactants[i][k] == _all_participants[j])
        {
          _stoichiometric_coeff[i][j] -= 1;
        }
      }

      for (unsigned int k = 0; k < _products[i].size(); ++k)
      {
        if (_products[i][k] == _all_participants[j])
        {
          _stoichiometric_coeff[i][j] += 1;
        }
      }
    }
  }
  _reaction_participants.resize(_num_reactions);
  _reaction_stoichiometric_coeff.resize(_num_reactions);
  // Now we find which index of _all_participants is associated with _species
  // so they can be accurately referred to later if necessary.

  _species_index.resize(
      _species.size()); // Stores vector of indices relating _all_participants to _species.
  std::vector<std::string>::iterator iter;

  for (unsigned int i = 0; i < _species.size(); ++i)
  {
    iter = std::find(_all_participants.begin(), _all_participants.end(), _species[i]);
    _species_index[i] = std::distance(_all_participants.begin(), iter);
  }

  // Finally, we reduce _all_participants to find just the relevant participants
  // (and stoichiometric coefficients) for each individual reaction.

  for (unsigned int i = 0; i < _num_reactions; ++i)
  {
    std::vector<std::string> species_temp(
        _reactants[i]); // Copy reactants into new temporary vector
    species_temp.insert(species_temp.end(),
                        _products[i].begin(),
                        _products[i].end()); // Append products to new temp vector (so it now stores
                                             // all reactants and products)

    // Separate out the unique values from species_temp
    sort(species_temp.begin(), species_temp.end());
    std::vector<std::string>::iterator it;
    it = std::unique(species_temp.begin(), species_temp.end());
    species_temp.resize(std::distance(species_temp.begin(), it));
    // _reaction_participants[i].resize(species_temp.size());

    // Copy over the species from species_temp to the permanent _reaction_participants vector
    // Now each unique participant per reaction is stored in _reaction_participants.
    // Note that reaction_participants only stores the TRACKED species.
    for (unsigned int j = 0; j < species_temp.size(); ++j)
    {
      if (std::find(_species.begin(), _species.end(), species_temp[j]) != _species.end())
        _reaction_participants[i].push_back(species_temp[j]);
    }

    _reaction_stoichiometric_coeff[i].resize(_reaction_participants[i].size(), 0);

    for (unsigned int j = 0; j < _reaction_participants[i].size(); ++j)
    {
      for (unsigned int k = 0; k < _reactants[i].size(); ++k)
      {
        if (_reactants[i][k] == _reaction_participants[i][j])
        {
          _reaction_stoichiometric_coeff[i][j] -= 1;
        }
      }

      for (unsigned int k = 0; k < _products[i].size(); ++k)
      {
        if (_products[i][k] == _reaction_participants[i][j])
        {
          _reaction_stoichiometric_coeff[i][j] += 1;
        }
      }
    }

    if (_energy_change[i])
    {
      if (!isParamValid("electron_energy") && !isParamValid("gas_energy"))
        mooseError("Reactions have energy changes, but no electron or gas temperature variable is "
                   "included!");
    }
  }
  if (isParamValid("electron_energy"))
  {
    _electron_energy_term.push_back(true);
    _energy_variable.push_back(_electron_energy[0]);
  }
  if (isParamValid("gas_energy"))
  {
    _electron_energy_term.push_back(false);
    _energy_variable.push_back(_gas_energy[0]);
  }

  // With all reactions set up, now we check to make sure all equations are balanced. This requires
  // the parameter balance_check = true and a vector of values corresponding to the number of
  // particles associated with each species. TO DO: check charge balance as well
  if (getParam<bool>("balance_check"))
  {

    int index; // stores index of species in the reactant/product arrays
    // std::vector<std::string>::iterator iter;
    Real r_sum;
    Real p_sum;
    std::vector<std::string> faulty_reaction;
    bool unbalanced = false;
    // bool electron_reaction;

    // charge balance is not yet implemented
    // bool charge_balance = getParam<bool>("charge_balance_check");

    for (unsigned int i = 0; i < _num_reactions; ++i)
    {
      // electron_reaction = false;
      r_sum = 0;
      p_sum = 0;
      for (unsigned int j = 0; j < _reactants[i].size(); ++j)
      {
        auto iter = std::find(_species.begin(), _species.end(), _reactants[i][j]);
        index = std::distance(_species.begin(), iter);
        if (_species[index] == getParam<std::string>("electron_density"))
          continue;
        else
          r_sum += num_particles[index];
        // if (_species[index] == getParam<std::string>("electron_density"))
        //  electron_reaction = true;
      }
      for (unsigned int j = 0; j < _products[i].size(); ++j)
      {
        auto iter = std::find(_species.begin(), _species.end(), _products[i][j]);
        index = std::distance(_species.begin(), iter);
        if (_species[index] == getParam<std::string>("electron_density"))
          continue;
        else
          p_sum += num_particles[index];
        // if (_species[index] == getParam<std::string>("electron_density"))
        //  electron_reaction = true;
      }
      // if (r_sum != p_sum && !electron_reaction)
      if (r_sum != p_sum)
      {
        unbalanced = true;
        faulty_reaction.push_back(_reaction[i]);
      }
    }
    if (unbalanced)
    {
      std::string error_str;
      for (unsigned int i = 0; i < faulty_reaction.size(); ++i)
      {
        error_str.append("    ");
        error_str.append(faulty_reaction[i]);
        error_str.append("\n");
      }

      mooseError("The following equations are unbalanced:\n", error_str,
                 "Fix unbalanced reactions or particle conservation will not be enforced.");
    }
  }

  // Here we check to make sure that there are no aux_variables in the species list.
  // Only nonlinear variables should be included.
  for (unsigned int i = 0; i < _species.size(); ++i)
  {
    for (unsigned int j = 0; j < _aux_species.size(); ++j)
    {
      if (_species[i] == _aux_species[j])
        mooseError("Species " + Moose::stringify(_species[i]) +
                   " is included as both a species and aux_species!\nA species can only be either "
                   "a nonlinear variable or an auxiliary variable, not both. Note that any species "
                   "included as an aux_species will be treated as an auxiliary variable and will "
                   "not have any source or sink terms applied to it (though it will be included as "
                   "a reactant in the source/sink terms of other nonlinear variables.)");
    }
  }

  // Last we check for file names. This will be REQUIRED in future versions.
  // If a file name does not exist, an error is thrown.
  // txt, csv, and dat files are checked.
  for (unsigned int i = 0; i < _num_eedf_reactions; ++i)
  {
    if (_is_identified[i])
    {
      std::string fileloc = getParam<std::string>("file_location") + "/" + _reaction_identifier[i];
      if (file_exists(fileloc))
      {
        continue;
      }

      if (file_exists(fileloc + ".txt"))
      {
        _reaction_identifier[i] += ".txt";
        continue;
      }

      if (file_exists(fileloc + ".csv"))
      {
        _reaction_identifier[i] += ".csv";
        continue;
      }

      if (file_exists(fileloc + ".dat"))
      {
        _reaction_identifier[i] += ".dat";
        continue;
      }

      // If we've come this far, the file does not exist. Error time.
      mooseError("File " + fileloc +
                 " does not exist. \nMake sure the rate coefficient file exists and is spelled "
                 "correctly in the directory denoted by file_location.\nThe program "
                 "automatically checks for txt, csv, and dat files.\n(Note that if no "
                 "file_location parameter is added, the current directory is used.)");
    }
  }
}

void
ChemicalReactionsBase::act()
{
}
