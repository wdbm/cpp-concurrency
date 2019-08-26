// This shows one possible solution to the TBB Graph
// exercise
//
// It is correct, but there are further optimisations
// that could be done - left as an exercise for group
// discussion and for the reader to implent

#include <algorithm>
#include <iostream>
#include <vector>
#include <array>
#include <tbb/tbb.h>


#include "fdet.hpp"

#define DEBUG 1

// Define our detector data vector type here
// This allows us to see what might go wrong is we were to use an std::vector
// (could it be corrected?)
using f_det_vec = tbb::concurrent_vector<f_det>;

// This is the "datatype" we shall use to gather the
// signals, which will be an array of vectors, the
// [x][y] array corresponds to the cell coordinate
// and the value is a concurrent vector of frame indexes
// where that cell saw a signal
// In this case it's really more convenient to use a struct
struct det_signal {
    std::array<tbb::concurrent_vector<size_t>, DETSIZE> count[DETSIZE];
};

// Read input data from a file
class frame_loader {
private:
  size_t m_frame_counter;
  std::ifstream& m_input_stream_p;
public:
  frame_loader(std::ifstream &ifs_p):
    m_frame_counter{0}, m_input_stream_p(ifs_p) {};

  bool operator() (f_det& fdet) {
    int read_err;
    read_err = fdet.read(m_input_stream_p);
    if (read_err) {
        return false;
    }
    if (DEBUG) {
        std::cout << "frame_loader loaded " << m_frame_counter << std::endl;
    }
    ++m_frame_counter;
    return true;
  }

  size_t frame_counter() {
    return m_frame_counter;
  }
};

// Aggregate frame data into the global data vector, return the index
// of the data item added
class add_frame_data {
private:
    f_det_vec& m_fdet_data;

public:
    add_frame_data(f_det_vec& fdet_data):
        m_fdet_data{fdet_data} {};

    size_t operator()(f_det fdet) {
        m_fdet_data.push_back(fdet);
        if (DEBUG) {
            std::cout << "Added new frame, average " << fdet.average() << std::endl;
            // std::cout << &m_fdet_data[0] << " " << &m_fdet_data[m_fdet_data.size()-1] << std::endl;
        }
        return m_fdet_data.size()-1;
    }
};

// Subtract the pedastal values from a frame
class subtract_pedastal {
private:
    f_det_vec& m_fdet_data;

public:
    subtract_pedastal(f_det_vec& fdet_data):
        m_fdet_data{fdet_data} {};

    size_t operator()(size_t t) {
        if (DEBUG) {
            std::cout << "Substracting pedastal for " << t << " " <<  &m_fdet_data[t] <<  std::endl;
        }
        tbb::parallel_for(tbb::blocked_range2d<size_t>(0, DETSIZE, 0, DETSIZE),
        [&, t](tbb::blocked_range2d<size_t>& r){
            for (size_t x=r.rows().begin(); x!=r.rows().end(); ++x) {
                for (size_t y=r.cols().begin(); y!=r.cols().end(); ++y) {
                    m_fdet_data[t].cells[x][y] -= pedastal(x, y);
                }
            }
        });
        return t;
    }
};


// Data quality masking of bad cells
class data_quality_mask {
private:
    f_det_vec& m_fdet_data;

public:
    data_quality_mask(f_det_vec& fdet_data):
        m_fdet_data{fdet_data} {};

    size_t operator()(size_t t) {
        if (DEBUG) {
            std::cout << "Applying DQ mask for " << t << " " << &m_fdet_data[t] << std::endl;
        }
        tbb::parallel_for(tbb::blocked_range2d<size_t>(0, DETSIZE, 0, DETSIZE),
        [&, t](tbb::blocked_range2d<size_t>& r){
            for (size_t x=r.rows().begin(); x!=r.rows().end(); ++x) {
                for (size_t y=r.cols().begin(); y!=r.cols().end(); ++y) {
                    // N.B. This could be parallelised with pedastal subtraction
                    // if we decide that value<0 indicates a bad cell (this means
                    // the race condition does not matter)
                    if (!cell_mask(x, y)) m_fdet_data[t].cells[x][y] = -1.0f;
                }
            }
        });
        return t;
    }
};


