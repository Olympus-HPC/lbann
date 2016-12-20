////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2016, Lawrence Livermore National Security, LLC. 
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
//
// lbann_layer .hpp .cpp - Parent class for all layer types
////////////////////////////////////////////////////////////////////////////////

#include "lbann/layers/lbann_layer.hpp"
#include "lbann/regularization/lbann_regularizer.hpp"
#include "lbann/utils/lbann_timer.hpp"
#include "lbann/models/lbann_model.hpp"
#include "lbann/io/lbann_file_io.hpp"
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace std;
using namespace El;

lbann::Layer::Layer(const uint index, lbann_comm* comm, Optimizer *optimizer,
                    uint mbsize, activation_type activation,
                    std::vector<regularizer*> regs)
  : m_activation_type(activation), optimizer(optimizer), comm(comm),
    regularizers(regs), m_mini_batch_size(mbsize),
    m_effective_mbsize(mbsize),
    fp_time(0.0), bp_time(0.0)
{
    Index = index;
    m_execution_mode = execution_mode::training;
    fp_input = NULL;
    bp_input = NULL;
    neural_network_model = NULL;

    // Most layers use standard elemental matrix distribution
    m_weights = new DistMat(comm->get_model_grid());
    m_weights_gradient = new DistMat(comm->get_model_grid());
    m_weighted_sum = new DistMat(comm->get_model_grid());
    m_prev_error_signal = new DistMat(comm->get_model_grid());
    m_error_signal = new DistMat(comm->get_model_grid());
    m_activations = new DistMat(comm->get_model_grid());
    m_prev_activations = new DistMat(comm->get_model_grid());

    /// Instantiate these view objects but do not allocate data for them
    m_weighted_sum_v = new DistMat(comm->get_model_grid());
    m_prev_error_signal_v = new DistMat(comm->get_model_grid());
    m_error_signal_v = new DistMat(comm->get_model_grid());
    m_activations_v = new DistMat(comm->get_model_grid());
    m_prev_activations_v = new DistMat(comm->get_model_grid());

    // Initialize activation function
    m_activation_fn = new_activation(activation);

}

lbann::Layer::~Layer() {
  delete m_activation_fn;
  delete m_weights;
  delete m_weights_gradient;
  delete m_weighted_sum;
  delete m_prev_error_signal;
  delete m_error_signal;
  delete m_activations;
  delete m_prev_activations;
  delete m_weighted_sum_v;
  delete m_prev_error_signal_v;
  delete m_error_signal_v;
  delete m_activations_v;
  delete m_prev_activations_v;
}

DataType lbann::Layer::forwardProp(DataType prev_WBL2NormSum) {
  double fp_start = get_time();

  // Get incoming activations and convert matrix distribution if necessary
  // Note that on assignment Elemental handles distribution conversion so a DistMatrixReadProxy is unnecessary
  if(fp_input != NULL) { // Input layers will not have a valid fp_input
    *m_prev_activations = *fp_input;
  }
  // Get incoming loss and convert matrix distribution if necessary
  // Note that on assignment Elemental handles distribution conversion so a DistMatrixReadProxy is unnecessary
  if(bp_input != NULL) { // Target layers will not have a valid bp_input
    *m_prev_error_signal = *bp_input;
  }
  // Set the view for all of the standard matrices based on the
  // current mini-batch size
  fp_set_std_matrix_view();
  // Apply connection regularization. (e.g. DropConnect).
  for (regularizer* reg : regularizers) reg->fp_connections();
  // Layer layer's linearity.
  fp_linearity();
  // Apply weight regularization (e.g. L2 normalization).
  for (regularizer* reg : regularizers) reg->fp_weights();
  // Apply activation function/nonlinearity.
  fp_nonlinearity();
  // Apply activation regularization (e.g. Dropout).
  for (regularizer* reg : regularizers) reg->fp_activations();
  fp_time += get_time() - fp_start;
  return prev_WBL2NormSum;
}

