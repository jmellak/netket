// Copyright 2018 The Simons Foundation, Inc. - All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NETKET_QUANTUMSTATERECONSTRUCTION_HPP_
#define NETKET_QUANTUMSTATERECONSTRUCTION_HPP_

#include <bitset>
#include <complex>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include "Machine/machine.hpp"
#include "Operator/abstract_operator.hpp"
#include "Optimizer/optimizer.hpp"
#include "Output/json_output_writer.hpp"
#include "Sampler/abstract_sampler.hpp"
#include "Stats/stats.hpp"
#include "Utils/parallel_utils.hpp"
#include "Utils/random_utils.hpp"

namespace netket {

class QuantumStateReconstruction {
  using VectorT = Eigen::Matrix<Complex, Eigen::Dynamic, 1>;
  using MatrixT = Eigen::Matrix<Complex, Eigen::Dynamic, Eigen::Dynamic>;

  AbstractSampler &sampler_;
  AbstractMachine &psi_;
  const AbstractHilbert &hilbert_;
  AbstractOptimizer &opt_;
  SR sr_;
  bool dosr_;

  std::vector<AbstractOperator *> rotations_;

  std::vector<std::vector<int>> connectors_;
  std::vector<std::vector<double>> newconfs_;
  std::vector<Complex> mel_;

  MatrixT Ok_;
  VectorT Okmean_;

  Eigen::MatrixXd vsamp_;
  Eigen::VectorXcd grad_;
  Eigen::VectorXcd rotated_grad_;

  // This optional will contain a value iff the MPI rank is 0.
  nonstd::optional<JsonOutputWriter> writer_;

  int totalnodes_;
  int mynode_;

  std::vector<AbstractOperator *> obs_;
  std::vector<std::string> obsnames_;
  ObsManager obsmanager_;

  int batchsize_;
  int batchsize_node_;
  int nsamples_;
  int nsamples_node_;
  int ninitsamples_;
  int ndiscardedsamples_;

  int npar_;

  // TODO check if this is OK or if the seed should be distributed
  DistributedRandomEngine engine_;

  std::vector<Eigen::VectorXd> trainingSamples_;
  std::vector<int> trainingBases_;

  // whether the wave-function is analytical or not
  bool is_holomorphic_;

 public:
  QuantumStateReconstruction(
      AbstractSampler &sampler, AbstractOptimizer &opt, int batchsize,
      int nsamples, std::vector<AbstractOperator *> rotations,
      std::vector<Eigen::VectorXd> trainingSamples,
      std::vector<int> trainingBases, int ndiscardedsamples = -1,
      int discarded_samples_on_init = 0, const std::string &method = "Sr",
      double diag_shift = 0.01, bool use_iterative = false,
      bool use_cholesky = true)
      : sampler_(sampler),
        psi_(sampler_.GetMachine()),
        hilbert_(psi_.GetHilbert()),
        opt_(opt),
        rotations_(rotations),
        trainingSamples_(trainingSamples),
        trainingBases_(trainingBases) {
    npar_ = psi_.Npar();
    is_holomorphic_ = psi_.IsHolomorphic();

    if (is_holomorphic_) {
      opt_.Init(Eigen::VectorXd(2 * npar_));
    } else {
      opt_.Init(Eigen::VectorXd(npar_));
    }

    grad_.resize(npar_);
    rotated_grad_.resize(npar_);

    Okmean_.resize(npar_);

    MPI_Comm_size(MPI_COMM_WORLD, &totalnodes_);
    MPI_Comm_rank(MPI_COMM_WORLD, &mynode_);

    batchsize_ = batchsize;
    batchsize_node_ = int(std::ceil(double(batchsize_) / double(totalnodes_)));

    nsamples_ = nsamples;
    nsamples_node_ = int(std::ceil(double(nsamples_) / double(totalnodes_)));

    ninitsamples_ = discarded_samples_on_init;

    if (ndiscardedsamples == -1) {
      ndiscardedsamples_ = 0.1 * nsamples_node_;
    } else {
      ndiscardedsamples_ = ndiscardedsamples;
    }

    if (method == "Gd") {
      dosr_ = false;
      InfoMessage() << "Using a gradient-descent based method" << std::endl;
    } else {
      setSrParameters(diag_shift, use_iterative, use_cholesky);
    }

    InfoMessage() << "Quantum state reconstruction running on " << totalnodes_
                  << " processes" << std::endl;

    MPI_Barrier(MPI_COMM_WORLD);
  }

