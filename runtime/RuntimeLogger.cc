/* Copyright (c) 2016-2019 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#include <fcntl.h>
#include <iosfwd>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <stdlib.h>
#include <unistd.h>

#include "TimeTrace.h"
#include "Cycles.h"         /* Cycles::rdtsc() */
#include "RuntimeLogger.h"
#include "Config.h"
#include "Util.h"

namespace NanoLogInternal {

// Define the static members of RuntimeLogger here
__thread RuntimeLogger::StagingBuffer *RuntimeLogger::stagingBuffer = nullptr;
thread_local RuntimeLogger::StagingBufferDestroyer RuntimeLogger::sbc;
RuntimeLogger RuntimeLogger::nanoLogSingleton;

// RuntimeLogger constructor
RuntimeLogger::RuntimeLogger()
        : threadBuffers()
        , nextBufferId()
        , bufferMutex()
        , compressionThread()
        , hasOutstandingOperation(false)
        , compressionThreadShouldExit(false)
        , syncRequested(false)
        , condMutex()
        , workAdded()
        , hintQueueEmptied()
        , outputFd(-1)
        , aioCb()
        , compressingBuffer(nullptr)
        , outputDoubleBuffer(nullptr)
        , currentLogLevel(NOTICE)
        , cycleAtThreadStart(0)
        , metrics()
        , coreId(-1)
        , registrationMutex()
        , invocationSites()
        , nextInvocationIndexToBePersisted(0)
{
    const char *filename = NanoLogConfig::DEFAULT_LOG_FILE;
    outputFd = open(filename, NanoLogConfig::FILE_PARAMS, 0666);
    if (outputFd < 0) {
        fprintf(stderr, "NanoLog could not open the default file location "
                "for the log file (\"%s\").\r\n Please check the permissions "
                "or use NanoLog::setLogFile(const char* filename) to "
                "specify a different log file.\r\n", filename);
        std::exit(-1);
    }

    memset(&aioCb, 0, sizeof(aioCb));

    int err = posix_memalign(reinterpret_cast<void **>(&compressingBuffer),
                             512, NanoLogConfig::OUTPUT_BUFFER_SIZE);
    if (err) {
        perror("The NanoLog system was not able to allocate enough memory "
                       "to support its operations. Quitting...\r\n");
        std::exit(-1);
    }

    err = posix_memalign(reinterpret_cast<void **>(&outputDoubleBuffer),
                         512, NanoLogConfig::OUTPUT_BUFFER_SIZE);
    if (err) {
        perror("The NanoLog system was not able to allocate enough memory "
                       "to support its operations. Quitting...\r\n");
        std::exit(-1);
    }

#ifndef BENCHMARK_DISCARD_ENTRIES_AT_STAGINGBUFFER
    compressionThread = std::thread(&RuntimeLogger::compressionThreadMain, this);
#endif
}

// RuntimeLogger destructor
RuntimeLogger::~RuntimeLogger() {
    sync();

    // Stop the compression thread
    {
        std::lock_guard<std::mutex> lock(nanoLogSingleton.condMutex);
        nanoLogSingleton.compressionThreadShouldExit = true;
        nanoLogSingleton.workAdded.notify_all();
    }

    if (nanoLogSingleton.compressionThread.joinable())
        nanoLogSingleton.compressionThread.join();

    // Free all the data structures
    if (compressingBuffer) {
        free(compressingBuffer);
        compressingBuffer = nullptr;
    }

    if (outputDoubleBuffer) {
        free(outputDoubleBuffer);
        outputDoubleBuffer = nullptr;
    }

    if (outputFd > 0)
        close(outputFd);

    outputFd = 0;
}

// Documentation in NanoLog.h
std::string
RuntimeLogger::getStats() {
    std::ostringstream out;
    char buffer[1024];

    // Leaks abstraction, but basically flush so we get all the I/O time
    uint64_t start = PerfUtils::Cycles::rdtsc();
    fdatasync(nanoLogSingleton.outputFd);
    uint64_t stop = PerfUtils::Cycles::rdtsc();
    nanoLogSingleton.metrics.cyclesDiskIO_upperBound += (stop - start);

    uint64_t cyclesAtBgThreadStart = nanoLogSingleton.cycleAtThreadStart;
    Metrics m = nanoLogSingleton.metrics;

    double outputTime = PerfUtils::Cycles::toSeconds(m.cyclesDiskIO_upperBound);
    double compressS = PerfUtils::Cycles::toSeconds(
                                                m.cyclesCompressingOnly);
    double compressingWithConsume = PerfUtils::Cycles::toSeconds(
                                                m.cyclesCompressingWithConsume);
    double compressPlusLockS = PerfUtils::Cycles::toSeconds(
                                                m.cyclesCompressAndLock);
    double scanAndCompressS = PerfUtils::Cycles::toSeconds(
                                                m.cyclesScanningAndCompressing);

    double bytesWritten = static_cast<double>(m.totalBytesWritten);
    double bytesRead = static_cast<double>(m.totalBytesRead);
    double padBytesWritten = static_cast<double>(m.padBytesWritten);
    double numEventsProcessedDouble = static_cast<double>(m.logsProcessed);

    snprintf(buffer, 1024,
               "\r\nWrote %lu events (%0.2lf MB) in %0.3lf seconds "
                   "(%0.3lf seconds spent compressing)\r\n",
             m.logsProcessed,
             bytesWritten / 1.0e6,
             outputTime,
             compressPlusLockS);
    out << buffer;

    snprintf(buffer, 1024,
           "There were %u file flushes and the final sync time was %lf sec\r\n",
           m.numAioWritesCompleted,
           PerfUtils::Cycles::toSeconds(stop - start));
    out << buffer;

    double secondsAwake =
            PerfUtils::Cycles::toSeconds(m.cyclesActive);
    double totalTime = PerfUtils::Cycles::toSeconds(
            PerfUtils::Cycles::rdtsc() - cyclesAtBgThreadStart);
    snprintf(buffer, 1024,
               "Compression Thread was active for %0.3lf out of %0.3lf seconds "
                   "(%0.2lf %%)\r\n",
               secondsAwake,
             totalTime,
               100.0 * secondsAwake / totalTime);
    out << buffer;

    snprintf(buffer, 1024,
                "On average, that's\r\n\t%0.2lf MB/s or "
                    "%0.2lf ns/byte w/ processing\r\n",
             (bytesWritten / 1.0e6) / (totalTime),
             (totalTime * 1.0e9) / bytesWritten);
    out << buffer;

    // Since we sleep at 1µs intervals and check for completion at wake up,
    // it's possible the IO finished before we woke-up, thus enlarging the time.
    snprintf(buffer, 1024,
                "\t%0.2lf MB/s or %0.2lf ns/byte disk throughput (min)\r\n",
             (bytesWritten / 1.0e6) / outputTime,
             (outputTime * 1.0e9) / bytesWritten);
    out << buffer;

    snprintf(buffer, 1024,
             "\t%0.2lf MB per flush with %0.1lf bytes/event\r\n",
             (bytesWritten / 1.0e6) / m.numAioWritesCompleted,
             bytesWritten * 1.0 / numEventsProcessedDouble);
    out << buffer;

    snprintf(buffer, 1024,
                "\t%0.2lf ns/event compress only\r\n"
                "\t%0.2lf ns/event compressing with consume\r\n"
                "\t%0.2lf ns/event compressing with locking\r\n"
                "\t%0.2lf ns/event scan+compress\r\n"
                "\t%0.2lf ns/event I/O time\r\n"
                "\t%0.2lf ns/event in total\r\n",
                compressS * 1.0e9 / numEventsProcessedDouble,
                compressingWithConsume * 1.0e9 / numEventsProcessedDouble,
                compressPlusLockS * 1.0e9 / numEventsProcessedDouble,
                scanAndCompressS * 1.0e9 / numEventsProcessedDouble,
                outputTime * 1.0e9 / double(m.totalMgsWritten),
                totalTime * 1.0e9 / numEventsProcessedDouble);
    out << buffer;

    snprintf(buffer, 1024, "The compression ratio was %0.2lf-%0.2lfx "
                   "(%lu bytes in, %lu bytes out, %lu pad bytes)\n",
                    1.0 * bytesRead / (bytesWritten + padBytesWritten),
                    1.0 * bytesRead / bytesWritten,
                    m.totalBytesRead,
                    m.totalBytesWritten,
                    m.padBytesWritten);
    out << buffer;

    return out.str();
}

/**
 * Returns a string detailing the distribution of how long vs. how many times
 * the log producers had to wait for free space and how big vs. how many times
 * the consumer (background thread) read.
 *
 * Note: The distribution stats for the producer must be enabled via
 * -DRECORD_PRODUCER_STATS during compilation, otherwise only the consumer
 * stats will be printed.
 */
std::string
RuntimeLogger::getHistograms()
{
    std::ostringstream out;
    char buffer[1024];

    snprintf(buffer, 1024, "Distribution of StagingBuffer.peek() sizes\r\n");
    out << buffer;
    size_t numIntervals =
            Util::arraySize(nanoLogSingleton.metrics.stagingBufferPeekDist);
    for (size_t i = 0; i < numIntervals; ++i) {
        snprintf(buffer, 1024
                , "\t%02lu - %02lu%%: %lu\r\n"
                , i*100/numIntervals
                , (i+1)*100/numIntervals
                , nanoLogSingleton.metrics.stagingBufferPeekDist[i]);
        out << buffer;
    }

    {
        std::unique_lock<std::mutex> lock(nanoLogSingleton.bufferMutex);
        for (size_t i = 0; i < nanoLogSingleton.threadBuffers.size(); ++i) {
            StagingBuffer *sb = nanoLogSingleton.threadBuffers.at(i);
            if (sb) {
                snprintf(buffer, 1024, "Thread %u:\r\n", sb->getId());
                out << buffer;

                snprintf(buffer, 1024,
                                 "\tAllocations   : %lu\r\n"
                                 "\tTimes Blocked : %u\r\n",
                         sb->numAllocations,
                         sb->numTimesProducerBlocked);
                out << buffer;

#ifdef RECORD_PRODUCER_STATS
                uint64_t averageBlockNs = PerfUtils::Cycles::toNanoseconds(
                        sb->cyclesProducerBlocked)/sb->numTimesProducerBlocked;
                snprintf(buffer, 1024,
                                 "\tAvgBlock (ns) : %lu\r\n"
                                 "\tBlock Dist\r\n",
                         averageBlockNs);
                for (size_t i = 0; i < Util::arraySize(
                        sb->cyclesProducerBlockedDist); ++i)
                {
                    snprintf(buffer, 1024
                            , "\t\t%4lu - %4lu ns: %u\r\n"
                            , i*10
                            , (i+1)*10
                            , sb->cyclesProducerBlockedDist[i]);
                    out << buffer;
                }
#endif
            }
        }
    }


#ifndef RECORD_PRODUCER_STATS
    out << "Note: Detailed Producer stats were compiled out. Enable "
            "via -DRECORD_PRODUCER_STATS";
#endif

    return out.str();
}

// See documentation in NanoLog.h
void
RuntimeLogger::preallocate() {
    nanoLogSingleton.ensureStagingBufferAllocated();
    // I wonder if it'll be a good idea to update minFreeSpace as well since
    // the user is already willing to invoke this up front cost.
}

/**
* Internal helper function to wait for AIO completion.
*/
void
RuntimeLogger::waitForAIO() {
    if (hasOutstandingOperation) {
        if (aio_error(&aioCb) == EINPROGRESS) {
            const struct aiocb *const aiocb_list[] = {&aioCb};
            int err = aio_suspend(aiocb_list, 1, NULL);

            if (err != 0)
                perror("LogCompressor's Posix AIO suspend operation failed");
        }

        int err = aio_error(&aioCb);
        ssize_t ret = aio_return(&aioCb);

        if (err != 0) {
            fprintf(stderr, "LogCompressor's POSIX AIO failed with %d: %s\r\n",
                    err, strerror(err));
        } else if (ret < 0) {
            perror("LogCompressor's Posix AIO Write operation failed");
        }
        ++metrics.numAioWritesCompleted;
        hasOutstandingOperation = false;
    }
}


/**
 * Metrics subtraction operator that takes the difference between all internal
 * member variables from two Metrics objects and effectively returns one
 * equivalent to (this - other).
 *
 * \param other
 *      Second Metrics struct to subtract by
 * \return
 *      Metrics struct containing the result of the subtraction
 */
RuntimeLogger::Metrics
RuntimeLogger::Metrics::operator-(const RuntimeLogger::Metrics &other)
{
    RuntimeLogger::Metrics result = *this;

    result.cyclesCompressingOnly -= other.cyclesCompressingOnly;
    result.cyclesCompressingWithConsume -= other.cyclesCompressingWithConsume;
    result.cyclesCompressAndLock -= other.cyclesCompressAndLock;
    result.cyclesScanningAndCompressing -= other.cyclesScanningAndCompressing;
    result.cyclesActive -= other.cyclesActive;
    result.cyclesSleeping_outOfWork -= other.cyclesSleeping_outOfWork;
    result.cyclesDiskIO_upperBound -= other.cyclesDiskIO_upperBound;
    result.numCompressBatches -= other.numCompressBatches;
    result.numCompressingAndLocks -= other.numCompressingAndLocks;
    result.numScansAndCompress -= other.numScansAndCompress;
    result.numSleeps_outOfWork -= other.numSleeps_outOfWork;
    result.totalBytesRead -= other.totalBytesRead;
    result.totalBytesWritten -= other.totalBytesWritten;
    result.logsProcessed -= other.logsProcessed;
    result.totalMgsWritten -= other.totalMgsWritten;
    result.padBytesWritten -= other.padBytesWritten;
    result.numAioWritesCompleted -= other.numAioWritesCompleted;

    for (uint64_t i = 0; i < Util::arraySize(stagingBufferPeekDist); ++i)
        result.stagingBufferPeekDist[i] -= other.stagingBufferPeekDist[i];

    return result;
}

/**
* Main compression thread that handles scanning through the StagingBuffers,
* compressing log entries, and outputting a compressed log file.
*/
void
RuntimeLogger::compressionThreadMain() {
    // Index of the last StagingBuffer checked for uncompressed log messages
    size_t lastStagingBufferChecked = 0;

    // Marks when the thread wakes up. This value should be used to calculate
    // the number of cyclesActive right before blocking/sleeping and then
    // updated to the latest rdtsc() when the thread re-awakens.
    uint64_t cyclesAwakeStart = PerfUtils::Cycles::rdtsc();
    cycleAtThreadStart = cyclesAwakeStart;

    // Manages the state associated with compressing log messages
    Log::Encoder encoder(compressingBuffer, NanoLogConfig::OUTPUT_BUFFER_SIZE);

    // Indicates whether a compression operation failed or not due
    // to insufficient space in the outputBuffer
    bool outputBufferFull = false;

    // Indicates that in scanning the StagingBuffers, we have passed the
    // zero-th index, but have not yet encoded that bit in the compressed output
    bool wrapAround = false;

    // Keeps a shadow mapping of the log identifiers to static information
    // to allow the logging threads to register in parallel with compression
    // lookup without locking
    std::vector<StaticLogInfo> shadowStaticInfo;

    // Marks when the last I/O started; used to calculate bandwidth
    uint64_t lastIOStartedTimestamp = 0;

#ifdef PRINT_BG_OPERATIONS
    // Gathers various metrics
    Metrics lastMetrics = metrics;

    // Timestamp where lastMetrics was taken
    uint64_t timestampOfLastMetrics = cyclesAwakeStart;
#endif

    PerfUtils::TimeTrace::record("Compression Thread Started");

    // Each iteration of this loop scans for uncompressed log messages in the
    // thread buffers, compresses as much as possible, and outputs it to a file.
    while (!compressionThreadShouldExit) {
        coreId = sched_getcpu();

        // Indicates how many bytes we have consumed from the StagingBuffers
        // in a single iteration of the while above. A value of 0 means we
        // were unable to consume anymore data any of the stagingBuffers
        // (either due to empty stagingBuffers or a full output encoder)
        uint64_t bytesConsumedThisIteration = 0;

        uint64_t start = PerfUtils::Cycles::rdtsc();
        // Step 1: Find buffers with entries and compress them
        {
            std::unique_lock<std::mutex> lock(bufferMutex);
            size_t i = lastStagingBufferChecked;

            // Output new dictionary entries, if necessary
            if (nextInvocationIndexToBePersisted < invocationSites.size())
            {
                std::unique_lock<std::mutex> lock (registrationMutex);
                encoder.encodeNewDictionaryEntries(
                                               nextInvocationIndexToBePersisted,
                                               invocationSites);

                // update our shadow copy
                for (uint64_t i = shadowStaticInfo.size();
                                    i < nextInvocationIndexToBePersisted; ++i)
                {
                    shadowStaticInfo.push_back(invocationSites.at(i));
                }
            }

            // Scan through the threadBuffers looking for log messages to
            // compress while the output buffer is not full.
            while (!compressionThreadShouldExit
                   && !outputBufferFull
                   && !threadBuffers.empty()) {
                uint64_t peekBytes = 0;
                StagingBuffer *sb = threadBuffers[i];
                char *peekPosition = sb->peek(&peekBytes);

                // If there's work, unlock to perform it
                if (peekBytes > 0) {
                    uint64_t peekStart = PerfUtils::Cycles::rdtsc();
                    PerfUtils::TimeTrace::record(peekStart,
                            "Peak Bytes was %d", int(peekBytes));
                    lock.unlock();

#ifdef RECORD_PRODUCER_STATS
                    // Record metrics on the peek size
                    size_t sizeOfDist =
                            Util::arraySize(metrics.stagingBufferPeekDist);
                    size_t distIndex = (sizeOfDist*peekBytes)/
                                            NanoLogConfig::STAGING_BUFFER_SIZE;
                    ++(metrics.stagingBufferPeekDist[distIndex]);
#endif

                    // Encode the data in RELEASE_THRESHOLD chunks
                    uint32_t remaining = downCast<uint32_t>(peekBytes);
                    while (remaining > 0) {
                        long bytesToEncode = std::min(
                                NanoLogConfig::RELEASE_THRESHOLD,
                                remaining);

                        uint64_t startCompressOnly = PerfUtils::Cycles::rdtsc();
#ifdef PREPROCESSOR_NANOLOG
                        long bytesRead = encoder.encodeLogMsgs(
                                peekPosition + (peekBytes - remaining),
                                bytesToEncode,
                                sb->getId(),
                                wrapAround,
                                &metrics.logsProcessed);
#else
                        long bytesRead = encoder.encodeLogMsgs(
                                peekPosition + (peekBytes - remaining),
                                bytesToEncode,
                                sb->getId(),
                                wrapAround,
                                shadowStaticInfo,
                                &metrics.logsProcessed);
#endif
                        metrics.cyclesCompressingOnly +=
                                PerfUtils::Cycles::rdtsc() - startCompressOnly;
                        metrics.numCompressBatches++;

                        if (bytesRead == 0) {
                            lastStagingBufferChecked = i;
                            outputBufferFull = true;
                            break;
                        }

                        wrapAround = false;
                        remaining -= downCast<uint32_t>(bytesRead);
                        sb->consume(bytesRead);
                        metrics.totalBytesRead += bytesRead;
                        bytesConsumedThisIteration += bytesRead;
                        metrics.cyclesCompressingWithConsume +=
                                 PerfUtils::Cycles::rdtsc() - startCompressOnly;
                    }

                    lock.lock();
                    metrics.numCompressingAndLocks++;
                    metrics.cyclesCompressAndLock
                                      += PerfUtils::Cycles::rdtsc() - peekStart;
                } else {
                    // If there's no work, check if we're supposed to delete
                    // the stagingBuffer
                    if (sb->checkCanDelete()) {
                        delete sb;

                        threadBuffers.erase(threadBuffers.begin() + i);
                        if (threadBuffers.empty()) {
                            lastStagingBufferChecked = i = 0;
                            wrapAround = true;
                            break;
                        }

                        // Back up the indexes so that we ensure we wont skip
                        // a buffer in our pass (and it's okay to redo one)
                        if (lastStagingBufferChecked >= i &&
                            lastStagingBufferChecked > 0) {
                            --lastStagingBufferChecked;
                        }
                        --i;
                    }
                }

                i = (i + 1) % threadBuffers.size();

                if (i == 0)
                    wrapAround = true;

                // Completed a full pass through the buffers
                if (i == lastStagingBufferChecked)
                    break;
            }

            metrics.cyclesScanningAndCompressing +=
                                             PerfUtils::Cycles::rdtsc() - start;
            metrics.numScansAndCompress++;
        }

        // If there's no data to output, go to sleep.
        if (encoder.getEncodedBytes() == 0) {
            std::unique_lock<std::mutex> lock(condMutex);

            // If a sync was requested, we should make at least 1 more
            // pass to make sure we got everything up to the sync point.
            if (syncRequested) {
                syncRequested = false;
                continue;
            }

            metrics.cyclesActive +=
                        PerfUtils::Cycles::rdtsc() - cyclesAwakeStart;

            hintQueueEmptied.notify_one();
            workAdded.wait_for(lock, std::chrono::microseconds(
                    NanoLogConfig::POLL_INTERVAL_NO_WORK_US));

            cyclesAwakeStart = PerfUtils::Cycles::rdtsc();
            continue;
        }

        if (hasOutstandingOperation) {
            if (aio_error(&aioCb) == EINPROGRESS) {
                const struct aiocb *const aiocb_list[] = {&aioCb};
                if (outputBufferFull) {
                    // If the output buffer is full and we're not done,
                    // wait for completion
                    PerfUtils::TimeTrace::record("Going to sleep (buffer full)");
                    uint64_t sleepStart = PerfUtils::Cycles::rdtsc();
                    metrics.cyclesActive += sleepStart - cyclesAwakeStart;
                    int err = aio_suspend(aiocb_list, 1, NULL);
                    uint64_t sleepEnd = PerfUtils::Cycles::rdtsc();
                    cyclesAwakeStart = sleepEnd;
                    PerfUtils::TimeTrace::record("Wakeup from sleep");
#ifdef PRINT_BG_OPERATIONS
                    printf("Fell asleep for %0.2lf ns\r\n",
                           1.0e9*PerfUtils::Cycles::toSeconds(
                                   sleepEnd - sleepStart));
#endif
                    if (err != 0)
                        perror("LogCompressor's Posix AIO "
                                       "suspend operation failed");
                } else {
                    // If not a lot of data was consumed, then go to sleep for
                    // a short while to avoid incurring additional/unnecessary
                    // cache misses for the producer.
                    using namespace NanoLogConfig;
                    if (bytesConsumedThisIteration <= LOW_WORK_THRESHOLD
                        && POLL_INTERVAL_DURING_LOW_WORK_US > 0)
                    {
                        std::unique_lock<std::mutex> lock(condMutex);
                        uint64_t sleepStart = PerfUtils::Cycles::rdtsc();
                        metrics.cyclesActive += sleepStart - cyclesAwakeStart;
                        workAdded.wait_for(lock, std::chrono::microseconds(
                                NanoLogConfig::POLL_INTERVAL_DURING_LOW_WORK_US));
                        uint64_t sleepEnd = PerfUtils::Cycles::rdtsc();
                        cyclesAwakeStart = sleepEnd;
                        metrics.cyclesSleeping_outOfWork += sleepEnd - sleepStart;
                        ++metrics.numSleeps_outOfWork;
#ifdef PRINT_BG_OPERATIONS
                        printf("Outta Work sleep for %0.2lf ns\r\n",
                                1.0e9*PerfUtils::Cycles::toSeconds(
                                                      sleepEnd - sleepStart));
#endif
                    }

                    if (aio_error(&aioCb) == EINPROGRESS)
                        continue;
                }
            }

            // Finishing up the IO
            int err = aio_error(&aioCb);
            ssize_t ret = aio_return(&aioCb);
            metrics.cyclesDiskIO_upperBound += PerfUtils::Cycles::rdtsc()
                                                    - lastIOStartedTimestamp;
            PerfUtils::TimeTrace::record("IO Complete");


            if (err != 0) {
                fprintf(stderr, "LogCompressor's POSIX AIO failed"
                        " with %d: %s\r\n", err, strerror(err));
            } else if (ret < 0) {
                perror("LogCompressor's Posix AIO Write failed");
            }
            ++metrics.numAioWritesCompleted;
            hasOutstandingOperation = false;

#ifdef PRINT_BG_OPERATIONS
            // Gather and print metrics after an I/O operation is completed.
            uint64_t now = PerfUtils::Cycles::rdtsc();
            uint64_t extraActiveTime = now - cyclesAwakeStart;
            uint64_t timestampOfNewMetrics = now;

            Metrics newMetrics = metrics;
            Metrics diff = newMetrics - lastMetrics;

            double elapsedS = PerfUtils::Cycles::toSeconds(
                                            now - timestampOfLastMetrics);
            double compressOnlyS = PerfUtils::Cycles::toSeconds(
                                        diff.cyclesCompressingOnly);
            double compressingAndLockingS = PerfUtils::Cycles::toSeconds(
                                        diff.cyclesCompressAndLock);
            double scanningAndCompressingS = PerfUtils::Cycles::toSeconds(
                                        diff.cyclesScanningAndCompressing);
            double bgActiveS = PerfUtils::Cycles::toSeconds(
                                        diff.cyclesActive + extraActiveTime);
            double ioS = PerfUtils::Cycles::toSeconds(
                                        diff.cyclesDiskIO_upperBound);
            double bgIdleS = (elapsedS - scanningAndCompressingS);
            double bytesCompressed = double(encoder.getEncodedBytes());
            double diskBW_MBps = 1e-6*(double(diff.totalBytesWritten))/ioS;
            double logMsgsCompressed = double(diff.logsProcessed);

            printf("At +%0.6lf seconds, compression thread compressed %lu "
                   "messages at %0.1lf bytes/msg\r\n It was active %0.2lf%% "
                   "of the time (%0.2lf us active; %0.2lf us idle).\r\n",
                   PerfUtils::Cycles::toSeconds(now - timestampOfLastMetrics),
                   diff.logsProcessed,
                   bytesCompressed/(logMsgsCompressed),
                   100.0*bgActiveS/elapsedS,
                   1.0e6*bgActiveS,
                   1.0e6*bgIdleS
            );

            // Stores metrics for the first logging thread only. These metrics
            // ARE SLOPPY, and are only intended for deep debugging with
            // a single logging thread.
            static uint32_t lastProducerBufferId = 0;
            static uint64_t lastProducerBlockedCycles = 0;
            static uint64_t lastProducerNumBlocks = 0;
            static uint64_t lastProducerNumAllocations = 0;

            StagingBuffer *sb = nullptr;
            {
                std::unique_lock<std::mutex> _(bufferMutex);
                if (threadBuffers.size() > 0)
                    sb = threadBuffers.at(0);
            }

            if (sb != nullptr && sb->id == lastProducerBufferId) {
                double producerBlockedS = PerfUtils::Cycles::toSeconds(
                         sb->cyclesProducerBlocked - lastProducerBlockedCycles);
                double estimatedRecordS = elapsedS - producerBlockedS;
                uint64_t numBlocks = sb->numTimesProducerBlocked
                                                - lastProducerNumBlocks;
                uint64_t numAllocations = sb->numAllocations
                                                - lastProducerNumAllocations;

                printf("Producer blocks %lu of %lu records "
                       "(%0.2lf%%) for an average length of %0.2lf ns.\r\n",
                       numBlocks,
                       numAllocations,
                       100.0*double(numBlocks)/double(numAllocations),
                       1e9*producerBlockedS/double(numBlocks)
                );

                printf("\t%6.2lf* ns/log or %6.2lf Mlog/s Only Producer\r\n",
                       (1e9*estimatedRecordS)/double(numAllocations),
                       double(numAllocations)/(1e6*estimatedRecordS)
                );
            }

            if (sb != nullptr) {
                lastProducerBufferId = sb->id;
                lastProducerBlockedCycles = sb->cyclesProducerBlocked;
                lastProducerNumBlocks = sb->numTimesProducerBlocked;
                lastProducerNumAllocations = sb->numAllocations;
            }
            // End sloppy producer metrics

            printf("\t%6.2lf  ns/log or %6.2lf Mlog/s Compress Only\r\n",
                   1e9*compressOnlyS/double(logMsgsCompressed),
                   logMsgsCompressed / (1e6 * compressOnlyS)
            );
            printf("\t%6.2lf  ns/log or %6.2lf Mlog/s Compress w/ locks\r\n",
                   1e9 * compressingAndLockingS / logMsgsCompressed,
                   logMsgsCompressed / (1e6 * compressingAndLockingS)
            );
            printf("\t%6.2lf* ns/log or %6.2lf Mlog/s Compress w/ scan\r\n",
                   1e9 * scanningAndCompressingS / logMsgsCompressed,
                   logMsgsCompressed / (1e6 * scanningAndCompressingS)
            );

            printf("\t%6.2lf* ns/log or %6.2lf Mlog/s Compress (w/ all)\r\n",
                   1e9 * bgActiveS / logMsgsCompressed,
                   logMsgsCompressed / (1e6 * bgActiveS)
            );

            printf("\t%6.2lf  ns/log or %6.2lf Mlog/s at %0.2lfMB/s "
                   "Disk Bandwidth at %0.1lf bytes/msg\r\n",
                   1e9*ioS/double(diff.totalMgsWritten),
                   double(diff.totalMgsWritten)/(1e6*ioS),
                   diskBW_MBps,
                   double(diff.totalBytesWritten)/double(diff.totalMgsWritten)
            );

            printf("Last I/O was %0.3lf MBs\r\n", double(diff.totalBytesWritten)*1e-6);

            // Metrics marked with an asterisks makes the assumption that the
            // application code is logging flat out.
            // This means if additional code executes between log statements
            // or delays are inserted, then these values are not accurate.
            // Additionally, even if the application is running flat out,
            // the first/last output may not be accurate as the test ramps
            // up/down. Be warned.
            printf("* These may not be accurate (see comments in code)\r\n");

            lastMetrics = newMetrics;
            timestampOfLastMetrics = timestampOfNewMetrics;
#endif
        }

        // At this point, compressed items exist in the buffer and the double
        // buffer used for IO is now free. Pad the output (if necessary) and
        // output.
        ssize_t bytesToWrite = encoder.getEncodedBytes();
        if (NanoLogConfig::FILE_PARAMS & O_DIRECT) {
            ssize_t bytesOver = bytesToWrite % 512;

            if (bytesOver != 0) {
                memset(compressingBuffer, 0, 512 - bytesOver);
                bytesToWrite = bytesToWrite + 512 - bytesOver;
                metrics.padBytesWritten += (512 - bytesOver);
            }
        }

        aioCb.aio_fildes = outputFd;
        aioCb.aio_buf = compressingBuffer;
        aioCb.aio_nbytes = bytesToWrite;
        metrics.totalBytesWritten += bytesToWrite;
        metrics.totalMgsWritten = metrics.logsProcessed;

        lastIOStartedTimestamp = PerfUtils::Cycles::rdtsc();
        PerfUtils::TimeTrace::record("Issuing I/O Of size %u bytes",
                                        int(bytesToWrite));
#ifdef PRINT_BG_OPERATIONS
        printf("Issuing I/O Of size %0.3lf MB\r\n",
               int(bytesToWrite)/(1024.0*1024));
#endif

        if (aio_write(&aioCb) == -1)
            fprintf(stderr, "Error at aio_write(): %s\n", strerror(errno));

        hasOutstandingOperation = true;

        // Swap buffers
        encoder.swapBuffer(outputDoubleBuffer,
                           NanoLogConfig::OUTPUT_BUFFER_SIZE);
        std::swap(outputDoubleBuffer, compressingBuffer);
        outputBufferFull = false;
    }

    if (hasOutstandingOperation) {
        // Wait for any outstanding AIO to finish
        while (aio_error(&aioCb) == EINPROGRESS);
        int err = aio_error(&aioCb);
        ssize_t ret = aio_return(&aioCb);
        metrics.cyclesDiskIO_upperBound +=
                          (PerfUtils::Cycles::rdtsc() - lastIOStartedTimestamp);

        if (err != 0) {
            fprintf(stderr, "LogCompressor's POSIX AIO failed with %d: %s\r\n",
                    err, strerror(err));
        } else if (ret < 0) {
            perror("LogCompressor's Posix AIO Write operation failed");
        }
        ++metrics.numAioWritesCompleted;
        hasOutstandingOperation = false;
    }

    cycleAtThreadStart = 0;
}

// Documentation in NanoLog.h
void
RuntimeLogger::setLogFile_internal(const char *filename) {
    // Check if it exists and is readable/writeable
    if (access(filename, F_OK) == 0 && access(filename, R_OK | W_OK) != 0) {
        std::string err = "Unable to read/write from new log file: ";
        err.append(filename);
        throw std::ios_base::failure(err);
    }

    // Try to open the file
    int newFd = open(filename, NanoLogConfig::FILE_PARAMS, 0666);
    if (newFd < 0) {
        std::string err = "Unable to open file new log file: '";
        err.append(filename);
        err.append("': ");
        err.append(strerror(errno));
        throw std::ios_base::failure(err);
    }

    // Everything seems okay, stop the background thread and change files
    sync();

    // Stop the compression thread completely
    {
        std::lock_guard<std::mutex> lock(nanoLogSingleton.condMutex);
        compressionThreadShouldExit = true;
        workAdded.notify_all();
    }

    if (compressionThread.joinable())
        compressionThread.join();

    if (outputFd > 0)
        close(outputFd);
    outputFd = newFd;

    // Relaunch thread
    nextInvocationIndexToBePersisted = 0; // Reset the dictionary
    compressionThreadShouldExit = false;
#ifndef BENCHMARK_DISCARD_ENTRIES_AT_STAGINGBUFFER
    compressionThread = std::thread(&RuntimeLogger::compressionThreadMain, this);
#endif
}

/**
* Set where the NanoLog should output its compressed log. If a previous
* log file was specified, NanoLog will attempt to sync() the remaining log
* entries before swapping files. For best practices, the output file shall
* be set before the first invocation to log by the main thread as this
* function is *not* thread safe.
*
* By default, the NanoLog will output to /tmp/compressedLog
*
* \param filename
*      File for NanoLog to output the compress log
*
* \throw is_base::failure
*      if the file cannot be opened or crated
*/
void
RuntimeLogger::setLogFile(const char *filename) {
    nanoLogSingleton.setLogFile_internal(filename);
}

/**
* Sets the minimum log level new NANO_LOG messages will have to meet before
* they are saved. Anything lower will be dropped.
*
* \param logLevel
*      LogLevel enum that specifies the minimum log level.
*/
void
RuntimeLogger::setLogLevel(LogLevel logLevel) {
    if (logLevel < 0)
        logLevel = static_cast<LogLevel>(0);
    else if (logLevel >= NUM_LOG_LEVELS)
        logLevel = static_cast<LogLevel>(NUM_LOG_LEVELS - 1);
    nanoLogSingleton.currentLogLevel = logLevel;
}

/**
* Blocks until the NanoLog system is able to persist to disk the
* pending log messages that occurred before this invocation. Note that this
* operation has similar behavior to a "non-quiescent checkpoint" in a
* database which means log messages occurring after this point this
* invocation may also be persisted in a multi-threaded system.
*/
void
RuntimeLogger::sync() {
#ifdef BENCHMARK_DISCARD_ENTRIES_AT_STAGINGBUFFER
    return;
#endif

    std::unique_lock<std::mutex> lock(nanoLogSingleton.condMutex);
    nanoLogSingleton.syncRequested = true;
    nanoLogSingleton.workAdded.notify_all();
    nanoLogSingleton.hintQueueEmptied.wait(lock);
}

/**
* Attempt to reserve contiguous space for the producer without making it
* visible to the consumer (See reserveProducerSpace).
*
* This is the slow path of reserveProducerSpace that checks for free space
* within storage[] that involves touching variable shared with the compression
* thread and thus causing potential cache-coherency delays.
*
* \param nbytes
*      Number of contiguous bytes to reserve.
*
* \param blocking
*      Test parameter that indicates that the function should
*      return with a nullptr rather than block when there's
*      not enough space.
*
* \return
*      A pointer into storage[] that can be written to by the producer for
*      at least nbytes.
*/
char *
RuntimeLogger::StagingBuffer::reserveSpaceInternal(size_t nbytes, bool blocking) {
    const char *endOfBuffer = storage + NanoLogConfig::STAGING_BUFFER_SIZE;

#ifdef RECORD_PRODUCER_STATS
#endif

    uint64_t start = PerfUtils::Cycles::rdtsc();

    // There's a subtle point here, all the checks for remaining
    // space are strictly < or >, not <= or => because if we allow
    // the record and print positions to overlap, we can't tell
    // if the buffer either completely full or completely empty.
    // Doing this check here ensures that == means completely empty.
    while (minFreeSpace <= nbytes) {
        // Since consumerPos can be updated in a different thread, we
        // save a consistent copy of it here to do calculations on
        char *cachedConsumerPos = consumerPos;

        if (cachedConsumerPos <= producerPos) {
            minFreeSpace = endOfBuffer - producerPos;

            if (minFreeSpace > nbytes)
                break;

            // Not enough space at the end of the buffer; wrap around
            endOfRecordedSpace = producerPos;

            // Prevent the roll over if it overlaps the two positions because
            // that would imply the buffer is completely empty when it's not.
            if (cachedConsumerPos != storage) {
                // prevents producerPos from updating before endOfRecordedSpace
                Fence::sfence();
                producerPos = storage;
                minFreeSpace = cachedConsumerPos - producerPos;
            }
        } else {
            minFreeSpace = cachedConsumerPos - producerPos;
        }

#ifdef BENCHMARK_DISCARD_ENTRIES_AT_STAGINGBUFFER
        // If we are discarding entries anwyay, just reset space to the head
        producerPos = storage;
        minFreeSpace = endOfBuffer - storage;
#endif

        // Needed to prevent infinite loops in tests
        if (!blocking && minFreeSpace <= nbytes)
            return nullptr;
    }


    uint64_t cyclesBlocked = PerfUtils::Cycles::rdtsc() - start;
    cyclesProducerBlocked += cyclesBlocked;
#ifdef RECORD_PRODUCER_STATS

    size_t maxIndex = Util::arraySize(cyclesProducerBlockedDist) - 1;
    size_t index = std::min(cyclesBlocked/cyclesIn10Ns, maxIndex);
    ++(cyclesProducerBlockedDist[index]);
#endif

    ++numTimesProducerBlocked;
    return producerPos;
}

/**
* Peek at the data available for consumption within the stagingBuffer.
* The consumer should also invoke consume() to release space back
* to the producer. This can and should be done piece-wise where a
* large peek can be consume()-ed in smaller pieces to prevent blocking
* the producer.
*
* \param[out] bytesAvailable
*      Number of bytes consumable
* \return
*      Pointer to the consumable space
*/
char *
RuntimeLogger::StagingBuffer::peek(uint64_t *bytesAvailable) {
    // Save a consistent copy of producerPos
    char *cachedProducerPos = producerPos;

    if (cachedProducerPos < consumerPos) {
        Fence::lfence(); // Prevent reading new producerPos but old endOf...
        *bytesAvailable = endOfRecordedSpace - consumerPos;

        if (*bytesAvailable > 0)
            return consumerPos;

        // Roll over
        consumerPos = storage;
    }

    *bytesAvailable = cachedProducerPos - consumerPos;
    return consumerPos;
}

}; // namespace NanoLog Internal