void lbann::Layer::backProp() {
  double bp_start = get_time();

  // Get incoming loss and convert matrix distribution if necessary
  // Note that on assignment Elemental handles distribution conversion so a DistMatrixReadProxy is unnecessary
  if(bp_input != NULL) { // Target layers will not have a valid bp_input
    *m_prev_error_signal = *bp_input;
  }
  // Set the view for all of the standard matrices based on the
  // current mini-batch size
  //  bp_set_std_matrix_view();
  // Backprop activation regularization.
  for (regularizer* reg : regularizers) reg->bp_activations();
  // Backprop the activation function/nonlinearity.
  bp_nonlinearity();
  // Backprop weight regularization.
  for (regularizer* reg : regularizers) reg->bp_weights();
  // Backprop the layer's linearity.
  bp_linearity();
  // Backprop connection regularization.
  for (regularizer* reg : regularizers) reg->bp_connections();
  bp_time += get_time() - bp_start;
}

void lbann::Layer::summarize(lbann_summary& summarizer, int64_t step) {
  std::string prefix = "layer" + std::to_string(static_cast<long long>(Index)) + "/weights/";
  // TODO: implement summarizer functions for other matrix distributions
  const ElMat& wb = get_weights_biases();
  summarizer.reduce_mean(prefix + "mean", wb, step);
  summarizer.reduce_min(prefix + "min", wb, step);
  summarizer.reduce_max(prefix + "max", wb, step);
  summarizer.reduce_stdev(prefix + "stdev", wb, step);
  prefix = "layer" + std::to_string(static_cast<long long>(Index)) + "/weights_gradient/";
  const ElMat& wb_d = get_weights_biases_gradient();
  summarizer.reduce_mean(prefix + "mean", wb_d, step);
  summarizer.reduce_min(prefix + "min", wb_d, step);
  summarizer.reduce_max(prefix + "max", wb_d, step);
  summarizer.reduce_stdev(prefix + "stdev", wb_d, step);
  prefix = "layer" + std::to_string(static_cast<long long>(Index)) + "/activations/";
  const ElMat& acts = get_activations();
  summarizer.reduce_mean(prefix + "mean", acts, step);
  summarizer.reduce_min(prefix + "min", acts, step);
  summarizer.reduce_max(prefix + "max", acts, step);
  summarizer.reduce_stdev(prefix + "stdev", acts, step);
  prefix = "layer" + std::to_string(static_cast<long long>(Index)) + "/";
  summarizer.reduce_scalar(prefix + "fp_time", fp_time, step);
  summarizer.reduce_scalar(prefix + "bp_time", bp_time, step);
  reset_counters();
}

void lbann::Layer::setup(int) {
  for (regularizer* reg : regularizers) reg->setup(this);
}

ElMat *lbann::Layer::fp_output() {
  return m_activations;
}

ElMat *lbann::Layer::bp_output() {
  return m_error_signal;
}

void lbann::Layer::setup_fp_input(ElMat *fp_input)
{
  this->fp_input = fp_input;
  // cout << "Layer " << Index << " is looking at fp_input " << fp_input << endl;
}

void lbann::Layer::setup_bp_input(ElMat *bp_input)
{
  this->bp_input = bp_input;
  // cout << "Layer " << Index << " is looking at bp_input " << bp_input << endl;
}

/* TODO: user buffered I/O for more efficient writes */

/** Stores meta data needed to reconstruct matrix in memory after reading
 *  it back from a file */
struct layer_header {
    uint64_t rank;       /**< rank of MPI process that wrote the file */
    uint64_t width;      /**< global width of matrix */
    uint64_t height;     /**< global height of matrix */
    uint64_t localwidth; /**< local width of matrix on current process */
    uint64_t localheight;/**< local height of matrix on current process */
    uint64_t ldim;       /**< specifies padding of first dimension in local storage */
};

/** \brief Given an open file descriptor, file name, and a matrix, write the matrix
 *         to the file descriptor, return the number of bytes written */