// Signal search
class signal_search {
private:
    f_det_vec& m_fdet_data;
    det_signal& m_fdet_signal;

public:
    signal_search(f_det_vec& fdet_data, det_signal& fdet_signal):
        m_fdet_data{fdet_data}, m_fdet_signal{fdet_signal} {};

    size_t operator()(size_t t) {
        if (DEBUG) {
            std::cout << "Signal search for " << t << " " << &m_fdet_data[t] << std::endl;
        }
        tbb::parallel_for(tbb::blocked_range2d<size_t>(0, DETSIZE, 0, DETSIZE),
        [&, t](tbb::blocked_range2d<size_t>& r){
            for (size_t x=r.rows().begin(); x!=r.rows().end(); ++x) {
                for (size_t y=r.cols().begin(); y!=r.cols().end(); ++y) {
                    float sum = 0.0f;
                    int count = 0;
                    for (auto dx=x-1; dx<x+2; ++dx) {
                        for (auto dy=y-1; dy<y+2; ++dy) {
                            if (dx>=0 && dx<DETSIZE && dy>=0 && dy<DETSIZE && 
                                m_fdet_data[t].cells[dx][dy]>0.0) {
                                ++count;
                                sum += m_fdet_data[t].cells[dx][dy];
                            }
                        }
                    }
                    if (sum > 150.0*count) {
                        if (DEBUG) {
                            std::cout << "Signal at (" << t << ", " << x << ", " << y << ")" << std::endl;
                        }
                        m_fdet_signal.count[x][y].push_back(t);
                    }
                }
            }
        });
        return t;
    }
};


// Fooble detector code
bool detect_fooble_in_cell(tbb::concurrent_vector<size_t> cell_signal) {
    return false;
}


int main(int argn, char* argv[]) {
    if (argn != 2) {
        std::cerr << "Usage: solution INPUT_FILE" << std::endl;
        return 1;
    }

    std::ifstream det_in(argv[1], std::ios::binary);
    if (!det_in.good()) {
        std::cerr << "Problem opening imput file" << std::endl;
        return 2;
    }

    // Setup a big vector where we will add all the data
    // Assume in this case it fits in memory!
    f_det_vec fdet_data;
    det_signal fdet_signal;

    // To make the graph nodes a bit easier define necessary
    // instances here
    frame_loader data_loader(det_in);
    add_frame_data frame_aggregator(fdet_data);
    subtract_pedastal sub_pedastal(fdet_data);
    data_quality_mask dq_cell_mask(fdet_data);
    signal_search sig_search(fdet_data, fdet_signal);

    tbb::flow::graph data_process;
    tbb::flow::source_node<f_det> loader(data_process, data_loader, false);
    tbb::flow::function_node<f_det, size_t> aggregate(data_process, 1, frame_aggregator);
    tbb::flow::function_node<size_t, size_t> pedastal(data_process, tbb::flow::unlimited, sub_pedastal);
    tbb::flow::function_node<size_t, size_t> mask(data_process, tbb::flow::unlimited, dq_cell_mask);
    tbb::flow::function_node<size_t, size_t> search(data_process, tbb::flow::unlimited, sig_search);

    tbb::flow::make_edge(loader, aggregate);
    tbb::flow::make_edge(aggregate, pedastal);
    tbb::flow::make_edge(pedastal, mask);
    tbb::flow::make_edge(mask, search);

    loader.activate();
    data_process.wait_for_all();

    // Now we found all of the signals, but we need to detect foobles
    // by looking at consecutive timeframes
    // We can't really do this with the above flow graph as it was processing
    // at the single timeframe granularity


    return 0;
}