  void InitOutput(std::string output_prefix, Index save_params_every) {
    if (mynode_ == 0) {
      writer_.emplace(output_prefix + ".log", output_prefix + ".wf",
                      save_params_every);
    }
  }

  void InitSweeps() {
    sampler_.Reset();

    for (int i = 0; i < ninitsamples_; i++) {
      sampler_.Sweep();
    }
  }

  void AddObservable(AbstractOperator &ob, const std::string &obname) {
    obs_.push_back(&ob);
    obsnames_.push_back(obname);
  }

  void Sample() {
    sampler_.Reset();

    for (int i = 0; i < ndiscardedsamples_; i++) {
      sampler_.Sweep();
    }

    vsamp_.resize(nsamples_node_, psi_.Nvisible());

    for (int i = 0; i < nsamples_node_; i++) {
      sampler_.Sweep();
      vsamp_.row(i) = sampler_.Visible();
    }
  }

  void Gradient(std::vector<Eigen::VectorXd> &batchSamples,
                std::vector<int> &batchBases) {
    Eigen::VectorXcd der(psi_.Npar());

    // Positive phase driven by data
    const int ndata = batchsize_node_;
    Ok_.resize(ndata, psi_.Npar());
    for (int i = 0; i < ndata; i++) {
      RotateGradient(batchBases[i], batchSamples[i], der);
      Ok_.row(i) = der.conjugate();
    }
    grad_ = -2.0 * (Ok_.colwise().mean());

    // Negative phase driven by the machine
    Sample();
    ComputeObservables();

    const int nsamp = vsamp_.rows();
    Ok_.resize(nsamp, psi_.Npar());

    for (int i = 0; i < nsamp; i++) {
      Ok_.row(i) = psi_.DerLog(vsamp_.row(i)).conjugate();
    }
    grad_ += 2.0 * (Ok_.colwise().mean());

    // Summing the gradient over the nodes
    SumOnNodes(grad_);
    grad_ /= double(totalnodes_);
  }

  void ComputeObservables() {
    const Index nsamp = vsamp_.rows();
    for (const auto &obname : obsnames_) {
      obsmanager_.Reset(obname);
    }
    for (Index i_samp = 0; i_samp < nsamp; ++i_samp) {
      for (std::size_t i_obs = 0; i_obs < obs_.size(); ++i_obs) {
        const auto &op = obs_[i_obs];
        const auto &name = obsnames_[i_obs];
        obsmanager_.Push(name, ObsLocValue(*op, vsamp_.row(i_samp)).real());
      }
    }
  }

  Complex ObsLocValue(const AbstractOperator &ob, const Eigen::VectorXd &v) {
    ob.FindConn(v, mel_, connectors_, newconfs_);

    assert(connectors_.size() == mel_.size());

    auto logvaldiffs = (psi_.LogValDiff(v, connectors_, newconfs_));

    assert(mel_.size() == std::size_t(logvaldiffs.size()));

    Complex obval = 0;

    for (int i = 0; i < logvaldiffs.size(); i++) {
      obval += mel_[i] * std::exp(logvaldiffs(i));
    }

    return obval;
  }