static bool writeDist(int fd, const char* filename, const DistMat& M, uint64_t* bytes)
{
    // TODO: store in network order

    // build our header
    struct layer_header header;
    header.rank        = (uint64_t) M.Grid().Rank();
    header.width       = (uint64_t) M.Width();
    header.height      = (uint64_t) M.Height();
    header.localwidth  = (uint64_t) M.LocalWidth();
    header.localheight = (uint64_t) M.LocalHeight();
    header.ldim        = (uint64_t) M.LDim();

    // write the header to the file
    ssize_t write_rc = write(fd, &header, sizeof(header));
    if (write_rc != sizeof(header)) {
        // error!
    }
    *bytes += write_rc;

    // now write the data for our part of the distributed matrix
    const Int localHeight = M.LocalHeight();
    const Int localWidth = M.LocalWidth();
    const Int lDim = M.LDim();
    if(localHeight == lDim) {
        // the local dimension in memory matches the local height,
        // so we can write our data in a single shot
        void* buf = (void*) M.LockedBuffer();
        size_t bufsize = localHeight * localWidth * sizeof(DataType);
        write_rc = write(fd, buf, bufsize);
        if (write_rc != bufsize) {
            // error!
        }
        *bytes += write_rc;
    } else {
        // TODO: if this padding is small, may not be a big deal to write it out anyway
        // we've got some padding along the first dimension
        // while storing the matrix in memory, avoid writing the padding
        for(Int j = 0; j < localWidth; ++j) {
            void* buf = (void*) M.LockedBuffer(0, j);
            size_t bufsize = localHeight * sizeof(DataType);
            write_rc = write(fd, buf, bufsize);
            if (write_rc != bufsize) {
                // error!
            }
            *bytes += write_rc;
        }
    }

    return true;
}

/** \brief Given an open file descriptor, file name, and a matrix, read the matrix
 *         from the file descriptor, return the number of bytes read */
static bool readDist(int fd, const char* filename, DistMat& M, uint64_t* bytes)
{
    // read in the header
    struct layer_header header;
    ssize_t read_rc = read(fd, &header, sizeof(header));
    if (read_rc != sizeof(header)) {
        // error!
    }
    *bytes += read_rc;

    // resize our global matrix
    Int height = header.height;
    Int width  = header.width;
    M.Resize(height, width);

    // TODO: check that header values match up

    if(M.ColStride() == 1 && M.RowStride() == 1) {
        if(M.Height() == M.LDim()) {
            void* buf = (void*) M.Buffer();
            size_t bufsize = height * width * sizeof(DataType);
            read_rc = read(fd, buf, bufsize);
            if (read_rc != bufsize) {
                // error!
            }
            *bytes += read_rc;
        } else {
            for(Int j = 0; j < width; ++j) {
                void* buf = (void*) M.Buffer(0, j);
                size_t bufsize = height * sizeof(DataType);
                read_rc = read(fd, buf, bufsize);
                if (read_rc != bufsize) {
                    // error!
                }
                *bytes += read_rc;
            }
        }
    } else {
        const Int localHeight = M.LocalHeight();
        const Int localWidth = M.LocalWidth();
        const Int lDim = M.LDim();
        if(localHeight == lDim) {
            void* buf = (void*) M.Buffer();
            size_t bufsize = localHeight * localWidth * sizeof(DataType);
            read_rc = read(fd, buf, bufsize);
            if (read_rc != bufsize) {
                // error!
            }
            *bytes += read_rc;
        } else {
            for(Int jLoc = 0; jLoc < localWidth; ++jLoc) {
                void* buf = (void*) M.Buffer(0, jLoc);
                size_t bufsize = localHeight * sizeof(DataType);
                read_rc = read(fd, buf, bufsize);
                if (read_rc != bufsize) {
                    // error!
                }
                *bytes += read_rc;
            }
        }
    }
    return true;
}

