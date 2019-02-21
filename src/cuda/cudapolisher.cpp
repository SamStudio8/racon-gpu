/*!
 * @file cudapolisher.cpp
 *
 * @brief CUDA Polisher class source file
 */

#include <future>
#include <iostream>

#include "sequence.hpp"
#include "cudapolisher.hpp"

#include "bioparser/bioparser.hpp"

#pragma message("TODO: Include logger/logger.hpp in cudapolisher.cpp")

namespace racon {

CUDAPolisher::CUDAPolisher(std::unique_ptr<bioparser::Parser<Sequence>> sparser,
    std::unique_ptr<bioparser::Parser<Overlap>> oparser,
    std::unique_ptr<bioparser::Parser<Sequence>> tparser,
    PolisherType type, uint32_t window_length, double quality_threshold,
    double error_threshold, int8_t match, int8_t mismatch, int8_t gap,
    uint32_t num_threads)
        : Polisher(std::move(sparser), std::move(oparser), std::move(tparser),
                type, window_length, quality_threshold,
                error_threshold, match, mismatch, gap, num_threads)
{
    std::cout << "[CUDAPolisher] Constructed." << std::endl;

    const uint32_t MAX_WINDOWS = 1024;
    const uint32_t MAX_DEPTH_PER_WINDOW = 32;
    for(uint32_t i = 0; i < num_threads; i++)
    {
        batch_processors_.emplace_back(createCUDABatch(MAX_WINDOWS, MAX_DEPTH_PER_WINDOW));
    }
}

CUDAPolisher::~CUDAPolisher()
{
    // Empty destructor for now.
}

void CUDAPolisher::fillNextBatchOfWindows(uint32_t batch_id)
{
    batch_processors_.at(batch_id)->reset();

    // Use mutex to read the vector containing windows in a threadsafe manner.
    std::lock_guard<std::mutex> guard(mutex_windows_);

    while(next_window_index_ < windows_.size())
    {
        if (batch_processors_.at(batch_id)->addWindow(windows_.at(next_window_index_)))
        {
            next_window_index_++;
        }
        else
        {
            break;
        }
    }
}

bool CUDAPolisher::processBatch(uint32_t batch_id)
{
    bool result = true;
    while(result)
    {
        fillNextBatchOfWindows(batch_id);
        if (batch_processors_.at(batch_id)->hasWindows())
        {
            // Launch workload.
            result = batch_processors_.at(batch_id)->generateConsensus();
        }
        else
        {
            break;
        }
    }
    return result;
}

void CUDAPolisher::polish(std::vector<std::unique_ptr<Sequence>>& dst,
    bool drop_unpolished_sequences)
{
    // Dummy polish function.
    std::cout << "Starting CUDA polish" << std::endl;

    // Process each of the batches in a separate thread.
    std::vector<std::future<bool>> thread_futures;
    for(uint32_t i = 0; i < batch_processors_.size(); i++)
    {
        thread_futures.emplace_back(std::async(std::launch::async,
                                               &CUDAPolisher::processBatch,
                                               this,
                                               i)
                                   );
    }

    // Wait for threads to finish, and collect their results.
    for(uint32_t i = 0; i < thread_futures.size(); i++)
    {
        thread_futures.at(i).wait();
        if (!thread_futures.at(i).get())
        {
            std::cerr << "Batch " << i << " had issues \
                processing its windows." << std::endl;
        }
    }

    // Collect results from all windows into final output.
    std::string polished_data = "";
    for (uint64_t i = 0; i < windows_.size(); ++i) {
        polished_data += windows_[i]->consensus();

        if (i == windows_.size() - 1 || windows_[i + 1]->rank() == 0) {
            double polished_ratio = 1.0f;

            if (!drop_unpolished_sequences || polished_ratio > 0) {
                std::string tags = type_ == PolisherType::kF ? "r" : "";
                tags += " LN:i:" + std::to_string(polished_data.size());
                tags += " RC:i:" + std::to_string(targets_coverages_[windows_[i]->id()]);
                tags += " XC:f:" + std::to_string(polished_ratio);
                dst.emplace_back(createSequence(sequences_[windows_[i]->id()]->name() +
                    tags, polished_data));
            }

            polished_data.clear();
        }
        windows_[i].reset();
    }

    (void) dst;
    (void) drop_unpolished_sequences;
    std::cout << "[CUDAPolisher] Polished." << std::endl;
}

}