  void Run(const std::string &output_prefix, Index n_iter, Index step_size = 1,
           Index save_params_every = 50) {
    assert(n_iter > 0);
    assert(step_size > 0);
    assert(save_params_every > 0);

    std::vector<Eigen::VectorXd> batchSamples;
    std::vector<int> batchBases;
    opt_.Reset();
    InitOutput(output_prefix, save_params_every);

    InitSweeps();
    std::uniform_int_distribution<int> distribution(
        0, trainingSamples_.size() - 1);

    for (Index i = 0; i < n_iter; i += step_size) {
      int index;
      batchSamples.resize(batchsize_node_);
      batchBases.resize(batchsize_node_);

      // Randomly select a batch of training data
      for (int k = 0; k < batchsize_node_; k++) {
        index = distribution(engine_.Get());
        batchSamples[k] = trainingSamples_[index];
        batchBases[k] = trainingBases_[index];
      }
      Gradient(batchSamples, batchBases);
      UpdateParameters();
      PrintOutput(i);
    }
  }

  void UpdateParameters() {
    auto pars = psi_.GetParameters();

    Eigen::VectorXcd deltap(npar_);

    if (dosr_) {
      sr_.ComputeUpdate(Ok_, grad_, deltap, is_holomorphic_);
    } else {
      deltap = grad_;
    }

    if (is_holomorphic_) {
      Eigen::VectorXd deltap_real(2 * npar_);
      deltap_real << deltap.real(), deltap.imag();
      Eigen::VectorXd parst(2 * npar_);
      parst << pars.real(), pars.imag();
      opt_.Update(deltap_real, parst);
      pars.real() = parst.head(npar_);
      pars.imag() = parst.tail(npar_);
    } else {
      Eigen::VectorXd deltap_real = deltap.real();
      Eigen::VectorXd parst = pars.real();
      opt_.Update(deltap, parst);
      pars.real() = parst;
    }

    SendToAll(pars);

    psi_.SetParameters(pars);

    MPI_Barrier(MPI_COMM_WORLD);
  }

  // Evaluate the gradient for a given visible state, in the
  // basis identified by b_index
  void RotateGradient(int b_index, const Eigen::VectorXd &state,
                      Eigen::VectorXcd &rotated_gradient) {
    Complex den;
    Eigen::VectorXcd num;
    Eigen::VectorXd v(psi_.Nvisible());
    rotations_[b_index]->FindConn(state, mel_, connectors_, newconfs_);
    assert(connectors_.size() == mel_.size());

    const std::size_t nconn = connectors_.size();

    auto logvaldiffs = (psi_.LogValDiff(state, connectors_, newconfs_));
    den = 0.0;
    num.setZero(psi_.Npar());
    for (std::size_t k = 0; k < nconn; k++) {
      v = state;
      for (std::size_t j = 0; j < connectors_[k].size(); j++) {
        v(connectors_[k][j]) = newconfs_[k][j];
      }
      num += mel_[k] * std::exp(logvaldiffs(k)) * psi_.DerLog(v);
      den += mel_[k] * std::exp(logvaldiffs(k));
    }
    rotated_gradient = (num / den);
  }

  void PrintOutput(Index i) {
    // Note: This has to be called in all MPI processes, because converting
    // the ObsManager to JSON performs a MPI reduction.
    auto obs_data = json(obsmanager_);
    obs_data["Acceptance"] = sampler_.Acceptance();

    if (writer_.has_value()) {  // writer_.has_value() iff the MPI rank is 0, so
                                // the output is only written once
      writer_->WriteLog(i, obs_data);
      writer_->WriteState(i, psi_);
    }
    MPI_Barrier(MPI_COMM_WORLD);
  }

  void setSrParameters(double diag_shift = 0.01, bool use_iterative = false,
                       bool use_cholesky = true) {
    dosr_ = true;
    sr_.setParameters(diag_shift, use_iterative, use_cholesky);
  }
};

}  // namespace netket

#endif  // NETKET_UNSUPERVISED_HPP