/** \brief Given a distributed matrix, compute the MPI datatypes needed for MPI I/O */
static void create_types(const DistMatrix<DataType> &M, MPI_Datatype* mattype, MPI_Datatype* viewtype)
{
    // TODO: we could cache these datatypes on Matrix object

    // initialize return params to known values
    *mattype  = MPI_DATATYPE_NULL;
    *viewtype = MPI_DATATYPE_NULL;

    // TODO: use TypeMap<>() and templating to figure this out
    MPI_Datatype type = DataTypeMPI;

    // get global width and height of matrix
    Int global_width  = M.Width();
    Int global_height = M.Height();

    // get local width and height, plus leading dimension of local matrix
    Int W    = M.LocalWidth();
    Int H    = M.LocalHeight();
    Int LDim = M.LDim();

    // create a datatype to describe libelemental data in memory,
    // data is stored in column-major order with a local height of H
    // and a local width of W, also the leading dimension LDim >= H
    // so there may be holes in our local buffer between consecutive
    // columns which we need to account for

    // first we have H consecutive elements in a column
    MPI_Datatype tmptype;
    MPI_Type_contiguous(H, type, &tmptype);

    // then there may be some holes at then end of our column,
    // since LDim >= H
    MPI_Datatype coltype;
    MPI_Aint extent = LDim * sizeof(DataType);
    MPI_Type_create_resized(tmptype, 0, extent, &coltype);
    MPI_Type_free(&tmptype);

    // finally we have W such columns
    MPI_Type_contiguous(W, coltype, mattype);
    MPI_Type_free(&coltype);
    MPI_Type_commit(mattype);

    // create datatype to desribe fileview for a collective IO operation
    // we will store matrix in column-major order in the file

    // get width and height of the process grid
    int rank    = M.Grid().Rank();
    int ranks   = M.Grid().Size();
    int pheight = M.Grid().Height();
    int pwidth  = M.Grid().Width();

    // TODO: need to account for alignment if user has set this

    // create_darray expects processes to be in row-major order,
    // find our global rank in row-major order
    int prow = M.Grid().Row();
    int pcol = M.Grid().Col();
    int row_major_rank = prow * pwidth + pcol;

    int gsizes[2];
    gsizes[0] = global_height;
    gsizes[1] = global_width;
    int distribs[2] = {MPI_DISTRIBUTE_CYCLIC, MPI_DISTRIBUTE_CYCLIC};
    // TODO: if using block sizes > 1, then change dargs below (BlockHeight, BlockWidth)
    int dargs[2] = {1, 1};
    int psizes[2];
    psizes[0] = pheight;
    psizes[1] = pwidth;
    MPI_Type_create_darray(ranks, row_major_rank, 2, gsizes, distribs, dargs, psizes, MPI_ORDER_FORTRAN, type, viewtype);
    MPI_Type_commit(viewtype);

    return;
}

