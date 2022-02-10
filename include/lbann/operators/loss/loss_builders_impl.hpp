////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2022, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
// Written by the LBANN Research Team (B. Van Essen, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-697807.
// All rights reserved.
//
// This file is part of LBANN: Livermore Big Artificial Neural Network
// Toolkit. For details, see http://software.llnl.gov/LBANN or
// https://github.com/LLNL/LBANN.
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
////////////////////////////////////////////////////////////////////////////////
#ifndef LBANN_INCLUDE_LBANN_OPERATORS_LOSS_LOSS_BUILDERS_IMPL_HPP_INCLUDED
#define LBANN_INCLUDE_LBANN_OPERATORS_LOSS_LOSS_BUILDERS_IMPL_HPP_INCLUDED

#include "lbann/operators/loss/entrywise.hpp"
#include "lbann/operators/loss/loss_builders.hpp"

LBANN_DEFINE_OPERATOR_BUILDER(binary_cross_entropy, BinaryCrossEntropy);
LBANN_DEFINE_OPERATOR_BUILDER(boolean_accuracy, BooleanAccuracy);
LBANN_DEFINE_OPERATOR_BUILDER(boolean_false_negative, BooleanFalseNegative);
LBANN_DEFINE_OPERATOR_BUILDER(boolean_false_positive, BooleanFalsePositive);
LBANN_DEFINE_OPERATOR_BUILDER(sigmoid_binary_cross_entropy,
                              SigmoidBinaryCrossEntropy);

#endif // LBANN_INCLUDE_LBANN_OPERATORS_LOSS_LOSS_BUILDERS_IMPL_HPP_INCLUDED