/** \brief Write the given a distributed matrix to the specified file using MPI I/O */
void Write_MPI(const DistMatrix<DataType> &M, std::string basename = "DistMatrix", FileFormat format = BINARY, std::string title = "")
{
    // TODO: error out if format != BINARY

    // TODO: use TypeMap<>() and templating to figure this out
    MPI_Datatype type = DataTypeMPI;

    // define our file name
    string filename = basename + "." + FileExtension(BINARY);
    const char* path = filename.c_str();

    // get MPI communicator
    MPI_Comm comm = M.Grid().Comm().comm;

    // get our rank
    int rank = M.Grid().Rank();

    // first, delete the existing file
    if (rank == 0) {
        /*
        int unlink_rc = unlink(path);
        if (unlink_rc != 0) {
            fprintf(stderr, "Error deleting file `%s'\n", path);
            fflush(stderr);
        }
        */
        MPI_File_delete(path, MPI_INFO_NULL);
    }

    // get global width and height of matrix
    Int global_width  = M.Width();
    Int global_height = M.Height();

    // define datatypes to describe local buffer and view into file
    MPI_Datatype mattype, viewtype;
    create_types(M, &mattype, &viewtype);

    // define hints for creating the file (e.g., number of stripes on Lustre)
    MPI_Info info;
    MPI_Info_create(&info);
    MPI_Info_set(info, "striping_factor", "10");
    //MPI_Info_set(info, "striping_factor", "80");

    // open the file
    MPI_File fh;
    MPI_Status status;
    char datarep[] = "native";
    int amode = MPI_MODE_WRONLY | MPI_MODE_CREATE;
    MPI_File_open(comm, path, amode, info, &fh);

    // done with the info object
    MPI_Info_free(&info);

    // truncate file to 0 bytes
//    MPI_File_set_size(fh, 0);

    // set our view to write header (height and width as unsigned 32-bit ints)
    MPI_Offset disp = 0;
    MPI_File_set_view(fh, disp, MPI_UINT32_T, MPI_UINT32_T, datarep, MPI_INFO_NULL);
    if (rank == 0) {
        uint32_t dimensions[2];
        dimensions[0] = global_height;
        dimensions[1] = global_width;
        MPI_File_write_at(fh, 0, dimensions, 2, MPI_UINT32_T, &status);
    }
    disp += 2 * sizeof(uint32_t);

    // set view to write data
    MPI_File_set_view(fh, disp, type, viewtype, datarep, MPI_INFO_NULL);

    // write our portion of the matrix, since we set our view using create_darray,
    // all procs write at offset 0, the file view will take care of interleaving appropriately
    const char* buf = (const char*) M.LockedBuffer();
    MPI_File_write_at_all(fh, 0, buf, 1, mattype, &status);

    // close file
    MPI_File_close(&fh);

    // free our datatypes
    MPI_Type_free(&mattype);
    MPI_Type_free(&viewtype);

    return;
}

/** \brief Read the specified file and initialize a distributed matrix using MPI I/O */
void Read_MPI(DistMatrix<DataType> &M, std::string filename, FileFormat format = BINARY, bool sequential = false)
{
    // TODO: error out if format != BINARY

    // TODO: use TypeMap<>() and templating to figure this out
    MPI_Datatype type = DataTypeMPI;

    // define our file name
    const char* path = filename.c_str();

    // get MPI communicator
    MPI_Comm comm = M.Grid().Comm().comm;

    // get our rank
    int rank = M.Grid().Rank();

    // open the file
    MPI_File fh;
    MPI_Status status;
    char datarep[] = "native";
    int amode = MPI_MODE_RDONLY;
    int rc = MPI_File_open(comm, path, amode, MPI_INFO_NULL, &fh);
    if (rc != MPI_SUCCESS) {
        if (rank == 0) {
            cout << "Failed to open file `" << path << "'" << endl;
        }
        return;
    }

    // set displacement to beginning of file
    MPI_Offset disp = 0;

    // set our view to read header (height and width as unsigned 32-bit ints)
    uint32_t dimensions[2];
    MPI_File_set_view(fh, disp, MPI_UINT32_T, MPI_UINT32_T, datarep, MPI_INFO_NULL);
    if (rank == 0) {
        MPI_File_read_at(fh, 0, dimensions, 2, MPI_UINT32_T, &status);
    }
    disp += 2 * sizeof(uint32_t);

    // broadcast dimensions from rank 0
    MPI_Bcast(dimensions, 2, MPI_UINT32_T, 0, comm);

    // resize matrix to hold data
    Int global_height = dimensions[0];
    Int global_width  = dimensions[1];
    M.Resize(global_height, global_width);

    // now define datatypes to describe local buffer and view into file
    MPI_Datatype mattype, viewtype;
    create_types(M, &mattype, &viewtype);

    // set view to write data
    MPI_File_set_view(fh, disp, type, viewtype, datarep, MPI_INFO_NULL);

    // write our portion of the matrix, since we set our view using create_darray,
    // all procs write at offset 0, the file view will take care of interleaving appropriately
    char* buf = (char*) M.Buffer();
    MPI_File_read_at_all(fh, 0, buf, 1, mattype, &status);

    // close file
    MPI_File_close(&fh);

    // free our datatypes
    MPI_Type_free(&mattype);
    MPI_Type_free(&viewtype);

    return;
}

bool lbann::Layer::saveToFile(int fd, const char* dirname)
{
//    return writeDist(fd, filename, m_weights);
    char filepath[512];
    sprintf(filepath, "%s/weights_L%d_%03dx%03d", dirname, Index, m_weights->Height()-1, m_weights->Width()-1);
    if(m_weights->Grid().Rank() == 0) {
      cout << "Rank " << m_weights->Grid().Rank() << " saving layer " << Index << " to file " << filepath << endl;
    }
    Write(*m_weights, filepath, BINARY, "");
    //Write_MPI(m_weights, filepath, BINARY, "");
    return true;
}

bool lbann::Layer::loadFromFile(int fd, const char* dirname)
{
//   return readDist(fd, filename, m_weights);
    char filepath[512];
    sprintf(filepath, "%s/weights_L%d_%03dx%03d.bin", dirname, Index, m_weights->Height()-1, m_weights->Width()-1);
    struct stat buffer;
    Int restoreFileFound = 0;
    if (m_weights->Grid().Rank() == 0 && stat(filepath, &buffer) == 0) {
      restoreFileFound = 1;
    }
    mpi::Broadcast(&restoreFileFound, 1, 0, m_weights->Grid().Comm());

    if (restoreFileFound == 1) {
      if (m_weights->Grid().Rank() == 0) {
        cout << "Rank " << m_weights->Grid().Rank() << " restoring layer " << Index << " from file " << filepath << endl;
      }
      Read(*m_weights, filepath, BINARY, 1);
      //Read_MPI(m_weights, filepath, BINARY, 1);
      return true;
    } else {
      return false;
    }
}

bool lbann::Layer::saveToCheckpoint(int fd, const char* filename, uint64_t* bytes)
{
    writeDist(fd, filename, *m_weights, bytes);
    // Need to catch return value from function
    optimizer->saveToCheckpoint(fd, filename, bytes);
    return true;
}

bool lbann::Layer::loadFromCheckpoint(int fd, const char* filename, uint64_t* bytes)
{
    // TODO: implement reader for other matrix distributions
    readDist(fd, filename, (DistMat&) *m_weights, bytes);
    // Need to catch return value from function
    optimizer->loadFromCheckpoint(fd, filename, bytes);
    return true;
}

bool lbann::Layer::saveToCheckpointShared(const char* dir, uint64_t* bytes)
{
    int rank = m_weights->Grid().Rank();

    char path[512];
    sprintf(path, "%s/weights_L%d_%03dx%03d", dir, Index, m_weights->Height()-1, m_weights->Width()-1);
    if(rank == 0) {
      cout << "Saving layer " << Index << " to file " << path << endl;
    }
    Write(*m_weights, path, BINARY, "");
    //Write_MPI(m_weights, path, BINARY, "");

    if (rank == 0) {
        *bytes += 2 * sizeof(int) + m_weights->Height() * m_weights->Width() * sizeof(DataType);
    }

    optimizer->saveToCheckpointShared(dir, Index, bytes);

    return true;
}

bool lbann::Layer::loadFromCheckpointShared(const char* dir, uint64_t* bytes)
{
    int rank = m_weights->Grid().Rank();

    char path[512];
    sprintf(path, "%s/weights_L%d_%03dx%03d.bin", dir, Index, m_weights->Height()-1, m_weights->Width()-1);

    // check whether weights file exists
    int exists = lbann::exists(path);
    if (! exists) {
        return false;
    }

    // read weights file
    if (rank == 0) {
        cout << "Restoring layer " << Index << " from file " << path << endl;
    }
    Read(*m_weights, path, BINARY, 1);
    //Read_MPI(m_weights, path, BINARY, 1);

    if (rank == 0) {
        *bytes += 2 * sizeof(int) + m_weights->Height() * m_weights->Width() * sizeof(DataType);
    }

    optimizer->loadFromCheckpointShared(dir, Index, bytes);
    return true;
}

void lbann::Layer::fp_set_std_matrix_view() {
  int64_t cur_mini_batch_size = neural_network_model->get_current_mini_batch_size();

  if(m_prev_activations != NULL/* && m_prev_activations->Height() != 0 && m_prev_activations->Width() != 0*/) { // Input layers will not have a valid fp_input
    View(*m_prev_activations_v, *m_prev_activations, IR(0, m_prev_activations->Height()), IR(0, cur_mini_batch_size));
  }
  View(*m_weighted_sum_v, *m_weighted_sum, IR(0, m_weighted_sum->Height()), IR(0, cur_mini_batch_size));
  // Target layers will not have a valid bp_input
  if(m_prev_error_signal != NULL && m_prev_error_signal->Height() != 0 && m_prev_error_signal->Width() != 0) {
    View(*m_prev_error_signal_v, *m_prev_error_signal, IR(0, m_prev_error_signal->Height()), IR(0, cur_mini_batch_size));
  }
  View(*m_error_signal_v, *m_error_signal, IR(0, m_error_signal->Height()), IR(0, cur_mini_batch_size));
  View(*m_activations_v, *m_activations, IR(0, m_activations->Height()), IR(0, cur_mini_batch_size));

  // Update the layer's effective mini-batch size so it averages properly.
  if(cur_mini_batch_size != m_mini_batch_size) { /// When the current mini-batch is partial, check with the other models to figure out the entire size of the complete mini-batch
    int total_mini_batch_size = comm->intermodel_allreduce((int) cur_mini_batch_size);
    //    cout << "[" << comm->get_rank_in_world() << "] total_mini_batch_size " << total_mini_batch_size << " and cur mini batch size " << cur_mini_batch_size << endl;
    set_effective_minibatch_size(total_mini_batch_size);
  }else {
    set_effective_minibatch_size(cur_mini_batch_size * comm->get_num_models());
  }
}

#if 0
void lbann::Layer::bp_set_std_matrix_view() {
  int64_t cur_mini_batch_size = neural_network_model->get_current_mini_batch_size();

  if(m_prev_activations != NULL) { // Input layers will not have a valid fp_input
    View(*m_prev_activations_v, *m_prev_activations, IR(0, m_prev_activations->Height()), IR(0, cur_mini_batch_size));
  }
  View(*m_weighted_sum_v, *m_weighted_sum, IR(0, m_weighted_sum->Height()), IR(0, cur_mini_batch_size));
  if(m_prev_error_signal != NULL) { // Target layers will not have a valid bp_input
    View(*m_prev_error_signal_v, *m_prev_error_signal, IR(0, m_prev_error_signal->Height()), IR(0, cur_mini_batch_size));
  }
  View(*m_error_signal_v, *m_error_signal, IR(0, m_error_signal->Height()), IR(0, cur_mini_batch_size));
  View(*m_activations_v, *m_activations, IR(0, m_activations->Height()), IR(0, cur_mini_batch_size));

  // Update the layer's effective mini-batch size so it averages properly.
  if(cur_mini_batch_size != m_mini_batch_size) { /// When the current mini-batch is partial, check with the other models to figure out the entire size of the complete mini-batch
    int total_mini_batch_size = comm->intermodel_allreduce((int) cur_mini_batch_size);
    //    cout << "[" << comm->get_rank_in_world() << "] total_mini_batch_size " << total_mini_batch_size << " and cur mini batch size " << cur_mini_batch_size << endl;
    set_effective_minibatch_size(total_mini_batch_size);
  }else {
    set_effective_minibatch_size(cur_mini_batch_size * comm->get_num_models());
  }
}
#endif
void lbann::Layer::fp_nonlinearity() {
  // Forward propagation
  m_activation_fn->forwardProp(*m_activations_v);
}

void lbann::Layer::bp_nonlinearity() {
  // Backward propagation
  m_activation_fn->backwardProp(*m_weighted_sum_v);
  if (m_activation_type != activation_type::ID) {
    Hadamard(*m_prev_error_signal_v, *m_weighted_sum_v, *m_prev_error_signal_v);
  }
}
